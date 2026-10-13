#include "client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>

//创建连接
int client_connect(            //（对面的ip地址，对面的端口）
    const char *server_ip,
    int server_port
)
{
    // 1.创建socket
    int sockfd = socket(
        AF_INET, // 使用ipv4
        SOCK_STREAM, //面向连接通讯
        0  //默认使用TCP协议
    );
    // 套接字创建失败返回
    if (sockfd < 0)
    {
        perror("socket");
        return -1;
    }
    // 2. 设置服务器地址
    //创建ipv4地址信息结构体
    struct sockaddr_in server_addr; 
    //清空结构体，防止脏数据
    memset( 
        &server_addr, //地址起点
        0, //把地址内容全变成0
        sizeof(server_addr) //改地址的范围
    );
    //告诉socket api 传的是ipv4
    server_addr.sin_family = AF_INET;
    // 端口转网络字节序，把主机字节序转换为网络字节序
    server_addr.sin_port = htons(server_port);
    //把字符串地址转换为计算机能够使用的网络二进制地址
    if (inet_pton(
            AF_INET, //地址类型
            server_ip, //地址字符串
            &server_addr.sin_addr  //改完以后放在哪里
        ) <= 0)
    {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }
    //4. 建立TCP连接
    if (connect(
            sockfd,  //指定建立连接的套接字
            (struct sockaddr *)&server_addr, //指定要访问地址信息结构体
            sizeof(server_addr) //指定要读取的地址范围
        ) < 0) 
    {
        perror("connect");
        close(sockfd);
        return -1;
    }
    //5. 返回连接好的socket
    return sockfd;
}

//发送消息
int client_send(int sockfd, const char *msg)  //（套接字id,要发送的信息）
{
    if (sockfd < 0 || msg == NULL)  //防止套接字为空或者消息为空
    {
        return -1;
    }
    // 计算字符串长度，不包含末尾的 '\0'
    size_t len = strlen(msg);
    //标记已经发送完的字符长度
    size_t sent = 0; 
    // 循环发送，直到全部数据发送完毕，防止TCP发送缓冲区填满或者send被信号打断
    //导致send无法一次性发送所有信息
    while (sent < len)
    {
        ssize_t n = send(  //返回send已经发送的字符的数量，使用ssize_t防止信息有负数
            sockfd, //发送的套接字接口
            msg + sent, //要发送的信息的歧视地址
            len - sent, //要发送的信息的总长度
            0
        );
        if (n < 0) //send失败
        {
            // 如果被信号中断，则重试
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        if (n == 0) 
        {
            return -1;
        }
        //更新已经发送的信息的长度
        sent += (size_t)n;
    }
}