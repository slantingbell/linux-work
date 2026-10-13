
#include "socket_client.h"

#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>

// 读取用户输入
int client_read_input(char *buffer, size_t size) //（缓冲区地址，缓冲区大小）
{ 
    if (buffer == NULL || size < 2) //缓冲区地址不能为空，且至少能读取到一个有效字符
    {
        return -1;
    }
    if (fgets(buffer, size, stdin) == NULL) //从标准输入中读取最多size-1个字符到指定的区域，这里是buffer
    {
        return -1;
    }
    // 查找换行符
    char *newline = strchr(buffer, '\n');
    if (newline != NULL)
    {
        // 将换行符替换为字符串结束符,换行符是无效字符，不需要提取
        *newline = '\0';
    }
    return (int)strlen(buffer);
}
