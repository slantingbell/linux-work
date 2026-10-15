#ifndef SERVER_H
#define SERVER_H

#include <sys/types.h>
//自定义server对象
struct server {
    char *executable_path; // llama-server 可执行文件路径
    char *model_path;      // GGUF 模型路径
    char *host;           // 监听地址

    int port;             //监听端口
    int context_size;    //上下文长度
    int parallel;        //并行推理数量

    pid_t pid;          //运行llama-server的进程号
    int running;        //llama-server的运行状态 1运行 0不运行
};
//自定义server配置
struct server_config {
    const char *executable_path;  // llama-server 可执行文件路径
    const char *model_path;       // GGUF 模型路径
    const char *host;             // 监听地址
    int port;                     // HTTP 端口
    int context_size;             // 上下文长度
    int parallel;                 // 并行推理槽位数
};
/*
 * 创建并启动监听 Socket。
 *
 * port：服务端监听端口。
 *
 * 成功：返回监听 Socket 文件描述符。
 * 失败：返回 -1。
 */
int server_listen(int port);

// 接受一个客户端连接，返回客户端 socket
// client_addr 用于接收客户端 IP 和端口
int server_accept_client(
    int server_fd,
    struct sockaddr_in *client_addr
);
/*
 * 回收已经结束的子进程。
 *
 * 非阻塞地检查并回收已退出的子进程，
 * 不等待仍在运行的子进程。
 *
 * 成功：返回回收的子进程数量。
 * 失败：返回 -1。
 */
int server_reap_children(void);
/* 创建并初始化服务器对象 */
int llama_server_init(
    struct server **server,
    const struct server_config *config
);

/* 启动 llama-server 并建立 TCP 监听 */
int server_start(struct server *server);

/* 释放服务器对象 */
void server_free(struct server *server);

#endif