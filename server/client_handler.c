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
        errno = EINVAL; 
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
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            // 连接关闭，但尚未收到完整请求
            errno = ECONNRESET;
            return -1;
        }
        // 在本次实际接收的数据中查找结束标记
        char *end = memchr(incoming, '\0', (size_t)n);

        // 本次接收的数据中没有结束标记
        if (end == NULL) {
            size_t received = (size_t)n; //本次接收到的字节数

            // 判断提示词缓冲区剩余容量是否够用
            if (received > prompt_size - 1 - used) { 
                errno = EMSGSIZE;
                return -1;
            }
            //把本轮接收到的提示词数据放入提示词缓冲区
            memcpy(prompt + used, incoming, received);
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

struct stream_context {
    llm_stream_callback callback;
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

int llm_generate_stream(
    const char *base_url,
    const char *prompt,
    const struct chat_message *history,
    size_t history_count,
    llm_stream_callback callback,
    void *userdata,
    char *errbuf,
    size_t errcap
)
{
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    struct json_object *root = NULL;
    struct json_object *messages = NULL;
    struct json_object *message = NULL;
    struct stream_context ctx = {
        .callback = callback,
        .userdata = userdata,
        .callback_failed = 0,
        .snippet_len = 0
    };

    CURLcode curl_rc;
    long http_status = 0;
    int result = -1;
    char url[1024];

    if (base_url == NULL || prompt == NULL ||
        (history_count > 0 && history == NULL) || callback == NULL) {
        fprintf(stderr, "Invalid arguments\n");
        set_error(errbuf, errcap, "内部错误：参数非法");
        return -1;
    }

    int n = snprintf(
        url,
        sizeof(url),
        "%s/v1/chat/completions",
        base_url
    );

    if (n < 0 || (size_t)n >= sizeof(url)) {
        fprintf(stderr, "URL is too long\n");
        set_error(errbuf, errcap, "内部错误：后端 URL 过长");
        return -1;
    }

    /* 1. 构建 JSON 请求体 */
    root = json_object_new_object();
    messages = json_object_new_array();
    message = json_object_new_object();

    if (root == NULL || messages == NULL || message == NULL) {
        set_error(errbuf, errcap, "内部错误：构建请求体失败（内存不足）");
        goto cleanup;
    }

    for (size_t i = 0; i <= history_count; i++) {
        const char *role = i < history_count ? history[i].role : "user";
        const char *content = i < history_count ? history[i].content : prompt;
        message = json_object_new_object();
        if (message == NULL ||
            role == NULL || content == NULL ||
            json_object_object_add(message, "role",
                                   json_object_new_string(role)) != 0 ||
            json_object_object_add(message, "content",
                                   json_object_new_string(content)) != 0 ||
            json_object_array_add(messages, message) != 0) {
            if (message != NULL) {
                json_object_put(message);
                message = NULL;
            }
            set_error(errbuf, errcap, "内部错误：构建请求体失败");
            goto cleanup;
        }
        message = NULL; /* messages 接管 message 的所有权 */
    }

    /* root 接管 messages 的所有权 */
    if (json_object_object_add(root, "messages", messages) != 0) {
        set_error(errbuf, errcap, "内部错误：构建请求体失败");
        goto cleanup;
    }
    messages = NULL;

    if (json_object_object_add(
            root,
            "stream",
            json_object_new_boolean(1)) != 0) {
        set_error(errbuf, errcap, "内部错误：构建请求体失败");
        goto cleanup;
    }

    const char *request_body =
        json_object_to_json_string_ext(
            root,
            JSON_C_TO_STRING_PLAIN
        );

    if (request_body == NULL) {
        set_error(errbuf, errcap, "内部错误：序列化请求体失败");
        goto cleanup;
    }

    /* 2. 初始化 libcurl */
    curl = curl_easy_init();
    if (curl == NULL) {
        fprintf(stderr, "curl_easy_init failed\n");
        set_error(errbuf, errcap, "内部错误：初始化 HTTP 客户端失败");
        goto cleanup;
    }

    headers = curl_slist_append(
        headers,
        "Content-Type: application/json"
    );
    headers = curl_slist_append(
        headers,
        "Accept: text/event-stream"
    );

    if (headers == NULL) {
        set_error(errbuf, errcap, "内部错误：构建请求头失败");
        goto cleanup;
    }

    /* 3. 配置 HTTP POST */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
    curl_easy_setopt(
        curl,
        CURLOPT_POSTFIELDSIZE,
        (long)strlen(request_body)
    );

    /* 4. 启用流式响应回调 */
    curl_easy_setopt(
        curl,
        CURLOPT_WRITEFUNCTION,
        on_http_data
    );
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    /* 超时设置：连接 5 秒，整个请求最多 10 分钟 */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);

    /* 5. 发送请求并持续接收响应 */
    curl_rc = curl_easy_perform(curl);

    /* 6. 获取 HTTP 状态码 */
    curl_easy_getinfo(
        curl,
        CURLINFO_RESPONSE_CODE,
        &http_status
    );

    if (ctx.callback_failed) {
        fprintf(stderr, "Stream callback failed\n");
        set_error(errbuf, errcap, "向客户端写数据失败（客户端可能已断开）");
        goto cleanup;
    }

    if (curl_rc != CURLE_OK) {
        fprintf(
            stderr,
            "HTTP request failed: %s\n",
            curl_easy_strerror(curl_rc)
        );
        set_error(
            errbuf,
            errcap,
            "无法从推理后端获取响应：%s",
            curl_easy_strerror(curl_rc)
        );
        goto cleanup;
    }

    if (http_status < 200 || http_status >= 300) {
        fprintf(stderr, "HTTP status: %ld\n", http_status);
        /*
         * 后端把真正的失败原因写在响应体里（例如
         * "request (1076 tokens) exceeds the available context size"）。
         * 以前这里只打了一行状态码就把响应体丢了，客户端只能看到空白。
         */
        size_t snip = utf8_safe_len(ctx.snippet, ctx.snippet_len, 160);
        if (snip > 0) {
            set_error(
                errbuf,
                errcap,
                "推理后端返回 HTTP %ld：%.*s",
                http_status,
                (int)snip,
                ctx.snippet
            );
        } else {
            set_error(errbuf, errcap, "推理后端返回 HTTP %ld", http_status);
        }
        goto cleanup;
    }

    result = 0;

cleanup:
    if (curl != NULL) {
        curl_easy_cleanup(curl);
    }

    curl_slist_free_all(headers);

    if (root != NULL) {
        json_object_put(root);
    }
    if (messages != NULL) {
        json_object_put(messages);
    }
    if (message != NULL) {
        json_object_put(message);
    }

    return result;
}
