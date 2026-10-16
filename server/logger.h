#ifndef LOGGER_H
#define LOGGER_H

enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

// 初始化日志模块
// path 为 NULL 时仅输出到 stderr
// 成功返回 0；文件打开失败返回 -1（仍可使用 stderr 输出）
int logger_init(const char *path);

// 输出一条日志：[时间戳] [级别] 消息
// 同时写入 stderr 和日志文件（若已打开）
void logger_write(enum log_level level, const char *fmt, ...);

// 关闭日志文件
void logger_close(void);

#endif
