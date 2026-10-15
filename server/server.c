#include "server.h"

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "server_config.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>

//创建套接字并启动监听端口
int server_listen(int port)
{
    // 检查端口范围
    if (port < 1 || port > 65535) {
        fprintf(stderr, "无效端口号: %d\n", port);
        return -1;
    }

    // 1. 创建 TCP socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return -1;
    }

    // 2. 设置地址复用选项
    int opt = 1;
    if (setsockopt(server_fd,  //要设置的socketid
                SOL_SOCKET,  //要改动的配置层级
                SO_REUSEADDR,  //要改动的配置选项，此处配置允许地址端口复用
                &opt,          //要更改的选项信息的指针
                 sizeof(opt))  //选项大小
                  < 0) {
        perror("setsockopt");  
        close(server_fd);      //退出前关闭刚刚创立的的套接字
        return -1;
    }

    // 3. 配置服务器地址
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons((unsigned short)port);

    // 4. 绑定端口
    if (bind(server_fd,
             (struct sockaddr *)&server_addr,
             sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    // 5. 开始监听
    if (listen(server_fd, 128) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }

    return server_fd;
}

//等待接收，客户端连接并返回客户端socket (服务器套接字，存放客户端地址的容器的地址)
int server_accept_client(
    int server_fd,
    struct sockaddr_in *client_addr)
{
    if (server_fd < 0 || client_addr == NULL) { 
        errno = EINVAL; //手动设置错误代码，表示无效参数
        return -1;
    }

    socklen_t addr_len = sizeof(*client_addr);

    int client_fd = accept( //接收客户端连接并保存地址
        server_fd,  //监听套接字
        (struct sockaddr *)client_addr,  //地址缓冲区
        &addr_len    //地址缓冲区长度
    );
    if (client_fd < 0) {  
        perror("acccept");
        return -1;
    }
    return client_fd;  
}

//回收子进程
int server_reap_children(void)
{
    int reaped = 0;
    while (1) {
        int status = 0;
        pid_t pid = waitpid(
            -1,           // 任意子进程
            &status,      // 接收子进程退出状态
            WNOHANG       // 非阻塞
        );
        if (pid > 0) {
            // 成功回收一个子进程
            reaped++;
            continue;
        }
        if (pid == 0) {
            // 没有已退出、可回收的子进程
            return reaped;
        }
        // pid == -1
        if (errno == EINTR) {
            // 被信号中断，重新尝试
            continue;
        }
        if (errno == ECHILD) {
            // 当前没有可等待的子进程
            return reaped;
        }
        // 其他错误
        return -1;
    }
}

//工具函数，作用是复制字符串，并保持地址空间独立
static char *duplicate_string(const char *src)
{
    size_t len = strlen(src);

    char *dst = malloc(len + 1);
    if (dst == NULL) {
        return NULL;
    }

    memcpy(dst, src, len + 1);
    return dst;
}

//llama-server的初始化配置，多此一举主要是为了校验启动server的配置
int llama_server_init(
    struct server **server, //要保存server对象地址的容器
    const struct server_config *config //server配置
)
{
    *server = NULL;
    /* 1. 校验参数 */
    if (server == NULL ||
        config == NULL ||
        config->executable_path == NULL ||
        config->executable_path[0] == '\0' ||
        config->model_path == NULL ||
        config->model_path[0] == '\0' ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->port < 1 ||
        config->port > 65535 ||
        config->context_size <= 0 ||
        config->parallel <= 0) {
        errno = EINVAL;
        return -1;
    }

    /* 2. 初始化输出参数 */

    /* 3. 分配并初始化对象 */
    struct server *instance = calloc(1, sizeof(*instance));
    if (instance == NULL) {
        errno = ENOMEM;
        return -1;
    }

    /* 4. 复制字符串配置 */
    instance->executable_path =
        duplicate_string(config->executable_path);

    instance->model_path =
        duplicate_string(config->model_path);

    instance->host =
        duplicate_string(config->host);

    if (instance->executable_path == NULL ||
        instance->model_path == NULL ||
        instance->host == NULL) {
        goto fail;
    }

    /* 5. 保存数值配置 */
    instance->port = config->port;
    instance->context_size = config->context_size;
    instance->parallel = config->parallel;

    /* 6. 初始化运行状态 */
    instance->pid = -1;
    instance->running = 0;

    /* 7. 返回对象 */
    *server = instance;
    return 0;

fail: //释放资源
    {
        int saved_errno = errno;

        free(instance->executable_path);
        free(instance->model_path);
        free(instance->host);
        free(instance);

        errno = saved_errno;
        return -1;
    }

}


/*
 * 启动 llama-server 子进程
 *
 * 返回值：
 *   0  成功创建子进程
 *  -1  启动失败
 */
int server_start(struct server *server)
{
    /* 1. 检查参数 */
    if (server == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 2. 防止重复启动 */
    if (server->running) {
        errno = EALREADY;
        return -1;
    }

    /* 3. 将数值参数转换为字符串 */
    char port_str[16];
    char context_str[16];
    char parallel_str[16];

    snprintf(port_str, sizeof(port_str), "%d",
             server->port);

    snprintf(context_str, sizeof(context_str), "%d",
             server->context_size);

    snprintf(parallel_str, sizeof(parallel_str), "%d",
             server->parallel);

    /* 4. 创建子进程 */
    pid_t pid = fork();

    if (pid < 0) {
        /* fork 失败 */
        return -1;
    }

    if (pid == 0) {
        /*
         * 子进程：
         * 使用 exec 启动 llama-server。
         *
         * argv 对应：
         * llama-server -m MODEL
         *              --host HOST
         *              --port PORT
         *              -c CONTEXT
         *              -np PARALLEL
         */
        execl(
            server->executable_path,
            server->executable_path,
            "-m", server->model_path,
            "--host", server->host,
            "--port", port_str,
            "-c", context_str,
            "-np", parallel_str,
            (char *)NULL
        );

        /*
         * 只有 exec 失败才会执行到这里。
         */
        perror("exec llama-server");
        _exit(127);
    }

    /*
     * 父进程：
     * 保存子进程 PID。
     */
    server->pid = pid;
    server->running = 1;

    return 0;
}
//关闭服务器
void server_free(struct server *server)
{
    /* 1. 检查参数 */
    if (server == NULL) {
        return;
    }

    /* 2. 停止并回收 llama-server 子进程 */
    if (server->pid > 0) {
        pid_t pid = server->pid;

        /*
         * 请求子进程正常终止。
         * 如果子进程已经退出，kill 可能返回 ESRCH。
         */
        if (kill(pid, SIGTERM) == 0) { //向指定进程发送终止信号，成功0.不成功-1
            int status;
            pid_t result;

            /* 等待子进程退出，并处理信号中断 */
            do {
                result = waitpid(pid, &status, 0);
            } while (result == -1 && errno == EINTR); //被信号终端就重新回收
        }
    }

    /* 3. 释放对象内部动态分配的资源 */
    free(server->executable_path);
    free(server->model_path);
    free(server->host);

    /* 4. 清理对象状态 */
    server->pid = -1;
    server->running = 0;

    /* 5. 释放服务器对象本身 */
    free(server);
}