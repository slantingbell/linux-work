# linux-work
A lightweight local LLM text interaction system implemented in C for Linux environments.

项目初步架构
llm-stream-system/
│
├── server/                    # C服务端
│   ├── server.c               # 服务端主程序
│   ├── client_handler.c       # 客户端处理
│   ├── client_handler.h
│   ├── config.h               # 服务端配置
│   ├── logger.c               # 日志
│   └── logger.h
│
├── client/                    # C客户端
│   ├── client.c               # 客户端程序
│   ├── config.h               # 客户端配置
│
├── llm/                       # LLM调用模块
│   ├── llama_client.c         # 调用 llama.cpp API
│   ├── llama_client.h
│
│
├── llama.cpp/                 # llama.cpp源码
│   └── models/
│       └── model.gguf
│
├── logs/
│   ├── server.log
│   └── client.log
│
└── Makefile

## client
### clinet
用户输入
   |
   |
发送prompt
   |
   |
接收token
   |
   |
实时打印
伪代码
while(1)
{
    输入prompt

    send(sockfd,prompt,...);


    while(1)
    {
        recv(sockfd,buffer,...);

        printf("%s",buffer);

        fflush(stdout);
    }
}
## server
### server
          server.py

              |
              |
       接收客户端连接

              |
       +------+------+
       |             |
    client A      client B

       |             |

   handler A     handler B

       |             |

       +------LLM----+

创建socket
监听端口
接收连接
创建线程
伪代码
ocket()

bind()

listen()


while(1)
{

    client_fd = accept();


    pthread_create(
        &tid,
        NULL,
        handle_client,
        &client_fd
    );

}
### client_handler
接受用户输入
   |
   |
读取prompt
   |
   |
向llm发送prompt
   |
   |
接受返回的token并返回给client
伪代码
void *handle_client(void *arg)
{

    int client_fd =
        *(int *)arg;


    char buffer[1024];


    recv(
        client_fd,
        buffer,
        sizeof(buffer),
        0
    );


    llama_generate(
        client_fd,
        buffer
    );


    close(client_fd);

}
## llama
### llmama_client
    接收client_handler的prompt
        |
        |
    运行LLM
        |
        |
    返回结果
伪代码
