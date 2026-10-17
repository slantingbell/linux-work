#include <sys/types.h>
#include <stddef.h>
#include <errno.h>
#include <stdarg.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include "client_handler.h"
// 接收并解析一条客户端提示词
// 成功：返回提示词长度（不含 '\0'）
// 失败：返回 -1
ssize_t client_receive_request(
    int client_fd,  //客户端套接字
    char *prompt,   //存放提示词缓冲区
    size_t prompt_size //缓冲区大小
)
{
    if (client_fd < 0 || prompt == NULL || prompt_size == 0) { //检查合法
        errno = EINVAL; //EINVAL：无效参数
        return -1;
    }

    char incoming[2048]; //临时缓冲区server
    size_t used = 0;  //提示词缓冲区记录已经接收到的字节数

    while (1) {
        // 批量接收数据
        ssize_t n = recv(    //这玩意是覆盖写入  
            client_fd,
            incoming,
            sizeof(incoming),
            0
        );
        //异常退出
        if (n < 0) {
            if (errno == EINTR) {//EINTR：被信号中断，重新尝试
                continue;
            }
            return -1;
        }
        if (n == 0) {
            // 连接关闭，但尚未收到完整请求
            errno = ECONNRESET; //ECONNRESET：连接被重置
            return -1;
        }
        // 在本次实际接收的数据中查找结束标记
        char *end = memchr(incoming, '\0', (size_t)n);//返回'\0'在incoming中第一次出现的位置

        // 本次接收的数据中没有结束标记
        if (end == NULL) {
            size_t received = (size_t)n; //本次接收到的字节数

            // 判断提示词缓冲区剩余容量是否够用
            if (received > prompt_size - 1 - used) { 
                errno = EMSGSIZE; //EMSGSIZE：消息大小错误
                return -1;
            }
            memcpy(prompt + used, incoming, received);//把本轮接收到的提示词数据放入提示词缓冲区
            used += received; //更新提示词缓冲区中的信息数量
            continue;
        }
        //end != NULL
        // 结束标记之前的正文长度
        size_t body_len = (size_t)(end - incoming);

        // 检查完整请求能否放入 prompt
        if (body_len > prompt_size - 1 - used) {
            errno = EMSGSIZE;
            return -1;
        }

        // 只复制结束标记之前的正文
        memcpy(prompt + used, incoming, body_len);
        used += body_len;

        // 构造合法的 C 字符串
        prompt[used] = '\0';
        
        return (ssize_t)used;
    }
}

#define ERR_SNIPPET_CAP 192 // 保留的响应体片段上限：出错时后端写的原因就在这里

/**
 * 流式转发上下文：SSE 字节 -> 解析出 token -> 发给客户端
 */
struct stream_context {
    llm_stream_callback callback;//流式输出的回调函数
    void *userdata; 
    int callback_failed;
    /*
     * 响应体开头的一小段原始字节。
     * 推理成功时这里是无用的 SSE 前几个字节；推理失败时
     * llama-server 会把真正的失败原因写在这里（例如 "exceeds the context size"）。
     * 出错才取用，所以正常路径上没有额外开销。
     */
    char snippet[ERR_SNIPPET_CAP + 1];
    size_t snippet_len;
};

// 写失败原因；errbuf 为 NULL 时静默忽略
static void set_error(char *errbuf, size_t errcap, const char *fmt, ...)
{
    if (errbuf == NULL || errcap == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errcap, fmt, ap);
    va_end(ap);
}

// 取不超过 max 的字符串长度，且不把一个 UTF-8 字符切成两半
static size_t utf8_safe_len(const char *s, size_t len, size_t max)
{
    if (len <= max) {
        return len;
    }
    size_t n = max;
    // 回退到字符起始字节（续字节形如 10xxxxxx）
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}



/*
 * libcurl 每次收到响应数据时调用此函数。
 *
 * 注意：
 * 1. ptr 不保证以 '\0' 结尾。
 * 2. 一次回调可能只有部分 SSE 数据，也可能包含多个事件。
 * 3. 返回实际处理的字节数；返回 0 可中止传输。
 */
static size_t on_http_data(
    char *ptr,
    size_t size,
    size_t nmemb, 
    void *userdata
)
{
    struct stream_context *ctx = userdata;
    size_t len = size * nmemb;

    if (len == 0) {
        return 0;
    }

    // 留一段响应体原文，供失败时定位原因
    size_t room = ERR_SNIPPET_CAP - ctx->snippet_len;
    if (room > 0) {
        size_t take = len < room ? len : room;
        memcpy(ctx->snippet + ctx->snippet_len, ptr, take);
        ctx->snippet_len += take;
        ctx->snippet[ctx->snippet_len] = '\0';
    }

    if (ctx->callback != NULL) {
        if (ctx->callback(ptr, len, ctx->userdata) != 0) {
            ctx->callback_failed = 1;
            return 0; /* 中止 libcurl 传输 */
        }
    }

    return len;
}
/**
 * 生成流式响应
 * 把提示词发送给llama-server并接收流式响应
 * 成功返回 0；失败返回 -1，并把失败原因写入 errbuf（errbuf 可为 NULL 表示不关心）
 */
/*
 * 向 LLM 推理后端发送聊天请求，并以“流式”的方式接收响应。
 *
 * 整体流程：
 *
 *   1. 检查函数参数
 *   2. 拼接请求 URL
 *   3. 构建 JSON 请求体
 *   4. 初始化 libcurl
 *   5. 配置 HTTP POST 请求
 *   6. 设置流式响应回调 on_http_data()
 *   7. 调用 curl_easy_perform() 发送请求并持续接收数据
 *   8. 检查 callback 是否失败
 *   9. 检查 libcurl 是否出错
 *  10. 检查 HTTP 状态码
 *  11. 出错时使用 snippet 保存的响应片段辅助定位问题
 *  12. 统一释放资源
 *
 * 参数：
 *
 * base_url
 *     LLM 后端的基础 URL，例如：
 *     "http://127.0.0.1:8080"
 *
 * prompt
 *     当前用户最新发送的问题。
 *
 * history
 *     之前的聊天历史。
 *
 * history_count
 *     history 数组中有多少条历史消息。
 *
 * callback
 *     LLM 每收到一段流式输出后，最终交给哪个业务函数处理。
 *
 * userdata
 *     传递给 callback 的上下文指针。
 *     它可以指向客户端信息、请求上下文等。
 *
 * errbuf
 *     用来保存错误信息的缓冲区。
 *
 * errcap
 *     errbuf 的容量。
 */
int llm_generate_stream(
    const char *base_url,// LLM 后端的基础 URL，例如："http://127.0.0.1:8080"
    const char *prompt, //当前用户最新发送的问题
    const struct chat_message *history,//之前的聊天历史
    size_t history_count,//history 数组中有多少条历史消息
    llm_stream_callback callback,// LLM 每收到一段流式输出后，最终交给哪个业务函数处理
    void *userdata,//传递给 callback 的上下文指针
    char *errbuf,//用来保存错误信息的缓冲区
    size_t errcap//errbuf 的容量
)
{
    /*
     * CURL *
     *
     * libcurl 的 HTTP 请求对象。
     *
     * curl_easy_init() 成功后会得到一个 CURL 对象，
     * 后面的 curl_easy_setopt()、curl_easy_perform()
     * 都需要使用它。
     *
     * 初始化为 NULL 是为了后面的统一 cleanup。
     */
    CURL *curl = NULL;


    /*
     * curl_slist *
     *
     * 用来保存 HTTP 请求头。
     *
     * 例如：
     *
     * Content-Type: application/json
     * Accept: text/event-stream
     *
     * 初始化为 NULL，同样方便统一释放。
     */
    struct curl_slist *headers = NULL;


    /*
     * root：
     * 整个 JSON 请求对象。
     *
     * 最终大概会形成：
     *
     * {
     *     "messages": [
     *         {
     *             "role": "user",
     *             "content": "你好"
     *         }
     *     ],
     *     "stream": true
     * }
     */
    struct json_object *root = NULL;


    /*
     * messages：
     * JSON 中的 messages 数组。
     *
     * 例如：
     *
     * "messages": [
     *     {...},
     *     {...}
     * ]
     */
    struct json_object *messages = NULL;


    /*
     * message：
     * 表示一条具体的聊天消息。
     *
     * 例如：
     *
     * {
     *     "role": "user",
     *     "content": "你好"
     * }
     */
    struct json_object *message = NULL;


    /*
     * stream_context 是我们自己定义的流式处理上下文。
     *
     * 这里直接初始化：
     *
     * callback
     *     保存真正的业务回调函数。
     *
     * userdata
     *     保存调用者传进来的上下文。
     *
     * callback_failed
     *     记录业务 callback 是否失败。
     *
     * snippet_len
     *     当前 snippet 中保存了多少字节。
     *
     * 注意：
     *
     * 这里的 ctx 会作为 userdata 传给 libcurl。
     *
     * 即：
     *
     *     curl_easy_setopt(
     *         curl,
     *         CURLOPT_WRITEDATA,
     *         &ctx
     *     );
     *
     * libcurl 后面调用：
     *
     *     on_http_data(..., &ctx)
     *
     * 然后 on_http_data() 中：
     *
     *     struct stream_context *ctx = userdata;
     *
     * 就把这个上下文取回来。
     */
    struct stream_context ctx = {
        .callback = callback,
        .userdata = userdata,
        .callback_failed = 0,
        .snippet_len = 0
    };


    /*
     * curl_rc：
     *
     * 保存 curl_easy_perform() 的返回结果。
     *
     * CURLE_OK 表示 libcurl 层面执行成功。
     */
    CURLcode curl_rc;


    /*
     * HTTP 状态码。
     *
     * 例如：
     *
     * 200 → 请求成功
     * 400 → 请求错误
     * 404 → 找不到资源
     * 500 → 后端服务器错误
     */
    long http_status = 0;


    /*
     * 整个函数最终的返回值。
     *
     * 默认：
     *
     * -1 → 失败
     *
     * 只有所有步骤成功后才设置：
     *
     * result = 0
     */
    int result = -1;


    /*
     * 用来保存最终请求 URL。
     *
     * 例如：
     *
     * base_url = "http://127.0.0.1:8080"
     *
     * 最终：
     *
     * http://127.0.0.1:8080/v1/chat/completions
     */
    char url[1024];


    /*
     * ============================================================
     * 1. 参数合法性检查
     * ============================================================
     *
     * 检查：
     *
     * base_url == NULL
     *     → 没有后端地址
     *
     * prompt == NULL
     *     → 没有用户问题
     *
     * history_count > 0 && history == NULL
     *     → 明明说有历史消息，但 history 指针却是 NULL
     *
     * callback == NULL
     *     → 没有提供流式数据处理函数
     *
     * 任意一个成立，都无法正常执行。
     */
    if (base_url == NULL || prompt == NULL ||
        (history_count > 0 && history == NULL) || callback == NULL) {

        /*
         * 给程序员看的错误信息。
         *
         * stderr 是标准错误输出。
         */
        fprintf(stderr, "Invalid arguments\n");


        /*
         * 给上层调用者保存错误信息。
         *
         * set_error() 内部最终会写入 errbuf。
         */
        set_error(errbuf, errcap, "内部错误：参数非法");


        /*
         * 参数本身就不合法，所以直接失败返回。
         *
         * -1 表示失败。
         */
        return -1;
    }


    /*
     * ============================================================
     * 2. 构造请求 URL
     * ============================================================
     *
     * snprintf() 会按照指定格式把字符串写入 url。
     *
     * 例如：
     *
     * base_url：
     *     http://127.0.0.1:8080
     *
     * 拼接后：
     *
     *     http://127.0.0.1:8080/v1/chat/completions
     *
     * sizeof(url)：
     *     告诉 snprintf() 最多允许写多少字节，
     *     防止数组越界。
     */
    int n = snprintf(
        url,
        sizeof(url),
        "%s/v1/chat/completions",
        base_url
    );


    /*
     * snprintf() 返回“本来需要写多少字符”。
     *
     * 如果：
     *
     * n < 0
     *
     * 表示格式化失败。
     *
     * 如果：
     *
     * (size_t)n >= sizeof(url)
     *
     * 表示 URL 太长，url 数组装不下。
     *
     * 注意：
     *
     * snprintf() 在这种情况下可能已经进行了截断，
     * 所以不能继续使用这个 URL。
     */
    if (n < 0 || (size_t)n >= sizeof(url)) {

        fprintf(stderr, "URL is too long\n");

        set_error(errbuf, errcap, "内部错误：后端 URL 过长");

        return -1;
    }


    /*
     * ============================================================
     * 3. 构建 JSON 请求体
     * ============================================================
     */


    /*
     * 创建一个 JSON 对象。
     *
     * 相当于准备：
     *
     * {
     *     ...
     * }
     */
    root = json_object_new_object();


    /*
     * 创建 JSON 数组。
     *
     * 准备：
     *
     * "messages": [
     *     ...
     * ]
     */
    messages = json_object_new_array();


    /*
     * 创建一个 JSON message 对象。
     *
     * 后面循环中实际上还会重新创建 message。
     *
     * 这里的初始化主要是为了保证 cleanup 时指针状态明确。
     */
    message = json_object_new_object();


    /*
     * 检查 JSON 对象是否创建成功。
     *
     * 如果内存不足，json-c 可能返回 NULL。
     *
     * 只要任何一个对象创建失败，就不能继续构建请求。
     */
    if (root == NULL || messages == NULL || message == NULL) {

        set_error(
            errbuf,
            errcap,
            "内部错误：构建请求体失败（内存不足）"
        );

        /*
         * 不直接 return。
         *
         * 而是跳到统一 cleanup。
         *
         * 这样已经成功创建的 JSON 对象可以统一释放。
         */
        goto cleanup;
    }


    /*
     * ============================================================
     * 构建 messages 数组
     * ============================================================
     *
     * 循环次数：
     *
     * history_count + 1
     *
     * 因为：
     *
     * 前 history_count 条：
     *     是历史消息
     *
     * 最后一条：
     *     是当前 prompt
     *
     * 例如：
     *
     * history_count = 2
     *
     * 那么 i：
     *
     * 0 → history[0]
     * 1 → history[1]
     * 2 → 当前 prompt
     */
    for (size_t i = 0; i <= history_count; i++) {


        /*
         * 如果 i < history_count：
         *
         *     当前处理的是历史消息
         *
         * 否则：
         *
         *     当前处理的是最新的用户 prompt。
         *
         * 三目运算符：
         *
         *     条件 ? 条件成立的值 : 条件不成立的值
         */
        const char *role =
            i < history_count
                ? history[i].role
                : "user";


        /*
         * 同理：
         *
         * 历史消息：
         *
         *     history[i].content
         *
         * 当前用户问题：
         *
         *     prompt
         */
        const char *content =
            i < history_count
                ? history[i].content
                : prompt;


        /*
         * 为当前这一条消息创建一个 JSON 对象。
         *
         * 例如：
         *
         * {
         *     "role": "user",
         *     "content": "你好"
         * }
         */
        message = json_object_new_object();


        /*
         * 下面一次检查多个条件。
         *
         * 1. message == NULL
         *
         *     JSON 对象创建失败。
         *
         * 2. role == NULL
         *
         *     消息角色不存在。
         *
         * 3. content == NULL
         *
         *     消息内容不存在。
         *
         * 4. json_object_object_add(... role ...)
         *
         *     向 message 添加 "role" 字段。
         *
         * 5. json_object_object_add(... content ...)
         *
         *     向 message 添加 "content" 字段。
         *
         * 6. json_object_array_add(...)
         *
         *     把 message 放进 messages 数组。
         *
         * 任何一步失败，都进入错误处理。
         */
        if (message == NULL ||
            role == NULL || content == NULL ||
            json_object_object_add(
                message,
                "role",
                json_object_new_string(role)
            ) != 0 ||
            json_object_object_add(
                message,
                "content",
                json_object_new_string(content)
            ) != 0 ||
            json_object_array_add(
                messages,
                message
            ) != 0) {


            /*
             * 如果 message 不为 NULL，
             * 说明它可能已经成功创建。
             *
             * 此时释放它，防止内存泄漏。
             */
            if (message != NULL) {
                json_object_put(message);

                /*
                 * 设置成 NULL。
                 *
                 * 防止 cleanup 阶段再次释放同一个对象。
                 */
                message = NULL;
            }


            set_error(
                errbuf,
                errcap,
                "内部错误：构建请求体失败"
            );


            /*
             * 跳到统一资源释放区域。
             */
            goto cleanup;
        }


        /*
         * 到这里说明：
         *
         * message 已经成功加入 messages。
         *
         * json_object_array_add() 后，
         * messages 数组接管了 message 的所有权。
         *
         * 所以这里不能再：
         *
         *     json_object_put(message);
         *
         * 否则可能导致引用计数/生命周期处理错误。
         *
         * 把局部指针设置成 NULL，
         * 表示：
         *
         *     “我自己已经不再拥有这个对象了。”
         */
        message = NULL;
    }


    /*
     * ============================================================
     * 把 messages 数组加入 root
     * ============================================================
     *
     * 最终结构变成：
     *
     * root
     *  │
     *  └── "messages"
     *          │
     *          ↓
     *      messages 数组
     */
    if (json_object_object_add(
            root,
            "messages",
            messages
        ) != 0) {

        set_error(
            errbuf,
            errcap,
            "内部错误：构建请求体失败"
        );

        goto cleanup;
    }


    /*
     * root 已经接管 messages 的所有权。
     *
     * 所以 messages 局部指针设置为 NULL。
     *
     * 后面 cleanup 时：
     *
     *     json_object_put(root);
     *
     * root 会负责释放里面的 messages。
     */
    messages = NULL;


    /*
     * ============================================================
     * 添加 stream: true
     * ============================================================
     *
     * 最终 JSON 中增加：
     *
     *     "stream": true
     *
     * 告诉 LLM 后端：
     *
     *     “我要流式输出，不要等完整答案生成完以后
     *      再一次性返回。”
     */
    if (json_object_object_add(
            root,
            "stream",
            json_object_new_boolean(1)
        ) != 0) {

        set_error(
            errbuf,
            errcap,
            "内部错误：构建请求体失败"
        );

        goto cleanup;
    }


    /*
     * ============================================================
     * 将 JSON 对象转换成字符串
     * ============================================================
     *
     * json_object_to_json_string_ext()
     * 把 JSON 对象转换成真正可以通过 HTTP 发送的字符串。
     *
     * JSON 对象：
     *
     *     root
     *
     * ↓ 序列化
     *
     * 字符串：
     *
     *     {"messages":[...],"stream":true}
     *
     * JSON_C_TO_STRING_PLAIN：
     *
     * 表示使用紧凑格式，不额外添加漂亮的缩进和换行。
     */
    const char *request_body =
        json_object_to_json_string_ext(
            root,
            JSON_C_TO_STRING_PLAIN //表示使用紧凑格式
        );


    /*
     * 如果序列化失败，无法得到请求正文。
     */
    if (request_body == NULL) {

        set_error(
            errbuf,
            errcap,
            "内部错误：序列化请求体失败"
        );

        goto cleanup;
    }


    /*
     * ============================================================
     * 4. 初始化 libcurl
     * ============================================================
     *
     * 创建一个 CURL easy handle。
     *
     * 后面所有 curl_easy_setopt() 都使用这个对象。
     */
    curl = curl_easy_init();


    /*
     * 初始化失败。
     *
     * 可能是内部资源不足等原因。
     */
    if (curl == NULL) {

        fprintf(stderr, "curl_easy_init failed\n");

        set_error(
            errbuf,
            errcap,
            "内部错误：初始化 HTTP 客户端失败"
        );

        goto cleanup;
    }


    /*
     * ============================================================
     * 构建 HTTP 请求头
     * ============================================================
     *
     * curl_slist_append()
     * 向 HTTP 请求头链表中添加一项。
     */


    /*
     * 告诉服务器：
     *
     * HTTP 请求体是 JSON。
     */
    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );


    /*
     * 告诉服务器：
     *
     * 我希望收到 SSE：
     *
     * text/event-stream
     *
     * 这与流式 LLM 输出有关。
     */
    headers = curl_slist_append(
        headers,
        "Accept: text/event-stream"
    );


    /*
     * 检查 headers 是否创建成功。
     *
     * 注意：
     *
     * curl_slist_append() 返回新的链表指针。
     *
     * 如果失败可能返回 NULL。
     */
    if (headers == NULL) {

        set_error(
            errbuf,
            errcap,
            "内部错误：构建请求头失败"
        );

        goto cleanup;
    }


    /*
     * ============================================================
     * 5. 配置 HTTP POST 请求
     * ============================================================
     */


    /*
     * 设置请求 URL。
     *
     * curl 会向这个地址发送请求。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_URL,
        url
    );


    /*
     * 设置 HTTP 请求头。
     *
     * 使用刚才构建的 headers。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_HTTPHEADER,
        headers
    );


    /*
     * 设置使用 POST 方法。
     *
     * 1L：
     *
     * libcurl 的这个参数要求 long 类型，
     * 所以这里写 1L。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_POST,
        1L
    );//开启post，1L开启，0L关闭


    /*
     * 设置 POST 请求体。
     *
     * request_body 就是刚才生成的 JSON 字符串。
     *
     * 例如：
     *
     * {
     *     "messages": [...],
     *     "stream": true
     * }
     */
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDS,
        request_body
    );


    /*
     * 明确告诉 libcurl：
     *
     * POST 请求体到底有多少字节。
     *
     * strlen() 得到 JSON 字符串长度。
     *
     * 注意：
     *
     * strlen() 不包含最后的 '\0'。
     *
     * HTTP 请求体发送的是实际 JSON 内容，
     * 并不需要把 C 字符串结尾的 '\0' 当成正文发送。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDSIZE,
        (long)strlen(request_body)
    );


    /*
     * ============================================================
     * 6. 启用流式响应回调
     * ============================================================
     *
     * 这是整个函数最关键的部分之一。
     *
     * CURLOPT_WRITEFUNCTION：
     *
     * 告诉 libcurl：
     *
     *     “你每收到一批 HTTP 响应数据，
     *      不要自己简单处理，
     *      请调用 on_http_data()。”
     */
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        on_http_data
    );


    /*
     * CURLOPT_WRITEDATA：
     *
     * 设置传递给 on_http_data() 的 userdata。
     *
     * 这里传的是：
     *
     *     &ctx
     *
     * 也就是 stream_context 结构体的地址。
     *
     * 因此 libcurl 后面相当于调用：
     *
     *     on_http_data(
     *         ptr,
     *         size,
     *         nmemb,
     *         &ctx
     *     );
     *
     * on_http_data() 中：
     *
     *     struct stream_context *ctx = userdata;
     *
     * 就能重新获得这个 ctx。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEDATA,
        &ctx
    );//将上下文指针附加给回调函数


    /*
     * ============================================================
     * 超时设置
     * ============================================================
     */


    /*
     * 连接超时时间：
     *
     * 最多等待 5 秒建立连接。
     *
     * 如果 5 秒都无法建立连接，
     * libcurl 会报超时错误。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_CONNECTTIMEOUT,
        5L
    );


    /*
     * 整个请求的最大时间：
     *
     * 600 秒 = 10 分钟。
     *
     * 注意：
     *
     * 这是整个请求的最大持续时间，
     * 包括连接、发送请求、接收流式响应等过程。
     */
    curl_easy_setopt(
        curl,
        CURLOPT_TIMEOUT,
        600L
    );


    /*
     * ============================================================
     * 7. 发送请求并持续接收响应
     * ============================================================
     *
     * curl_easy_perform() 是真正执行 HTTP 请求的地方。
     *
     * 前面的 curl_easy_setopt()
     * 只是“配置”。
     *
     * 到这里才真正：
     *
     *     建立连接
     *         ↓
     *     发送 POST 请求
     *         ↓
     *     等待服务器响应
     *         ↓
     *     收到响应数据
     *         ↓
     *     调用 on_http_data()
     *         ↓
     *     继续接收
     *         ↓
     *     ...
     *         ↓
     *     HTTP 响应结束
     */
    curl_rc = curl_easy_perform(curl);


    /*
     * ============================================================
     * 8. 获取 HTTP 状态码
     * ============================================================
     *
     * curl_easy_perform() 执行完成后，
     * 可以通过 curl_easy_getinfo() 查询本次请求的信息。
     *
     * CURLINFO_RESPONSE_CODE：
     *
     * 获取 HTTP 状态码。
     *
     * 例如：
     *
     * 200
     * 400
     * 500
     */
    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &http_status
    );


    /*
     * ============================================================
     * 9. 首先检查“业务 callback”是否失败
     * ============================================================
     *
     * 这个判断非常重要。
     *
     * 因为：
     *
     * curl_easy_perform() 返回的 curl_rc
     *
     * 和：
     *
     * ctx.callback_failed
     *
     * 是两个不同层次的错误。
     *
     * curl_rc：
     *     libcurl / HTTP / 网络层面的状态。
     *
     * callback_failed：
     *     你的业务 callback 是否处理失败。
     */
    if (ctx.callback_failed) {

        fprintf(
            stderr,
            "Stream callback failed\n"
        );


        /*
         * 比如：
         *
         * callback 里面向客户端 send()，
         * 但是客户端已经断开。
         *
         * 那么 callback 可能返回非 0。
         *
         * 这里把这个错误转换成更容易理解的错误信息。
         */
        set_error(
            errbuf,
            errcap,
            "向客户端写数据失败（客户端可能已断开）"
        );


        /*
         * 跳到统一 cleanup。
         */
        goto cleanup;
    }


    /*
     * ============================================================
     * 10. 检查 libcurl 本身是否出错
     * ============================================================
     *
     * CURLE_OK：
     *
     *     libcurl 执行成功。
     *
     * 不是 CURLE_OK：
     *
     *     说明网络、连接、超时、TLS 等 libcurl 层面
     *     发生了问题。
     */
    if (curl_rc != CURLE_OK) {

        fprintf(
            stderr,
            "HTTP request failed: %s\n",
            curl_easy_strerror(curl_rc)
        );


        /*
         * curl_easy_strerror()
         * 把 CURLcode 转换成人类可读的错误信息。
         *
         * 例如：
         *
         *     Timeout was reached
         *
         *     Couldn't connect to server
         */
        set_error(
            errbuf,
            errcap,
            "无法从推理后端获取响应：%s",
            curl_easy_strerror(curl_rc)
        );


        goto cleanup;
    }


    /*
     * ============================================================
     * 11. 检查 HTTP 状态码
     * ============================================================
     *
     * 200 ~ 299：
     *
     *     HTTP 成功。
     *
     * 其他：
     *
     *     HTTP 层面的失败。
     *
     * 例如：
     *
     * 400
     * 401
     * 404
     * 500
     */
    if (http_status < 200 || http_status >= 300) {

        fprintf(
            stderr,
            "HTTP status: %ld\n",
            http_status
        );


        /*
         * ========================================================
         * 后端失败原因处理
         * ========================================================
         *
         * 有些情况下：
         *
         * HTTP 状态码只有：
         *
         *     400
         *
         * 但真正有用的错误信息在 HTTP 响应正文里面。
         *
         * 例如：
         *
         *     request (1076 tokens) exceeds
         *     the available context size
         *
         * 之前如果只保存 HTTP 状态码：
         *
         *     HTTP 400
         *
         * 那客户端就不知道为什么失败。
         *
         * 所以 on_http_data() 在收到响应的时候，
         * 顺便把前面一小段响应正文保存到了：
         *
         *     ctx.snippet
         */
        size_t snip = utf8_safe_len(
            ctx.snippet,
            ctx.snippet_len,
            160
        );


        /*
         * snip > 0：
         *
         * 说明 snippet 中确实保存了一些响应正文。
         */
        if (snip > 0) {

            /*
             * 把：
             *
             * HTTP 状态码
             * +
             * 响应正文的一小段
             *
             * 拼成最终错误信息。
             *
             * %.*s：
             *
             * 不要求字符串一直打印到 '\0'，
             * 而是最多打印指定长度。
             *
             * 这里：
             *
             * (int)snip
             *
             * 就是指定最多打印多少字符。
             */
            set_error(
                errbuf,
                errcap,
                "推理后端返回 HTTP %ld：%.*s",
                http_status,
                (int)snip,
                ctx.snippet
            );

        } else {

            /*
             * 如果后端没有返回任何可用的响应正文，
             * 那么只能告诉上层 HTTP 状态码。
             */
            set_error(
                errbuf,
                errcap,
                "推理后端返回 HTTP %ld",
                http_status
            );
        }


        /*
         * 请求失败。
         */
        goto cleanup;
    }


    /*
     * ============================================================
     * 12. 到这里说明整个请求成功
     * ============================================================
     *
     * 能走到这里意味着：
     *
     *     callback 没失败
     *     ↓
     *     libcurl 没失败
     *     ↓
     *     HTTP 状态码是 2xx
     *
     * 所以：
     *
     *     result = 0
     *
     * 表示整个函数成功。
     */
    result = 0;


cleanup:

    /*
     * ============================================================
     * 13. 统一释放资源
     * ============================================================
     *
     * 前面很多地方都使用：
     *
     *     goto cleanup;
     *
     * 这样做的目的就是：
     *
     *     不管在哪一步失败，
     *     最后都走到同一个资源释放区域。
     *
     * 避免出现：
     *
     *     return -1;
     *
     *     return -1;
     *
     *     return -1;
     *
     * 每个地方都重复写资源释放代码。
     */


    /*
     * 如果 curl 成功初始化了，
     * 就释放 CURL handle。
     *
     * curl_easy_cleanup()
     * 会释放 curl_easy_init() 创建的资源。
     */
    if (curl != NULL) {

        curl_easy_cleanup(curl);
    }


    /*
     * 释放 HTTP 请求头链表。
     *
     * headers 可能是 NULL。
     *
     * curl_slist_free_all(NULL)
     * 是允许的，所以这里可以直接调用。
     */
    curl_slist_free_all(headers);


    /*
     * ============================================================
     * 释放 JSON 对象
     * ============================================================
     *
     * 注意这里存在“所有权”关系。
     *
     * 最终结构大概是：
     *
     * root
     *  │
     *  └── messages
     *          │
     *          ├── message
     *          ├── message
     *          └── message
     *
     * 当 root 持有 messages，
     * messages 又持有 message 时，
     * 释放 root 会递归释放它拥有的 JSON 子对象。
     */


    /*
     * 如果 root 还存在，
     * 释放 root。
     *
     * root 里面已经包含：
     *
     *     messages
     *     stream
     *
     * 所以 root 会负责这些对象的生命周期。
     */
    if (root != NULL) {

        json_object_put(root);
    }


    /*
     * 如果 messages != NULL，
     * 说明 messages 没有被 root 接管。
     *
     * 因此需要单独释放。
     *
     * 为什么前面有：
     *
     *     messages = NULL;
     *
     * 就是为了防止这里重复释放已经交给 root 的 messages。
     */
    if (messages != NULL) {

        json_object_put(messages);
    }


    /*
     * 如果 message != NULL，
     * 说明当前 message 仍然由这个局部指针拥有，
     * 没有成功加入 messages。
     *
     * 所以需要单独释放。
     *
     * 如果 message 已经成功加入 messages，
     * 前面已经：
     *
     *     message = NULL;
     *
     * 所以这里不会重复释放。
     */
    if (message != NULL) {

        json_object_put(message);
    }


    /*
     * ============================================================
     * 14. 返回最终结果
     * ============================================================
     *
     * 正常：
     *
     *     result = 0
     *
     * 失败：
     *
     *     result = -1
     */
    return result;
}
