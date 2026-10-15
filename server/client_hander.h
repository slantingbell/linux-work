#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H
#include <sys/types.h>
#include <stddef.h>
/*
 * 处理一个已经建立连接的客户端。
 *
 * client_fd：与客户端通信的已连接 Socket
 *
 * 返回值：
 *   0  正常结束
 *  -1  处理失败
 */
int client_handle(int client_fd);

// 接收并解析一条客户端提示词
// 成功：返回提示词长度（不含 '\0'）
// 失败：返回 -1

struct llm_client;

/* 接收一个生成文本片段的回调函数 */
typedef int (*llm_token_callback)(
    const char *data,
    size_t len,
    void *userdata
);

/* 连接推理服务 */
int llm_client_connect(
    struct llm_client **client,
    const char *socket_path
);

/* 创建一个独立会话 */
int llm_client_create_session(
    struct llm_client *client,
    char *session_id,
    size_t session_id_size
);

/* 发送推理请求，并逐段接收生成结果 */
int llm_client_generate_stream(
    struct llm_client *client,
    const char *session_id,
    const char *prompt,
    llm_token_callback callback,
    void *userdata
);

/* 清空指定会话的对话历史 */
int llm_client_reset_session(
    struct llm_client *client,
    const char *session_id
);

/* 销毁指定会话 */
int llm_client_destroy_session(
    struct llm_client *client,
    const char *session_id
);

/* 断开推理服务连接并释放客户端对象 */
void llm_client_close(
    struct llm_client *client
);

#endif