#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

#include "server.h"
#include "server_config.h"
#include "client_handler.h"
#include "sse_parser.h"
#include "logger.h"

// 收到 SIGINT（ctrl=c）/SIGTERM(kill) 时置位，通知主循环退出
static volatile sig_atomic_t g_stop = 0; //volatitle 阻止编译器优化 sig_atomoic_t 保证操作原子性
//信号监听函数
static void on_signal(int sig)  
{
    (void)sig; //防编译器警告变量未使用
    g_stop = 1;
}

// 收到SIGCHLD时非阻塞回收子进程，避免僵尸进程堆积
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    (void)sig;
    server_reap_children();
    errno = saved_errno;
}

// 循环发送直到全部发出，供服务端内部使用
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) { //防止无限循环
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// 流式转发上下文：SSE 字节 -> 解析出 token -> 发给客户端
struct forward_ctx {
    int client_fd;
    struct sse_parser parser;
    char response[HISTORY_BYTE_CAP + 1];
    size_t response_len;
    int response_too_large;
};

// sse_parser 每解析出一个 token 时回调
static int on_token(const char *token, size_t len, void *userdata)
{
    struct forward_ctx *ctx = userdata;
    if (!ctx->response_too_large) {
        if (len > HISTORY_BYTE_CAP - ctx->response_len) {
            ctx->response_too_large = 1;
        } else {
            memcpy(ctx->response + ctx->response_len, token, len);
            ctx->response_len += len;
            ctx->response[ctx->response_len] = '\0';
        }
    }
    return send_all(ctx->client_fd, token, len);
}

// llm_generate_stream 每收到一段原始 SSE 数据时回调
static int on_sse_bytes(const char *data, size_t len, void *userdata)
{
    struct forward_ctx *ctx = userdata;
    return sse_parser_feed(&ctx->parser, data, len, on_token, ctx);
}


//推理失败时把失败内容标识并发送给客户端
static int send_error_notice(int client_fd, const char *reason)
{
    char msg[512];
    int n = snprintf(msg, sizeof(msg), "\n[服务端错误] %s\n", reason);
    if (n < 0) {
        return -1;
    }
    size_t len = (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1;
    return send_all(client_fd, msg, len);
}
//删除最早的一轮历史对话（历史对话数组，当前信息数，当前字节数）
static void history_drop_oldest(struct chat_message *history,
                                size_t *count, size_t *bytes)
{   //更新总的字节数
    *bytes -= strlen(history[0].content) + strlen(history[1].content);
    //删除一轮
    free((char *)history[0].content);
    free((char *)history[1].content);
    //移动数组
    memmove(history, history + 2, (*count - 2) * sizeof(*history));
    //跟新总对话数
    *count -= 2;
}
//添加新一轮对话（历史对话容器，消息，总字节数，用户提示词，LLM回复）
static int history_add_turn(struct chat_message *history, size_t *count,
                            size_t *bytes, const char *prompt,
                            const char *response)
{
    //计算本轮对话总大小，不超过历史对话总容量，超过直接丢弃，因为就算把所有轮的对话全删掉也装不下
    size_t prompt_len = strlen(prompt);
    size_t response_len = strlen(response);
    if (prompt_len + response_len > HISTORY_BYTE_CAP) {
        return 1;
    }
    //把提示词和回答复制一份
    char *saved_prompt = strdup(prompt);
    char *saved_response = strdup(response);
    if (saved_prompt == NULL || saved_response == NULL) {
        free(saved_prompt);
        free(saved_response);
        return -1;
    }
   //如果装不下或者轮数超过了就删除最早一轮，直到能装下为止
    while (*count + 2 > HISTORY_TURNS * 2 || //轮数超了
           *bytes + prompt_len + response_len > HISTORY_BYTE_CAP) { //当前剩余容量不足
        history_drop_oldest(history, count, bytes); //删除最早一轮
    }

    history[*count] = (struct chat_message){"user", saved_prompt};
    history[*count + 1] = (struct chat_message){"assistant", saved_response};
    *count += 2;
    *bytes += prompt_len + response_len;
    return 0;
}

// 处理一个客户端连接：循环接收 prompt -> 调用 LLM -> 流式转发 -> 发送 <END>
static void handle_client(int client_fd, const char *llama_url)
{   
    //初始化同容器
    char prompt[PROMPT_CAP]; //提示词缓冲区
    char errbuf[512]; // 错误信息缓冲区
    struct chat_message history[HISTORY_TURNS * 2]; //历史上下文数组
    size_t history_count = 0;  //历史消息条数
    size_t history_bytes = 0;  //历史消息条数
 
    for (;;) {
        //接收提示词
        ssize_t n = client_receive_request(client_fd, prompt, sizeof(prompt));
        if (n < 0) {
            // 客户端断开或协议错误
            break;
        }

        if (n == 0) {
            // 空 prompt 不调用 LLM，直接回复结束标记防止客户端空等
            if (send_all(client_fd, STREAM_END, sizeof(STREAM_END) - 1) != 0) {
                break;
            }
            continue;
        }
        //写入日志
        logger_write(LOG_INFO, "收到 prompt fd=%d pid=%d len=%zd",
                     client_fd, (int)getpid(), n);
        //构建本轮对话所需要的临时状态
        struct forward_ctx ctx;
        ctx.client_fd = client_fd;
        sse_parser_init(&ctx.parser);
        //一轮回复数量记载
        ctx.response_len = 0;
        ctx.response_too_large = 0;
        //是否发生错误标记
        errbuf[0] = '\0';
        if (llm_generate_stream(llama_url, prompt, history, history_count, //请求LLM失败
                                on_sse_bytes, &ctx, errbuf,
                                sizeof(errbuf)) != 0) {
            const char *reason = errbuf[0] != '\0' ? errbuf : "未知错误"; //获取失败原因
            logger_write(LOG_ERROR, "LLM 请求失败 fd=%d pid=%d: %s",      
                         client_fd, (int)getpid(), reason);
            // 向客户端发送失败原因
            if (send_error_notice(client_fd, reason) != 0) {
                break; // 客户端已断开
            }
        } else if (ctx.response_too_large) { //请求成功，但是回答太长，拒绝加入历史上下文
            logger_write(LOG_WARN, "回答过长，不加入上下文 fd=%d pid=%d",
                         client_fd, (int)getpid());
        } else { //请求成功，且回复消息的信息量足够小，写入历史上下文
            int history_rc = history_add_turn(
                history, &history_count, &history_bytes, prompt, ctx.response);
            if (history_rc > 0) {
                logger_write(LOG_WARN, "本轮内容超过上下文上限，未保存 fd=%d pid=%d",
                             client_fd, (int)getpid());
            } else if (history_rc < 0) {  
                logger_write(LOG_WARN, "保存对话上下文失败 fd=%d pid=%d",
                             client_fd, (int)getpid());
            }
        }

        // 结束本轮对话时发送结束标志
        if (send_all(client_fd, STREAM_END, sizeof(STREAM_END) - 1) != 0) {
            break; // 客户端已断开
        }
    }
    //客户端断开，释放资源
    for (size_t i = 0; i < history_count; i++) {//histpry中的消息容器时动态分配的，需要动态回收
        free((char *)history[i].content);
    }
    close(client_fd);
    logger_write(LOG_INFO, "客户端断开 fd=%d pid=%d", client_fd, (int)getpid());
}

// libcurl 写回调：健康检查的响应体直接丢弃(不写回调函数libcrul默认会把响应体输出到终端)
static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb; //返回响应体总数据量，示意libcurl所有数据处理完毕，本次请求正常
}

// 轮询 llama-server /health 直到返回 200（模型加载需要时间）
// 返回 0 就绪；-1 超时或收到退出信号
static int wait_llama_ready(const char *base_url, int timeout_s)
{   
    //获取llama-server的存活信息的url接口
    char health_url[1024];
    snprintf(health_url, sizeof(health_url), "%s/health", base_url);
    //初始化curl句柄
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return -1;
    }
    int ready = -1;//默认不存活
    for (int i = 0; i < timeout_s * 2 && !g_stop; i++) { // 约每 500ms 轮询一次
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, health_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L); 
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);

        if (curl_easy_perform(curl) == CURLE_OK) { //成功发起请求
            //获取状态码
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            //确认存活，保存结果
            if (code == 200) {
                ready = 0;
                break;
            }
        }
        //等待0.5s
        usleep(500 * 1000);
    }
     //清理资源返回结果
    curl_easy_cleanup(curl);
    return ready;
}

int main(void)
{
    //初始化日志
    // 日志文件打开失败仅回退到 stderr，不影响启动
    if (logger_init("logs/server.log") != 0) {
        logger_write(LOG_WARN, "无法打开 logs/server.log，仅输出到 stderr");
    }
    //初始化 libcurl
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // 写入llama-server 的 HTTP 地址
    char llama_url[64];
    snprintf(llama_url, sizeof(llama_url), "http://%s:%d", LLAMA_HOST, LLAMA_PORT);

    // 1. 启动 llama-server 子进程
    struct server_config config = {
        .executable_path = LLAMA_SERVER_EXECUTABLE,
        .model_path      = LLAMA_MODEL_PATH,
        .host            = LLAMA_HOST,
        .port            = LLAMA_PORT,
        .context_size    = LLAMA_CONTEXT_SIZE,
        .parallel        = LLAMA_PARALLEL,
    };
    //检查llama-server config
    struct server *llama = NULL;
    if (llama_server_init(&llama, &config) != 0) {
        logger_write(LOG_ERROR, "llama-server 配置无效: %s", strerror(errno));
        logger_close();
        return 1;
    }
    //启动llama-server
    if (server_start(llama) != 0) {
        logger_write(LOG_ERROR, "llama-server 启动失败: %s", strerror(errno));
        server_free(llama);
        logger_close();
        return 1;
    }
    logger_write(LOG_INFO, "llama-server 已启动 pid=%d", (int)llama->pid);
    
    // 2. 注册信号处理函数
    //使在server收到ctrl+c或者被kill的时候能正确退出
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不加 SA_RESTART：让 accept 被信号打断以后执行循环体后面的逻辑，并有机会检查 g_stop结束循环
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    //使server在收到子进程退出信号以后能正确回收子进程
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);
    // 向已断开的客户端写数据时不让进程被杀
    signal(SIGPIPE, SIG_IGN); 

    // 3. 等待 llama-server 加载模型并就绪
    if (wait_llama_ready(llama_url, LLAMA_READY_TIMEOUT_S) != 0) {
        logger_write(LOG_ERROR, "llama-server 就绪等待超时或被中断");
        server_free(llama);
        logger_close();
        return 1;
    }
    logger_write(LOG_INFO, "llama-server 已就绪 %s", llama_url);

    // 4. 注册监听客户端端口
    int listen_fd = server_listen(SERVER_PORT);
    if (listen_fd < 0) {
        logger_write(LOG_ERROR, "监听端口 %d 失败", SERVER_PORT);
        server_free(llama);
        logger_close();
        return 1;
    }
    logger_write(LOG_INFO, "服务已监听 port=%d", SERVER_PORT);

    // 5. 主循环：每个客户端连接 fork 一个子进程处理
    while (!g_stop) {
        //accept子进程
        struct sockaddr_in client_addr;
        int client_fd = server_accept_client(listen_fd, &client_addr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue; // 被信号打断，回到循环检查 g_stop
            }
            logger_write(LOG_ERROR, "accept 失败: %s", strerror(errno));
            continue;
        }
        //创建子进程
        pid_t pid = fork();
        if (pid < 0) {//子进程创建失败
            logger_write(LOG_ERROR, "fork 失败: %s", strerror(errno));
            close(client_fd);
            continue;
        }
        if (pid == 0) {
            // 子进程
            //关闭监听套接字并重置信号处理函数
            close(listen_fd);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            //执行子进程处理逻辑
            handle_client(client_fd, llama_url);
            _exit(0);
        }

        // 父进程：继续监听下一个连接
        close(client_fd);
        //获取子进程地址
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        logger_write(LOG_INFO, "客户端连接 %s:%d fd=%d pid=%d",
                     ip, ntohs(client_addr.sin_port), client_fd, (int)pid);
    }

    // 6. 优雅退出：关闭监听并终止 llama-server
    close(listen_fd);
    server_free(llama);
    logger_write(LOG_INFO, "服务已退出");
    logger_close();
    curl_global_cleanup();
    return 0;
}
