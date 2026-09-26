#include "xiaozhi_mcp.h"
#include "xiaozhi_ws.h"
#include "pump_control.h"
#include "sensor_control.h"
#include "commons.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define THIS_MODULE_NAME "xiaozhi_mcp"
#define MCP_MAX_TOOLS    12
#define MCP_TEXT_MAX     256

typedef struct {
    const char *name;
    const char *desc;
    const char *schema;      /* chuoi JSON cua inputSchema */
    bool        user_only;
    mcp_tool_fn fn;
} mcp_tool_t;

static mcp_tool_t s_tools[MCP_MAX_TOOLS];
static int        s_tool_count;
static bool       s_initialized;

void mcp_register_tool(const char *name, const char *desc, const char *schema,
                       bool user_only, mcp_tool_fn fn)
{
    if (name == NULL || fn == NULL || s_tool_count >= MCP_MAX_TOOLS) {
        ESP_LOGE(THIS_MODULE_NAME, "bad tool / table full: %s", name ? name : "?");
        return;
    }
    s_tools[s_tool_count].name      = name;
    s_tools[s_tool_count].desc      = desc;
    s_tools[s_tool_count].schema    = schema;
    s_tools[s_tool_count].user_only = user_only;
    s_tools[s_tool_count].fn        = fn;
    s_tool_count++;
    ESP_LOGI(THIS_MODULE_NAME, "tool: %s", name);
}

static void mcp_send(cJSON *id_item, cJSON *result, cJSON *error)
{
    cJSON *resp = cJSON_CreateObject();
    char  *txt;

    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id",
        id_item ? cJSON_Duplicate(id_item, true) : cJSON_CreateNull());
    if (error) cJSON_AddItemToObject(resp, "error", error);
    else       cJSON_AddItemToObject(resp, "result", result);

    txt = cJSON_PrintUnformatted(resp);   /* phai free bang cJSON_free */
    if (txt != NULL) {
        xz_mcp_send_payload(txt);
        cJSON_free(txt);
    }
    cJSON_Delete(resp);
}

static void mcp_send_error(cJSON *id_item, int code, const char *msg)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    mcp_send(id_item, NULL, err);
}

static void mcp_handle_initialize(cJSON *id)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *result = cJSON_CreateObject();
    cJSON *caps, *info;

    cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
    caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "longnt_custom_board");
    cJSON_AddStringToObject(info, "version", desc ? desc->version : "1.0.0");

    mcp_send(id, result, NULL);
}

static void mcp_handle_tools_list(cJSON *params, cJSON *id)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = cJSON_AddArrayToObject(result, "tools");
    cJSON *wu     = cJSON_GetObjectItem(params, "withUserTools");
    bool   show_user = cJSON_IsTrue(wu);

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool, *schema;

        if (s_tools[i].user_only && !show_user) {
            continue;
        }
        tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].desc);

        schema = s_tools[i].schema ? cJSON_Parse(s_tools[i].schema) : NULL;
        if (schema == NULL) {
            schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddObjectToObject(schema, "properties");
        }
        cJSON_AddItemToObject(tool, "inputSchema", schema);
        cJSON_AddItemToArray(arr, tool);
    }
    cJSON_AddStringToObject(result, "nextCursor", "");
    mcp_send(id, result, NULL);
}

static void mcp_handle_tools_call(cJSON *params, cJSON *id)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    char   text[MCP_TEXT_MAX] = {0};
    cJSON *result, *content, *c;
    int    found = -1;
    bool   ok;

    if (!cJSON_IsString(name)) {
        mcp_send_error(id, -32602, "Missing tool name");
        return;
    }
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name->valuestring) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Unknown tool: %s", name->valuestring);
        mcp_send_error(id, -32601, msg);
        return;
    }

    ok = s_tools[found].fn(cJSON_IsObject(args) ? args : NULL, text, sizeof(text));
    if (text[0] == '\0') {
        snprintf(text, sizeof(text), "%s", ok ? "OK" : "Failed");
    }

    result  = cJSON_CreateObject();
    content = cJSON_AddArrayToObject(result, "content");
    c       = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "type", "text");
    cJSON_AddStringToObject(c, "text", text);
    cJSON_AddItemToArray(content, c);
    cJSON_AddBoolToObject(result, "isError", !ok);

    mcp_send(id, result, NULL);
}

void mcp_handle_message(cJSON *payload)
{
    cJSON *method = cJSON_GetObjectItem(payload, "method");
    cJSON *params = cJSON_GetObjectItem(payload, "params");
    cJSON *id     = cJSON_GetObjectItem(payload, "id");

    if (!cJSON_IsString(method)) {
        return;
    }
    if (strcmp(method->valuestring, "initialize") == 0) {
        mcp_handle_initialize(id);
    } else if (strcmp(method->valuestring, "tools/list") == 0) {
        mcp_handle_tools_list(params, id);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        mcp_handle_tools_call(params, id);
    } else if (strncmp(method->valuestring, "notifications/", 14) == 0) {
        ESP_LOGI(THIS_MODULE_NAME, "notify: %s", method->valuestring);
    } else {
        mcp_send_error(id, -32601, "Method not found");
    }
}

static bool tool_get_device_status(const cJSON *args, char *out, size_t out_sz)
{
    (void)args;
    snprintf(out, out_sz, "May bom dang %s, cong suat %u%%",
             pump_get_state() == PUMP_ON ? "BAT" : "TAT",
             (unsigned)pump_get_duty());
    return true;
}

static bool tool_pump_set_state(const cJSON *args, char *out, size_t out_sz)
{
    cJSON *on = args ? cJSON_GetObjectItem(args, "on") : NULL;

    if (!cJSON_IsBool(on)) {
        snprintf(out, out_sz, "Thieu tham so 'on' (true/false)");
        return false;
    }
    if (pump_set_state(cJSON_IsTrue(on) ? PUMP_ON : PUMP_OFF) != APP_OK) {
        snprintf(out, out_sz, "Dieu khien may bom that bai");
        return false;
    }
    snprintf(out, out_sz, "%s", cJSON_IsTrue(on) ? "Da BAT may bom" : "Da TAT may bom");
    return true;
}

static bool tool_sensor_get_environment(const cJSON *args, char *out, size_t out_sz)
{
    sensor_data_t d;
    (void)args;

    if (sensor_get_data(&d) != APP_OK) {
        snprintf(out, out_sz, "Khong doc duoc cam bien");
        return false;
    }
    snprintf(out, out_sz,
             "Nhiet do %.1f do C, do am %.1f%%, ap suat %.1f hPa, "
             "PM2.5 %d ug/m3, CO2 %u ppm",
             d.temperature, d.humidity, d.pressure,
             (int)d.pm2_5_atm, (unsigned)d.co2);
    return true;
}

void mcp_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    s_tool_count  = 0;

    mcp_register_tool("self.get_device_status",
        "Doc trang thai thiet bi: may bom dang bat hay tat, cong suat bao nhieu",
        "{\"type\":\"object\",\"properties\":{}}", false,
        tool_get_device_status);

    mcp_register_tool("self.pump.set_state",
        "Bat hoac tat may bom nuoc",
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\","
        "\"description\":\"true = bat bom, false = tat bom\"}},"
        "\"required\":[\"on\"]}", false,
        tool_pump_set_state);

    mcp_register_tool("self.sensor.get_environment",
        "Doc nhiet do, do am, ap suat, PM2.5 va CO2 hien tai trong nha",
        "{\"type\":\"object\",\"properties\":{}}", false,
        tool_sensor_get_environment);
}