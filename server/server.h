#ifndef SERVER_H
#define SERVER_H

#include <sys/types.h>

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

#endif