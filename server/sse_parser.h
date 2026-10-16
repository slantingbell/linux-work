#ifndef SSE_PARSER_H
#define SSE_PARSER_H

#include <stddef.h>

#define SSE_LINE_CAP 16384  // 单个 SSE 事件行的最大长度

// 每从 SSE 流中解析出一个 content token 调用一次
// 返回 0 继续，非 0 中止解析
typedef int (*sse_token_callback)(
    const char *token,
    size_t len,
    void *userdata
);

// 增量式 SSE 解析器状态
struct sse_parser {
    char line[SSE_LINE_CAP]; // 尚未完整的一行（事件可能跨 TCP 包到达）
    size_t line_len;         // 当前行的有效长度
    int done;                // 是否已收到 [DONE] 标记
};

void sse_parser_init(struct sse_parser *parser);

// 增量喂入 SSE 字节流，逐行解析：
//   data: {...}   -> 提取 choices[0].delta.content 并回调
//   data: [DONE]  -> 置 done 标志
//   其余行（event:/id:/注释/空行）忽略
// 返回 0 成功；-1 失败（行超长或回调要求中止）
int sse_parser_feed(
    struct sse_parser *parser,
    const char *data,
    size_t len,
    sse_token_callback callback,
    void *userdata
);

#endif
