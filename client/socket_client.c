#include "client_config.h"
#include"socket_client.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#define STREAM_END "<END>" //约定的结束标志
#define STREAM_END_LEN (sizeof(STREAM_END) - 1)
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
            sizeof(server_addr) //指定要读取的地址结构体大小，不同地址结构体大小不一样
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
    // 计算字符串长度，包含末尾的 '\0'
    size_t len = strlen(msg)+1;
    //标记已经发送完的字符长度
    size_t sent = 0; 
    // 循环发送，直到全部数据发送完毕，防止TCP发送缓冲区填满或者send被信号打断
    //导致send无法一次性发送所有信息
    while (sent < len)
    {
        ssize_t n = send(  //返回send已经发送的字符的数量，使用ssize_t防止信息有负数
            sockfd, //发送的套接字接口
            msg + sent, //要发送的信息的起始地址
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
        if (n == 0)  //防止无限循环
        {
            return -1;
        }
        //更新已经发送的信息的长度
        sent += (size_t)n;
    }
    return 0;
}
//接收流式信息并输出
//可重构
int client_receive_stream(struct client_connection *conn)
{
    if (conn == NULL || conn->sockfd < 0 ||                  //检查持久缓冲区是否合法
        conn->receive_len > sizeof(conn->receive_buffer))    //为什么需要持久缓冲区：TCP式流式接收返回的信息，如果本次临时缓冲区包含了下一轮问答中返回的信息，本轮问答结束
    {
        return -1;
    }
    char incoming[2048];  //临时缓冲区
    while (1)
    {
        ssize_t n = recv(      //接受数据到临时缓冲区
            conn->sockfd,   //接收套接字id
            incoming,       //存放的地方
            sizeof(incoming),  //一次最多接收的数据量
            0       //不启用特殊接收
        );
        if (n < 0)      //接收失败
        {
            if (errno == EINTR)  //如果是信号打断的就重启
                continue;
            perror("recv");  //如果不是信号打断就报错
            return -1;
        }
        if (n == 0)  //如果n=0说明连接本身正常关闭，但没有接收到结束标志，说明异常关闭
        {
            fprintf(stderr, "\n连接关闭，响应未完整结束\n");
            return -1;
        }
        size_t available = sizeof(conn->receive_buffer) //持久缓冲区中剩余空间
                         - conn->receive_len;
        if ((size_t)n > available)  //如果临时缓冲区中的数据大于持久缓冲区，说明接收数据会溢出，报错
        {
            fprintf(stderr, "\n接收缓冲区已满\n");
            return -1;
        }
        memcpy(                                            //把临时缓冲区的数据追加到持久缓冲区
            conn->receive_buffer + conn->receive_len,   //追加起始位置
            incoming,                                   //追加位置
            (size_t)n                                   //追加长度
        );
        conn->receive_len += (size_t)n;   //更新持久缓冲区中的有效信息长度
        char *end = NULL;                 //定义专门寻找结束标记的指针，相当于flag
        /*
         * 缓冲区保存的是带长度的字节序列，
         * 不假设它一定以 '\0' 结尾。
         */
        //在当前持久缓冲区中搜索结束标志
        if (conn->receive_len >= STREAM_END_LEN)   //如果持久缓冲区中的有效长度本身小于结束标志的长度，那一定不存在结束标志，跳过搜索
        {
            for (size_t i = 0; //搜索起始下标
                 i + STREAM_END_LEN <= conn->receive_len; //搜索结束下标
                 ++i)
            {
                if (memcmp(  //寻找结束标志
                        conn->receive_buffer + i,   //持久序列中比对长度起点
                        STREAM_END,     //比对内容
                        STREAM_END_LEN   //比对长度
                    ) == 0)  
                {
                    end = conn->receive_buffer + i; //找到，并记录结束标志位置
                    break;
                }
            }
        }
        if (end != NULL) //找到结束标记
        {
            size_t output_len = (size_t)(end - conn->receive_buffer); //计算结束标志之前有多少有效数据

            fwrite(conn->receive_buffer, 1, output_len, stdout);  //把持久缓冲区中结束标志之前的数据写入输出缓冲区
            fflush(stdout); //刷新输出缓存区 
            size_t consumed = output_len + STREAM_END_LEN;     //消费长度=持久缓冲区前的数据长度+结束标志长度
            size_t remaining = conn->receive_len - consumed;    // 计算持久缓冲区中第二轮问答的有效数据长度
            memmove(  //把消费过的数据从持久缓冲区中移除（此函数相当于整体移动储存单元，把超出范围的信息直接数据直接扔掉）
                conn->receive_buffer,  //持久缓冲区指针
                conn->receive_buffer + consumed,  //第二轮有效数据起始位置
                remaining  //第二轮有效数据长度
            );
            conn->receive_len = remaining; //更新持久缓冲区中有效信息长度
            return 0; //结束本次读取
        }
        //没找到结束标记
        size_t keep = STREAM_END_LEN - 1; //末尾至少保留4个字节，防止结束标记的一部分被输出之后，后续搜索不到结束标志
        if (conn->receive_len > keep)
        {
            size_t output_len = conn->receive_len - keep;  //计算可以直接输出的信息长度
            fwrite(                      //把可以输出的信息直接写入输出缓冲区
                conn->receive_buffer,   //持久缓冲区起始地址
                1,                      //元素字节数
                output_len,             //可输出的有效信息长度
                stdout                 //直接输出到标准输出流缓冲区
            );
            fflush(stdout);           //刷新输出流缓冲区显示信息
            memmove(                  //把已经输出的信息从持久缓冲区中移除
                conn->receive_buffer,   //持久缓冲区其实地址
                conn->receive_buffer + output_len,  //需要移除的信息长度
                keep                    //需要保留的信息长度
            );
            conn->receive_len = keep;  //更新持久缓冲区有效信息
        }
    }
}
//关闭连接
void client_close(int sockfd)
{
    if (sockfd < 0)  //识别非法套接字id
    {
        return;
    }

    if (close(sockfd) < 0)  //关闭失败打印失败信息
    {
        perror("close");
    }
}