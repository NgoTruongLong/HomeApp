#ifndef XIAOZHI_MCP_H
#define XIAOZHI_MCP_H

#include "debug.h"
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

/* Hàm xử lý 1 tool.
 *   args : object "arguments" server gửi xuống (có thể NULL)
 *   out  : ghi câu trả lời dạng text cho AI đọc
 * Trả về false -> response có isError = true. */
typedef bool (*mcp_tool_fn)(const cJSON *args, char *out, size_t out_sz);

/* user_only = true -> AI KHÔNG thấy, chỉ app companion thấy
 * (tools/list phải có withUserTools=true mới trả về). */
void mcp_register_tool(const char *name, const char *desc,
                       const char *schema_json, bool user_only, mcp_tool_fn fn);

/* Gọi 1 lần lúc init để đăng ký các tool của board. */
void mcp_init(void);

/* Gọi khi parse được {"type":"mcp","payload":<payload>}. */
void mcp_handle_message(cJSON *payload);

#endif /* XIAOZHI_MCP_H */