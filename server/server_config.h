#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H
#define SERVER_PORT 8000 //服务端监听端口
#define LLAMA_SERVER_EXECUTABLE "./llama.cpp/build/bin/llama-server" //llama-server位置
#define LLAMA_MODEL_PATH        "./models/qwen2.5-1.5b-instruct-q4_k_m.gguf"//model位置
#define LLAMA_HOST              "127.0.0.1" //llama-server地址
#define LLAMA_PORT              8080//llama-sever监听端口
#define LLAMA_CONTEXT_SIZE      4096//llama-server上下文大小
#define LLAMA_PARALLEL          4//llama-server并发处理的client数量
#define LLAMA_READY_TIMEOUT_S   120//
#define PROMPT_CAP  4096      // 与客户端 INPUT_SIZE 对应
#define STREAM_END  "<END>"   // 与客户端约定的流式结束标志
#define HISTORY_TURNS 3  //最大记录历史轮数
#define HISTORY_BYTE_CAP 4000//历史上下文大小
#endif