#include <sys/types.h>
#include <stddef.h>
#include <errno.h>

// 接收并解析一条客户端提示词
// 成功：返回提示词长度（不含 '\0'）
// 失败：返回 -1
ssize_t client_receive_request(
    int client_fd,  //客户端套接字
    char *prompt,   //存放提示词缓冲区
    size_t prompt_size //缓冲区大小
)
{
    if (client_fd < 0 || prompt == NULL || prompt_size == 0) { //检查合法
        errno = EINVAL; 
        return -1;
    }

    char incoming[2048]; //临时缓冲区
    size_t used = 0;  //提示词缓冲区记录已经接收到的字节数

    while (1) {
        // 批量接收数据
        ssize_t n = recv(    //这玩意是覆盖写入  
            client_fd,
            incoming,
            sizeof(incoming),
            0
        );
        //异常退出
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            // 连接关闭，但尚未收到完整请求
            errno = ECONNRESET;
            return -1;
        }
        // 在本次实际接收的数据中查找结束标记
        char *end = memchr(incoming, '\0', (size_t)n);

        // 本次接收的数据中没有结束标记
        if (end == NULL) {
            size_t received = (size_t)n; //本次接收到的字节数

            // 判断提示词缓冲区剩余容量是否够用
            if (received > prompt_size - 1 - used) { 
                errno = EMSGSIZE;
                return -1;
            }
            //把本轮接收到的提示词数据放入提示词缓冲区
            memcpy(prompt + used, incoming, received);
            used += received; //更新提示词缓冲区中的信息数量
            continue;
        }
        //end != NULL
        // 结束标记之前的正文长度
        size_t body_len = (size_t)(end - incoming);

        // 检查完整请求能否放入 prompt
        if (body_len > prompt_size - 1 - used) {
            errno = EMSGSIZE;
            return -1;
        }

        // 只复制结束标记之前的正文
        memcpy(prompt + used, incoming, body_len);
        used += body_len;

        // 构造合法的 C 字符串
        prompt[used] = '\0';
        
        return (ssize_t)used;
    }
}


