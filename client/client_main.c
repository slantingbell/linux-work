
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "socket_client.h"
#include "client_io.h"
#include "client.h"

int main(void)
{
    int sockfd = client_connect(SERVER_IP, SERVER_PORT); //调用接口连接服务器
    if (sockfd < 0)  //创建套接字失败报错
    {
        fprintf(stderr, "连接服务器失败\n");
        return 1;
    }
    struct client_connection conn = {  //创建连接结构体接收信息，内置持久输出缓冲区
        .sockfd = sockfd,  //写入套接字
        .receive_len = 0   //初始化持久缓冲区的有效信息为0
    };
    char input[INPUT_SIZE]; //创建输入缓冲区
    while (1)
    {
        printf("You> ");  //输出提示，示意当前该你说话
        fflush(stdout);
        int len = client_read_input(input, sizeof(input)); //读取输入到输入缓冲区，换行结束
        if (len < 0) // EOF 或输入错误
        {
            break;
        }
        if (strcmp(input, EXIT_COMMAND) == 0) //如果检测到为关闭客户端信号，则关闭客户端
        {
            printf("正在关闭客户端...\n");
            break;
        }
        if (len == 0) // 忽略空输入
        {
            continue;
        }
        if (client_send(sockfd, input) < 0) //像服务端发送消息
        {
            fprintf(stderr, "发送消息失败\n");
            break;
        }
        printf("Server> ");  //流式接收并输出服务端消息
        if (client_receive_stream(&conn) < 0)
        {
            fprintf(stderr, "接收服务器响应失败\n");
            break;
        }
        printf("\n");
    }
    client_close(sockfd); //检测到关闭信号后关闭套接字
    return 0;
}
