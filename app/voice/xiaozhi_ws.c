#include "xiaozhi_ws.h"
#include "debug.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "portmacro.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "xiaozhi_mcp.h"
/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME   "xiaozhi_ws"

#define XZ_OTA_URL          "https://api.tenclass.net/xiaozhi/ota/"
#define BOARD_NAME          "longnt_custom_board"
#define XZ_PROTOCOL_VERSION 3

#define XZ_URL_MAX          (160)
#define XZ_TOKEN_MAX        (160)
#define XZ_DEVID_MAX        (32)
#define XZ_UUID_MAX         (48)
#define XZ_UA_MAX           (64)
#define XZ_HEADERS_MAX      (512)
#define XZ_OTA_BUF_SIZE     (1024)
#define XZ_SESSION_ID_SIZE  (40)

#define XZ_FRAME_TYPE_AUDIO (0)
#define XZ_FRAME_HDR_SIZE   (4)

/******************************* DATA TYPES *******************************/
#pragma pack(1)
typedef struct {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t payload_size;   /* big-endian */
} xz_bin_header_t;
#pragma pack()

/******************************* VARIABLES *******************************/
static esp_websocket_client_handle_t s_ws;
static bool       s_connected;
static bool       s_ready;

static char       s_url[XZ_URL_MAX];
static char       s_token[XZ_TOKEN_MAX];
static char       s_device_id[XZ_DEVID_MAX];
static char       s_uuid[XZ_UUID_MAX];
static char       s_user_agent[XZ_UA_MAX];
static char       s_headers[XZ_HEADERS_MAX];
static char       s_session_id[XZ_SESSION_ID_SIZE];

static xz_audio_cb_t s_audio_cb;
static xz_msg_cb_t   s_msg_cb;

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void xz_ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
static void xz_handle_text(const char *json_str);
static void xz_handle_binary(const uint8_t *data, int len);
static void xz_send_hello(void);
static APP_RESULT xz_prepare_device_info(void);
static APP_RESULT xz_load_config(void);
static APP_RESULT xz_activate(void);

static void xz_set_str(char *dst, size_t dst_sz, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_sz, "%s", src);
}

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
void xz_set_callbacks(xz_audio_cb_t audio_cb, xz_msg_cb_t msg_cb)
{
    s_audio_cb = audio_cb;
    s_msg_cb   = msg_cb;
}

/* Trạng thái THẬT của link. KHÔNG dùng cờ `s_connected` trần: khi server đóng
 * hoặc TCP bị half-open, cờ đó còn true trong lúc client đã mất kết nối ->
 * xz_send_audio() vẫn gọi send_bin() mỗi frame 60 ms và spam
 *   E websocket_client: Websocket client is not connected
 *   W xiaozhi_ws: send_audio failed (-1), opus=...
 * esp_websocket_client_is_connected() phản ánh đúng state nội bộ của client. */
static bool xz_link_alive(void)
{
    return (s_ws != NULL) && s_connected &&
           esp_websocket_client_is_connected(s_ws);
}

bool xz_is_connected(void) { return xz_link_alive(); }
bool xz_is_ready(void)     { return s_ready && xz_link_alive(); }

APP_RESULT xz_init(void)
{
    memset(s_url, 0, sizeof(s_url));
    memset(s_token, 0, sizeof(s_token));
    memset(s_headers, 0, sizeof(s_headers));
    s_ws        = NULL;
    s_connected = false;
    s_ready     = false;
    s_session_id[0] = '\0';
    if (xz_prepare_device_info() != APP_OK) {
        return APP_ERROR;
    }
    return APP_OK;
}

/* Lấy Device-Id (MAC) + Client-Id (UUID v4) và lưu vào NVS namespace "device". */
static APP_RESULT xz_prepare_device_info(void)
{
    uint8_t mac[6] = {0};
    nvs_handle_t h;
    size_t len;
    esp_app_desc_t const *app_desc = esp_app_get_description();

    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return APP_ERROR;
    }
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (nvs_open("device", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "Cannot open NVS 'device'");
        return APP_ERROR;
    }
    len = sizeof(s_uuid);
    if (nvs_get_str(h, "uuid", s_uuid, &len) != ESP_OK || s_uuid[0] == '\0') {
        uint32_t r1 = esp_random();
        uint32_t r2 = esp_random();
        snprintf(s_uuid, sizeof(s_uuid), "%08lx-%04lx-4%03lx-%04lx-%04x%08lx",
                 (unsigned long)r1,
                 (unsigned long)(r2 >> 16),
                 (unsigned long)(r2 & 0x0FFF),
                 (unsigned long)((esp_random() & 0x3FFF) | 0x8000),
                 (unsigned int)((mac[0] << 8) | mac[1]),
                 (unsigned long)((mac[2] << 24) | (mac[3] << 16) | (mac[4] << 8) | mac[5]));
        nvs_set_str(h, "uuid", s_uuid);
        nvs_commit(h);
    }
    /* Lưu luôn device id để lần sau dùng lại (giống SmartClock). */
    nvs_set_str(h, "deviceid", s_device_id);
    nvs_commit(h);
    nvs_close(h);

    snprintf(s_user_agent, sizeof(s_user_agent), "%s/%s",
             BOARD_NAME, app_desc ? app_desc->version : "1.0.0");

    ESP_LOGI(THIS_MODULE_NAME, "Device-Id=%s Client-Id=%s UA=%s",
             s_device_id, s_uuid, s_user_agent);
    return APP_OK;
}

/* Đọc url/token từ NVS. */
static APP_RESULT xz_load_config(void)
{
    nvs_handle_t h;
    size_t len;

    if (nvs_open("websocket", NVS_READONLY, &h) != ESP_OK) {
        return APP_ERROR;
    }
    len = sizeof(s_url);
    if (nvs_get_str(h, "url", s_url, &len) != ESP_OK) {
        nvs_close(h);
        return APP_ERROR;
    }
    len = sizeof(s_token);
    if (nvs_get_str(h, "token", s_token, &len) != ESP_OK) {
        nvs_close(h);
        return APP_ERROR;
    }
    nvs_close(h);
    ESP_LOGI(THIS_MODULE_NAME, "WS config from NVS: %s", s_url);
    return APP_OK;
}

/******************************* OTA ACTIVATION *******************************/
static char s_ota_buf[XZ_OTA_BUF_SIZE];
static int  s_ota_len;

static esp_err_t xz_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        s_ota_len = 0;
        s_ota_buf[0] = '\0';
    } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (s_ota_len + evt->data_len < (int)sizeof(s_ota_buf)) {
            memcpy(s_ota_buf + s_ota_len, evt->data, evt->data_len);
            s_ota_len += evt->data_len;
            s_ota_buf[s_ota_len] = '\0';
        }
    }
    return ESP_OK;
}

static APP_RESULT xz_activate(void)
{
    esp_http_client_config_t cfg = {0};
    esp_http_client_handle_t client;
    esp_chip_info_t chip_info;
    char body[256];
    uint32_t flash_mb;
    int status;
    cJSON *root, *websocket, *url, *token, *activation, *code;
    nvs_handle_t h;

    esp_chip_info(&chip_info);
    {
        uint32_t flash_bytes = 0;
        if (esp_flash_get_size(esp_flash_default_chip, &flash_bytes) != ESP_OK ||
            flash_bytes == 0) {
            flash_bytes = 4u * 1024u * 1024u;   /* fallback nếu không đọc được */
        }
        flash_mb = flash_bytes / (1024u * 1024u);
    }

    snprintf(body, sizeof(body),
             "{\"application\":{\"version\":\"1.6.2\",\"board_type\":\"esp32s3-devkitc-1\"},"
             "\"chip\":{\"model\":%d,\"revision\":%d},"
             "\"flash_size\":%u}",
             (int)chip_info.model, chip_info.revision, (unsigned)flash_mb);

    cfg.url = XZ_OTA_URL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.event_handler = xz_http_event;

    client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "OTA: cannot create http client");
        return APP_ERROR;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id",          s_device_id);
    esp_http_client_set_header(client, "Client-Id",          s_uuid);
    esp_http_client_set_header(client, "User-Agent",         s_user_agent);
    esp_http_client_set_header(client, "Accept-Language",    "zh-CN");
    esp_http_client_set_header(client, "Content-Type",       "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(THIS_MODULE_NAME, "OTA: heap 8bit=%u internal=%u (largest=%u) dma=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));

    esp_err_t err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(THIS_MODULE_NAME, "OTA activation failed (err=%s, http=%d)",
                 esp_err_to_name(err), status);
        return APP_ERROR;
    }

    root = cJSON_Parse(s_ota_buf);
    if (root == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "OTA: bad JSON response");
        return APP_ERROR;
    }

    websocket = cJSON_GetObjectItem(root, "websocket");
    url   = websocket ? cJSON_GetObjectItem(websocket, "url")   : NULL;
    token = websocket ? cJSON_GetObjectItem(websocket, "token") : NULL;

    if (!cJSON_IsString(url) || !cJSON_IsString(token)) {
        activation = cJSON_GetObjectItem(root, "activation");
        code = activation ? cJSON_GetObjectItem(activation, "code") : NULL;
        if (cJSON_IsString(code)) {
            ESP_LOGW(THIS_MODULE_NAME,
                     "Device chua kich hoat. Ma kich hoat: %s -> nhap tai xiaozhi.me",
                     code->valuestring);
        } else {
            ESP_LOGE(THIS_MODULE_NAME, "OTA: no websocket url/token in response");
        }
        cJSON_Delete(root);
        return APP_ERROR;
    }

    xz_set_str(s_url, sizeof(s_url), url->valuestring);
    xz_set_str(s_token, sizeof(s_token), token->valuestring);
    cJSON_Delete(root);

    if (nvs_open("websocket", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "url", s_url);
        nvs_set_str(h, "token", s_token);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(THIS_MODULE_NAME, "Da kich hoat, WS url: %s", s_url);
    return APP_OK;
}

/******************************* WEBSOCKET *******************************/
static void xz_send_hello(void)
{
    /* PHAI la JSON hop le: ban truoc thieu dau '}' dong object goc va thua
     * dau phay cuoi chuoi -> server tra ngay
     *   {"type":"error","message":"Error occurred while processing message"}
     * sau khi nhan hello. */
    const char *hello =
        "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
        "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,"
        "\"channels\":1,\"frame_duration\":60},"
        "\"features\":{\"mcp\":true}}";
    ESP_LOGI(THIS_MODULE_NAME, "Sending hello: %s", hello);
    esp_websocket_client_send_text(s_ws, hello, strlen(hello), portMAX_DELAY);
}

APP_RESULT xz_mcp_send_payload(const char* payload) {
    APP_RESULT ret = APP_OK;
    char * msg;
    size_t total_size;
    int    n;

    if (payload == NULL || s_ws == NULL) {
        return APP_ERROR;
    }

    /* JSON phai hop le va "payload" phai la OBJECT (khop voi luc parse trong
     * xz_handle_text + dung protocol xiaozhi). Ban truoc: thieu {} bao ngoai,
     * de payload trong ngoac kep (thanh string) va gui sai do dai
     * (total_size thay vi strlen) -> server bao loi khi nhan message MCP. */
    total_size = sizeof(s_session_id) + strlen(payload) + 64;
    msg = (char *)malloc(total_size);
    ASSERT_CRITICAL(msg != NULL, APP_ERROR, APP_ERROR);

    n = snprintf(msg, total_size,
                 "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
                 s_session_id, payload);
    ret = esp_websocket_client_send_text(s_ws, msg, n, portMAX_DELAY);
    free(msg);
    return ret >= 0 ? APP_OK : APP_ERROR;
}

static void xz_ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(THIS_MODULE_NAME, "WebSocket connected");
        s_connected = true;
        xz_send_hello();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(THIS_MODULE_NAME, "WebSocket disconnected");
        s_connected = false;
        s_ready = false;
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x01) {                    /* text (JSON) */
            if (data->payload_len != data->data_len) break;   /* bỏ frame bị phân mảnh */
            char *js = (char *)malloc(data->data_len + 1);
            if (js != NULL) {
                memcpy(js, data->data_ptr, data->data_len);
                js[data->data_len] = '\0';
                xz_handle_text(js);
                free(js);
            }
        } else if (data->op_code == 0x02) {              /* binary (Opus) */
            if (data->payload_len != data->data_len) break;
            xz_handle_binary((const uint8_t *)data->data_ptr, data->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(THIS_MODULE_NAME, "WebSocket error");
        break;

    default:
        break;
    }
}

static void xz_handle_text(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    cJSON *type, *text, *state, *s_id;

    if (root == NULL) {
        ESP_LOGW(THIS_MODULE_NAME, "JSON parse failed: %s", json_str);
        return;
    }
    type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(THIS_MODULE_NAME, "Control: %s", json_str);

    text  = cJSON_GetObjectItem(root, "text");
    state = cJSON_GetObjectItem(root, "state");

    if (strcmp(type->valuestring, "hello") == 0) {
        s_ready = true;
        // save current session id
        s_id = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(s_id)) {
            xz_set_str(s_session_id, sizeof(s_session_id), s_id->valuestring);
        }
    } else if (strcmp(type->valuestring, "goodbye") == 0) {
        s_ready = false;
    } else if (strcmp(type->valuestring, "tts") == 0) {
        if (!cJSON_IsString(state)) {
            state = NULL;
        }
    } else if (strcmp(type->valuestring, "mcp") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (cJSON_IsObject(payload)) {
            mcp_handle_message(payload);
        }
        cJSON_Delete(root);
        return; 
    }

    if (s_msg_cb != NULL) {
        s_msg_cb(type->valuestring,
                 cJSON_IsString(text)  ? text->valuestring  : NULL,
                 cJSON_IsString(state) ? state->valuestring : NULL);
    }
    cJSON_Delete(root);
}

static void xz_handle_binary(const uint8_t *data, int len)
{
    const xz_bin_header_t *hdr = (const xz_bin_header_t *)data;
    uint16_t payload_size;
    const uint8_t *payload;
    size_t avail;

    if (len <= (int)sizeof(xz_bin_header_t)) {
        return;
    }
    if (hdr->type != XZ_FRAME_TYPE_AUDIO) {
        return;
    }
    payload_size = (uint16_t)((hdr->payload_size >> 8) | (hdr->payload_size << 8));
    payload = data + sizeof(xz_bin_header_t);
    avail = (size_t)(len - (int)sizeof(xz_bin_header_t));
    if (payload_size > avail) {
        payload_size = (uint16_t)avail;
    }
    if (s_audio_cb != NULL && payload_size > 0) {
        s_audio_cb(payload, payload_size);
    }
}

APP_RESULT xz_connect(void)
{
    if (s_connected) {
        return APP_OK;
    }
    if (s_url[0] == '\0' || xz_load_config() != APP_OK) {
        if (xz_activate() != APP_OK) {
            return APP_ERROR;
        }
    }

    snprintf(s_headers, sizeof(s_headers),
             "Authorization: Bearer %s\r\n"
             "Protocol-Version: %d\r\n"
             "Device-Id: %s\r\n"
             "Client-Id: %s\r\n",
             s_token, XZ_PROTOCOL_VERSION, s_device_id, s_uuid);

    esp_websocket_client_config_t cfg = {0};
    cfg.uri = s_url;
    cfg.headers = s_headers;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    /* Callback WEBSOCKET_EVENT_DATA chay TRONG task nay. KHONG duoc opus_decode
     * tai day: decode can >8KB stack, gay "Stack canary watchpoint triggered
     * (websocket_task)", con nang len 16KB thi "Error create websocket task"
     * (RAM noi bo chi con ~31KB, largest block ~15KB). on_xz_audio() chi copy
     * frame vao ring buffer; task voice_rx (stack PSRAM) moi decode.
     * Stack nay chi can du cho TLS handshake + copy frame. */
    cfg.task_stack = 8192;
    cfg.reconnect_timeout_ms = 5000;
    cfg.network_timeout_ms = 10000;

    s_ws = esp_websocket_client_init(&cfg);
    if (s_ws == NULL) {
        ESP_LOGE(THIS_MODULE_NAME, "websocket init failed");
        return APP_ERROR;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, xz_ws_event, NULL);

    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "websocket start failed");
        return APP_ERROR;
    }
    ESP_LOGI(THIS_MODULE_NAME, "Connecting to %s ...", s_url);
    return APP_OK;
}

void xz_disconnect(void)
{
    if (s_ws != NULL) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
    s_ready = false;
}

APP_RESULT xz_send_listen_start(void)
{
    const char *msg = "{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}";
    if (!xz_link_alive()) return APP_ERROR;
    esp_websocket_client_send_text(s_ws, msg, strlen(msg), portMAX_DELAY);
    return APP_OK;
}

APP_RESULT xz_send_listen_stop(void)
{
    const char *msg = "{\"type\":\"listen\",\"state\":\"stop\"}";
    if (!xz_link_alive()) return APP_ERROR;
    esp_websocket_client_send_text(s_ws, msg, strlen(msg), portMAX_DELAY);
    return APP_OK;
}

APP_RESULT xz_send_audio(const uint8_t *opus, size_t len)
{
    uint8_t *packet;
    xz_bin_header_t *hdr;
    size_t total;
    int ret;

    if (!xz_link_alive() || opus == NULL || len == 0 || len > 0xFFFF) {
        return APP_ERROR;
    }
    total = sizeof(xz_bin_header_t) + len;
    packet = (uint8_t *)malloc(total);
    if (packet == NULL) {
        return APP_ERROR;
    }
    hdr = (xz_bin_header_t *)packet;
    hdr->type = XZ_FRAME_TYPE_AUDIO;
    hdr->reserved = 0;
    hdr->payload_size = (uint16_t)(((len & 0xFF) << 8) | ((len >> 8) & 0xFF));
    memcpy(packet + sizeof(xz_bin_header_t), opus, len);

    ret = esp_websocket_client_send_bin(s_ws, (const char *)packet, total, portMAX_DELAY);
    free(packet);
    if (ret < 0) {
        /* Chỉ coi là MẤT KẾT NỐI khi client thực sự báo disconnected; lỗi tạm
         * thời (buffer đầy...) không được phá phiên. Khi mất kết nối thật thì
         * hạ cờ ngay để mic_task dừng gửi và chờ esp_websocket_client tự
         * reconnect (reconnect_timeout_ms). */
        if (s_ws == NULL || !esp_websocket_client_is_connected(s_ws)) {
            s_connected = false;
            s_ready     = false;
        }
        /* Throttle: tối đa 1 dòng log / 2 s (tránh spam mỗi 60 ms). */
        static TickType_t s_last_warn_tick = 0;
        TickType_t now_tick = xTaskGetTickCount();
        if ((now_tick - s_last_warn_tick) >= pdMS_TO_TICKS(2000)) {
            s_last_warn_tick = now_tick;
            ESP_LOGW(THIS_MODULE_NAME, "send_audio failed (%d), opus=%u B",
                     ret, (unsigned)len);
        }
        return APP_ERROR;
    }
    return APP_OK;
}
