#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H
#include <sys/types.h>
#include <stddef.h>
#include <stddef.h>
//接收客户端发送的提示词
ssize_t client_receive_request(
    int client_fd,  //客户端套接字
    char *prompt,   //存放提示词缓冲区
    size_t prompt_size //缓冲区大小
);
//接收完sse的回调函数
typedef int (*llm_stream_callback)(
    const char *data,
    size_t length,
    void *userdata
);
//把提示词发送给llama-server并接收流式响应
int llm_generate_stream(
    const char *base_url,
    const char *prompt,
    llm_stream_callback callback,
    void *userdata
);


#endif