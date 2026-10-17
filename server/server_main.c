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

/*
 * llama-server 启动配置
 * 注意：可执行文件与模型使用相对路径，需从项目根目录启动本程序。
 * 迁移到 Linux 后如目录结构不同，修改这里即可。
 */
#define LLAMA_SERVER_EXECUTABLE "./llama.cpp/.build-stale/bin/llama-server"
#define LLAMA_MODEL_PATH        "./models/qwen2.5-1.5b-instruct-q4_k_m.gguf"
#define LLAMA_HOST              "127.0.0.1"
#define LLAMA_PORT              8080
#define LLAMA_CONTEXT_SIZE      4096 
#define LLAMA_PARALLEL          4
#define LLAMA_READY_TIMEOUT_S   120

#define PROMPT_CAP  4096      // 与客户端 INPUT_SIZE 对应
#define STREAM_END  "<END>"   // 与客户端约定的流式结束标志
#define HISTORY_TURNS 3
#define HISTORY_BYTE_CAP 8192

// 收到 SIGINT/SIGTERM 时置位，通知主循环退出
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// 有子进程退出时非阻塞回收，避免僵尸进程堆积
static void on_sigchld(int sig)
{
    int saved_errno = errno;
    (void)sig;
    server_reap_children(); //回收子进程
    errno = saved_errno;    
}

// 循环发送直到全部发出，供服务端内部使用
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);//MSG_NOSIGNAL：向已断开的客户端写数据时不让进程被杀
        if (n < 0) {
            if (errno == EINTR) {//EINTR：被信号中断，重新尝试
                continue;
            }
            return -1;
        }
        if (n == 0) {
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

/*
 * 推理失败时，把失败原因当作正文发给客户端。
 *
 * 以前这里什么都不发、直接跟一个 <END>：客户端打出一片空白，
 * 看起来就像"模型给了个空回答"，而真正的失败原因全被吞进了服务端日志。
 * 现在原因作为普通文本先行发出，后面仍然照常跟 <END>，
 * 协议没有变化，老客户端不用改。
 */
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

static void history_drop_oldest(struct chat_message *history,
                                size_t *count, size_t *bytes)
{
    *bytes -= strlen(history[0].content) + strlen(history[1].content);
    free((char *)history[0].content);
    free((char *)history[1].content);
    memmove(history, history + 2, (*count - 2) * sizeof(*history));
    *count -= 2;
}

static int history_add_turn(struct chat_message *history, size_t *count,
                            size_t *bytes, const char *prompt,
                            const char *response)
{
    size_t prompt_len = strlen(prompt);
    size_t response_len = strlen(response);
    if (prompt_len + response_len > HISTORY_BYTE_CAP) {
        return 1;
    }

    char *saved_prompt = strdup(prompt);
    char *saved_response = strdup(response);
    if (saved_prompt == NULL || saved_response == NULL) {
        free(saved_prompt);
        free(saved_response);
        return -1;
    }

    while (*count + 2 > HISTORY_TURNS * 2 ||
           *bytes + prompt_len + response_len > HISTORY_BYTE_CAP) {
        history_drop_oldest(history, count, bytes);
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
    char prompt[PROMPT_CAP];
    char errbuf[512]; // llm_generate_stream 失败时写入原因
    struct chat_message history[HISTORY_TURNS * 2];
    size_t history_count = 0;
    size_t history_bytes = 0;

    for (;;) {
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

        logger_write(LOG_INFO, "收到 prompt fd=%d pid=%d len=%zd",
                     client_fd, (int)getpid(), n);

        struct forward_ctx ctx;
        ctx.client_fd = client_fd;
        sse_parser_init(&ctx.parser);
        ctx.response_len = 0;
        ctx.response_too_large = 0;

        errbuf[0] = '\0';
        if (llm_generate_stream(llama_url, prompt, history, history_count,
                                on_sse_bytes, &ctx, errbuf,
                                sizeof(errbuf)) != 0) {
            const char *reason = errbuf[0] != '\0' ? errbuf : "未知错误";
            logger_write(LOG_ERROR, "LLM 请求失败 fd=%d pid=%d: %s",
                         client_fd, (int)getpid(), reason);
            // 让客户端看到失败原因，而不是让它在空白里猜
            if (send_error_notice(client_fd, reason) != 0) {
                break; // 客户端已断开
            }
        } else if (ctx.response_too_large) {
            logger_write(LOG_WARN, "回答过长，不加入上下文 fd=%d pid=%d",
                         client_fd, (int)getpid());
        } else {
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

        // 结束时都要发送结束标记，避免客户端一直等待
        if (send_all(client_fd, STREAM_END, sizeof(STREAM_END) - 1) != 0) {
            break; // 客户端已断开
        }
    }

    for (size_t i = 0; i < history_count; i++) {
        free((char *)history[i].content);
    }
    close(client_fd);
    logger_write(LOG_INFO, "客户端断开 fd=%d pid=%d", client_fd, (int)getpid());
}

// libcurl 写回调：健康检查的响应体直接丢弃
static size_t discard_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void)ptr;
    (void)userdata;
    return size * nmemb;
}

// 轮询 llama-server /health 直到返回 200（模型加载需要时间）
// 返回 0 就绪；-1 超时或收到退出信号
static int wait_llama_ready(const char *base_url, int timeout_s)
{
    char health_url[1024];
    snprintf(health_url, sizeof(health_url), "%s/health", base_url);

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return -1;
    }

    int ready = -1;
    for (int i = 0; i < timeout_s * 2 && !g_stop; i++) { // 每 500ms 轮询一次
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, health_url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);

        if (curl_easy_perform(curl) == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            if (code == 200) {
                ready = 0;
                break;
            }
        }
        usleep(500 * 1000);
    }

    curl_easy_cleanup(curl);
    return ready;
}

int main(void)
{
    // 日志文件打开失败仅回退到 stderr，不影响启动
    if (logger_init("logs/server.log") != 0) {
        logger_write(LOG_WARN, "无法打开 logs/server.log，仅输出到 stderr");
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // llama-server 的 HTTP 地址
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

    struct server *llama = NULL;
    if (llama_server_init(&llama, &config) != 0) {
        logger_write(LOG_ERROR, "llama-server 配置无效: %s", strerror(errno));
        logger_close();
        return 1;
    }

    if (server_start(llama) != 0) {
        logger_write(LOG_ERROR, "llama-server 启动失败: %s", strerror(errno));
        server_free(llama);
        logger_close();
        return 1;
    }
    logger_write(LOG_INFO, "llama-server 已启动 pid=%d", (int)llama->pid);

    // 2. 注册信号（在轮询就绪前注册，等待期间也能响应 Ctrl+C）
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // 不加 SA_RESTART：让 accept 被信号打断以检查 g_stop
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = on_sigchld;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa_chld, NULL);

    signal(SIGPIPE, SIG_IGN); // 向已断开的客户端写数据时不让进程被杀

    // 3. 等待 llama-server 加载模型并就绪
    if (wait_llama_ready(llama_url, LLAMA_READY_TIMEOUT_S) != 0) {
        logger_write(LOG_ERROR, "llama-server 就绪等待超时或被中断");
        server_free(llama);
        logger_close();
        return 1;
    }
    logger_write(LOG_INFO, "llama-server 已就绪 %s", llama_url);

    // 4. 监听客户端端口
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
        struct sockaddr_in client_addr;
        int client_fd = server_accept_client(listen_fd, &client_addr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue; // 被信号打断，回到循环检查 g_stop
            }
            logger_write(LOG_ERROR, "accept 失败: %s", strerror(errno));
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            logger_write(LOG_ERROR, "fork 失败: %s", strerror(errno));
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            // 子进程：只处理连接，不监听
            close(listen_fd);
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);
            handle_client(client_fd, llama_url);
            _exit(0);
        }

        // 父进程：继续监听下一个连接
        close(client_fd);

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
