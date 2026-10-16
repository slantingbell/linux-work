# linux-work

基于 C/Linux 的轻量级本地 LLM 流式对话系统：TCP 服务端与客户端全部自己实现，推理交给 llama.cpp 的 `llama-server`（HTTP + SSE）。

## 整体架构

```
client (TCP :8000)  --->  server (fork 多进程)  --->  llama-server (HTTP :8080)
  文本 + \0                每连接一个子进程            POST /v1/chat/completions
  <---  token 流 + <END>  <---  SSE: choices[0].delta.content
```

- server 启动时自己 `fork` + `exec` 拉起 `llama-server`，并轮询 `/health` 直到模型加载完成（最长等 120 秒），就绪后才开始监听客户端
- 每个客户端连接由独立的 fork 子进程处理，连接之间互不阻塞；推理并发上限由 `llama-server` 的 `-np` 参数决定（当前 4）
- server 收到 SIGINT/SIGTERM 时关闭监听，并向 `llama-server` 发 SIGTERM 回收子进程

## 目录结构

```
linux-work/
├── server/                    # C 服务端
│   ├── server_main.c          # 主程序：启动 llama-server、等就绪、监听、fork、信号处理、流式转发
│   ├── server.c / server.h    # 监听 socket、客户端 accept、子进程回收、llama-server 生命周期
│   ├── client_handler.c/.h    # 接收客户端请求；libcurl 调用 llama-server 流式接口
│   ├── sse_parser.c / .h      # 增量式 SSE 解析，提取 delta.content
│   ├── logger.c / .h          # 分级日志（stderr + logs/server.log）
│   └── server_config.h        # 服务端监听端口
│
├── client/                    # C 客户端
│   ├── client_main.c          # 主程序：输入 -> 发送 -> 流式打印
│   ├── socket_client.c/.h     # TCP 连接、发送、流式接收（持久缓冲区处理粘包/拆包）
│   ├── client_io.c / .h       # 标准输入读取
│   └── client_config.h        # 服务端地址、缓冲区大小、退出命令
│
├── llama.cpp/                 # llama.cpp 源码根目录（.gitignore 忽略，需自行放置并编译）
├── models/                    # GGUF 模型（.gitignore 忽略，需自行放置）
├── scripts/                   # 构建辅助脚本
│   └── cuda_glibc_compat.h    # CUDA 13 + glibc ≥ 2.41 的编译 shim（见「构建」第 2 步）
├── logs/                      # 运行日志（服务端写入 logs/server.log）
└── Makefile
```

`llama.cpp/` 与 `models/` 都在 `.gitignore` 中，仓库里没有这两部分，克隆后需要自己补上（见「构建」）。

⚠️ `llama.cpp/` 必须**直接**是源码根目录（即 `llama.cpp/CMakeLists.txt` 存在）。不要把源码再多套一层，`server_main.c` 里硬编码的路径是 `./llama.cpp/build/bin/llama-server`，没有额外的软链接可用。

## 通信协议

| 方向 | 消息格式 | 结束条件 |
|---|---|---|
| Client → Server | 文本内容 + `\0` | 遇到 `\0` |
| Server → Client | 流式 token + `<END>` | 遇到 `<END>` |

- 一条 TCP 连接上可以反复问答：客户端每发一条 prompt，服务端就以一次 `<END>` 收尾
- 客户端发送的 `\0` 是长度终结符；服务端按字节数校验长度，不依赖换行
- 空 prompt 不会调用模型，服务端直接回一个 `<END>`
- 推理失败时，失败原因会作为正文（`\n[服务端错误] ...\n`）先发给客户端，之后照常补一个 `<END>`，因此协议对客户端仍是一致的

## 配置项

| 配置 | 位置 | 当前值 |
|---|---|---|
| 服务端监听端口 | `server/server_config.h` `SERVER_PORT` | 8000 |
| llama-server 可执行文件 | `server/server_main.c` `LLAMA_SERVER_EXECUTABLE` | `./llama.cpp/build/bin/llama-server` |
| 模型路径 | `server/server_main.c` `LLAMA_MODEL_PATH` | `./models/qwen2.5-1.5b-instruct-q4_k_m.gguf` |
| 推理后端地址 | `server/server_main.c` `LLAMA_HOST` / `LLAMA_PORT` | 127.0.0.1 : 8080 |
| 上下文长度 / 并发槽位 | `server/server_main.c` `LLAMA_CONTEXT_SIZE` / `LLAMA_PARALLEL` | 4096 / 4 |
| 后端就绪等待上限 | `server/server_main.c` `LLAMA_READY_TIMEOUT_S` | 120 秒 |
| 单条 prompt 上限 | `server/server_main.c` `PROMPT_CAP` | 4096 字节 |
| 服务端地址 | `client/client_config.h` `SERVER_IP` / `SERVER_PORT` | 127.0.0.1 : 8000 |
| 输入缓冲 / 接收缓冲 | `client/client_config.h` `INPUT_SIZE` / `CLIENT_RECV_CAP` | 4096 / 8192 |
| 退出命令 | `client/client_config.h` `EXIT_COMMAND` | `/exit` |

`llama-server` 由 server 用固定参数拉起，等价于：

```bash
llama-server -m ./models/qwen2.5-1.5b-instruct-q4_k_m.gguf \
             --host 127.0.0.1 --port 8080 -c 4096 -np 4
```

## 构建（Linux）

```bash
# 1. 安装依赖
sudo apt install build-essential cmake libcurl4-openssl-dev libjson-c-dev

# 2. 把 llama.cpp 源码放到 ./llama.cpp（源码根目录），然后编译
#    产出 llama.cpp/build/bin/llama-server
cd llama.cpp
cmake -B build -DGGML_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=86 \
      -DCMAKE_CUDA_FLAGS="-Xcompiler=-include$(pwd)/../scripts/cuda_glibc_compat.h"
cmake --build build --config Release -j
cd ..

# 3. 放置模型（文件名需与 LLAMA_MODEL_PATH 一致）
#    models/qwen2.5-1.5b-instruct-q4_k_m.gguf

# 4. 编译本项目（产出 bin/server 与 bin/client）
make

# 清理本项目构建产物
make clean
```

第 2 步的几点说明：

- **CUDA**：`GGML_CUDA` 默认是 `OFF`（`ggml/CMakeLists.txt:199`），不加 `-DGGML_CUDA=ON` 会构建出纯 CPU 版本，能跑但慢很多；`CMAKE_CUDA_ARCHITECTURES=86` 对应 Ampere（RTX 30 系），换卡需要改。没有 NVIDIA GPU 的话去掉这两个参数即可。
- **`cuda_glibc_compat.h` shim**：Ubuntu 26.04 的 glibc ≥ 2.41 按 C23 给 `rsqrtf()` 加了 `noexcept`，与 CUDA 头文件中的同名声明冲突，nvcc 会报 `exception specification is incompatible with that of previous function "rsqrtf"`。该 shim 经 `-Xcompiler=-include` 只作用于 nvcc 的宿主编译那一趟，用于清空 `__THROW`。**纯 CPU 构建不需要它**；升级 glibc 之外的版本后若报错消失，也可以去掉。shim 内容见 `scripts/cuda_glibc_compat.h` 顶部注释。
- **`--config Release`**：当前生成器是 Unix Makefiles（单配置），这个参数实际被忽略；llama.cpp 在未指定时会自己默认 `Release`（`CMakeLists.txt:30-32`），所以留着无害，与上游文档写法一致。

本项目 Makefile 本身用 `pkg-config` 取 libcurl/json-c 参数，没有 `pkg-config` 时回退到 `-lcurl -ljson-c`。另外，源码中少数文件缺少必要的 `#include`，Makefile 用 `-include` 预包含头文件做了兼容（详见 Makefile 内注释）；日后补全 `#include` 后可以移除这些选项。

## 运行

```bash
# 必须在项目根目录启动：llama-server 与模型用的是相对路径
./bin/server

# 另开终端启动客户端，可多开（每个连接一个服务端子进程）
./bin/client
```

```
You> 你好
Server> （token 逐个流式打印，结束后换行）
You> /exit
正在关闭客户端...
```

- 客户端输入 `/exit` 或按 `Ctrl+D` 退出
- 服务端 `Ctrl+C`（SIGINT/SIGTERM）优雅退出，会自动终止 llama-server
- 运行日志：`tail -f logs/server.log`（同时输出到 stderr，级别为 DEBUG/INFO/WARN/ERROR）
- 局域网其它机器使用时，改 `client/client_config.h` 的 `SERVER_IP` 即可；服务端监听 `INADDR_ANY`，无需改绑定地址

## 已知限制

- **上下文仅限当前连接**：同一 TCP 连接最多保留最近 3 轮对话（总计最多 8192 字节）；断开连接后清空，不支持跨连接恢复
- **客户端输入会被截断**：`fgets` 单次最多读 4095 字节，超长输入的剩余部分会被当作下一条 prompt 发出去
- **服务端 prompt 上限 4096 字节**：超出后 `client_receive_request` 返回 `EMSGSIZE`，该连接被直接关闭
- 客户端用 8192 字节持久缓冲区跨 `recv` 处理被拆开的 `<END>`，缓冲区边收边输出、不会随回复长度增长，因此长回复本身不受限
- 后端推理串行受 `-np` 限制：第 5 个并发请求要在 llama-server 侧排队
