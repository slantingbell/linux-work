#ifndef SOCKET_CLIENT_H
#define SOCKET_CLIENT_H


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
int client_receive_stream(
    int sockfd
);


// 关闭连接
void client_close(
    int sockfd
);


#endif