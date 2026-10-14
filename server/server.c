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