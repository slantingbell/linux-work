#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static FILE *g_log_file = NULL;

static const char *const level_names[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

int logger_init(const char *path)
{
    if (path == NULL) {
        return 0;
    }
    // 追加模式：多进程写同一日志文件时，O_APPEND 保证每次写入原子地落到文件末尾
    g_log_file = fopen(path, "a");
    if (g_log_file == NULL) {
        return -1;
    }
    return 0;
}

void logger_write(enum log_level level, const char *fmt, ...)
{
    if (fmt == NULL) {
        return;
    }

    // 生成时间戳
    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    const char *name = "?";
    if (level >= LOG_DEBUG && level <= LOG_ERROR) {
        name = level_names[level];
    }

    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "[%s] [%s] ", timestamp, name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);

    if (g_log_file != NULL) {
        va_start(ap, fmt);
        fprintf(g_log_file, "[%s] [%s] ", timestamp, name);
        vfprintf(g_log_file, fmt, ap);
        fputc('\n', g_log_file);
        fflush(g_log_file); // 立即落盘，并避免 fork 后缓冲区数据被父子进程重复写出
        va_end(ap);
    }
}

void logger_close(void)
{
    if (g_log_file != NULL) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}
