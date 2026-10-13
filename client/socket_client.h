#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H
#include "client.h"
// 创建连接
int client_connect(
    const char *ip,
    int port
);
// 发送消息
int client_send(
    int sockfd,
    const char *msg
);
// 接收流式响应
struct client_connection
{
    int sockfd;
    char receive_buffer[CLIENT_RECV_CAP];
    size_t receive_len;
};
int client_receive_stream(struct client_connection *conn);
// 关闭连接
void client_close(
    int sockfd
);
#endif