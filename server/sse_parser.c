#include "sse_parser.h"

#include <string.h>
#include <json-c/json.h>

#define SSE_DATA_PREFIX "data:"  //SSE数据格式前缀
#define SSE_DONE_MARK   "[DONE]" //完成标记
/**
 * 初始化see解析器
 */
void sse_parser_init(struct sse_parser *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->line_len = 0;
    parser->done = 0;
}

// 处理一条完整的 SSE 行（不含 '\n'）
static int sse_handle_line(
    struct sse_parser *parser,
    sse_token_callback callback,//每一token输入的回调函数
    void *userdata 
)
{
    size_t len = parser->line_len;
    parser->line_len = 0;

    // 去掉可能的 '\r'（\r\n 行尾）
    if (len > 0 && parser->line[len - 1] == '\r') {
        len--;
    }
    parser->line[len] = '\0';

    // 空行是事件分隔符，忽略
    if (len == 0) {
        return 0;
    }

    // 只关心 data: 行，event:/id:/注释等一律忽略
    if (strncmp(parser->line, SSE_DATA_PREFIX, strlen(SSE_DATA_PREFIX)) != 0) {
        return 0;
    }

    const char *payload = parser->line + strlen(SSE_DATA_PREFIX);
    while (*payload == ' ') {
        payload++;
    }

    // 流式响应结束标记
    if (strcmp(payload, SSE_DONE_MARK) == 0) {
        parser->done = 1;
        return 0;
    }

    // 解析 JSON，提取 choices[0].delta.content
    struct json_object *root = json_tokener_parse(payload);
    if (root == NULL) {
        // 无法解析的行不视为致命错误，直接跳过
        return 0;
    }
    /**
     *将拿到的paylaod进行解析，llama-server返回的流式token格式：
     *  chocies:[delta:{content:"xxxx"}]
     * 解析出content的内容，调用回调函数callback
     */
    struct json_object *choices = NULL;
    struct json_object *delta = NULL;
    struct json_object *content = NULL;

    if (json_object_object_get_ex(root, "choices", &choices) &&
        json_object_is_type(choices, json_type_array) &&
        json_object_array_length(choices) > 0) {
        struct json_object *choice0 = json_object_array_get_idx(choices, 0);
        if (choice0 != NULL &&
            json_object_object_get_ex(choice0, "delta", &delta) &&
            json_object_object_get_ex(delta, "content", &content) &&
            json_object_is_type(content, json_type_string)) {
            const char *text = json_object_get_string(content);
            size_t text_len = (size_t)json_object_get_string_len(content);
            if (text != NULL && text_len > 0 &&
                callback != NULL &&
                callback(text, text_len, userdata) != 0) {
                json_object_put(root);
                return -1;
            }
        }
    }
    /*
    *释放JSON对象资源
    */
    json_object_put(root);
    return 0;
}


/**
 * 处理一段 SSE 字节流，逐行解析
 */
int sse_parser_feed(
    struct sse_parser *parser,
    const char *data,
    size_t len,
    sse_token_callback callback,
    void *userdata
)
{
    if (parser == NULL || (data == NULL && len > 0)) {
        return -1;
    }

    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            // 一行完整，开始解析
            if (sse_handle_line(parser, callback, userdata) != 0) {
                return -1;
            }
        } else {
            // 防止行缓冲溢出
            if (parser->line_len >= sizeof(parser->line) - 1) {
                return -1;
            }
            parser->line[parser->line_len++] = c;
        }
    }
    return 0;
}
