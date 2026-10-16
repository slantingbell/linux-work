# linux-work 项目构建（目标环境：Linux）
# 依赖：gcc、libcurl、json-c
# 用法：
#   make        构建 bin/server 与 bin/client
#   make clean  清理构建产物

CC       ?= gcc
CFLAGS   ?= -Wall -Wextra -O2 -std=gnu11
DEPFLAGS  = -MMD -MP

# 优先用 pkg-config 获取 libcurl / json-c 参数；没有 pkg-config 时退回常见默认值
PKG_CFLAGS := $(shell pkg-config --cflags libcurl json-c 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs libcurl json-c 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lcurl -ljson-c
endif

BUILD_DIR := build
BIN_DIR   := bin

SERVER_SRCS := server/server_main.c \
               server/server.c \
               server/client_handler.c \
               server/sse_parser.c \
               server/logger.c

CLIENT_SRCS := client/client_main.c \
               client/socket_client.c \
               client/client_io.c

SERVER_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SERVER_SRCS))
CLIENT_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CLIENT_SRCS))
DEPS        := $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)

.PHONY: all clean

all: $(BIN_DIR)/server $(BIN_DIR)/client

$(BIN_DIR)/server: $(SERVER_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(PKG_LIBS)

$(BIN_DIR)/client: $(CLIENT_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^

# 兼容说明（在不改动已有源码的前提下保证编译通过）：
#   server.c         使用了 malloc/calloc/free、kill/SIGTERM，但未包含 <stdlib.h>、<signal.h>；
#                    且 server.h 中 struct sockaddr_in 的声明先于 <netinet/in.h> 出现，
#                    原型作用域结构体会与定义冲突导致编译报错
#   client_handler.c 使用了 recv，但未包含 <sys/socket.h>
# 这里用 -include 预包含对应头文件。日后若补全源文件中的 #include，可移除此选项。
$(BUILD_DIR)/server/server.o: server/server.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(PKG_CFLAGS) \
	    -include stdlib.h -include signal.h \
	    -include sys/socket.h -include netinet/in.h \
	    -c -o $@ $<

$(BUILD_DIR)/server/client_handler.o: server/client_handler.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(PKG_CFLAGS) -include sys/socket.h -c -o $@ $<

# client 侧同理：socket_client.h 的结构体中使用了 size_t，但它自身与 client_config.h
# 均未包含 <stddef.h>，而 socket_client.c / client_io.c 在包含该头文件之前未引入 size_t
$(BUILD_DIR)/client/socket_client.o: client/socket_client.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -include stddef.h -c -o $@ $<

$(BUILD_DIR)/client/client_io.o: client/client_io.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -include stddef.h -c -o $@ $<

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) $(PKG_CFLAGS) -c -o $@ $<

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
