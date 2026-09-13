#include "wifi_control.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "wifi_control"

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

/* Mạng mặc định từ Kconfig dùng khi chưa có profile lưu trong NVS. */
#define KCONFIG_SSID         CONFIG_NETWORK_WIFI_SSID
#define KCONFIG_PASSWORD     CONFIG_NETWORK_WIFI_PASSWORD
#define WIFI_MAX_RETRY       CONFIG_NETWORK_WIFI_MAXIMUM_RETRY

#define NVS_NS               "wifi_sta"
#define NVS_KEY_SSID         "ssid"
#define NVS_KEY_PASS         "pass"

/* Chu kỳ tự kết nối lại khi đang ở trạng thái RECONNECTING. */
#define RECONNECT_PERIOD_US  (3 * 1000 * 1000)
/******************************* FUNCTIONS PROTOTYPE *******************************/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
static void reconnect_timer_cb(void *arg);
static void wifi_lock(void);
static void wifi_unlock(void);
static void wifi_set_profile(const char *ssid, const char *password);
static void profile_save_to_nvs(const char *ssid, const char *password);
static bool profile_load_from_nvs(void);
static void profile_erase_from_nvs(void);
static void wifi_apply_config(const char *ssid, const char *password);
/******************************* DATA TYPES *******************************/
typedef struct {
    bool is_connected;
    bool started;
    bool user_disconnect;      /* người dùng chủ động ngắt -> không tự reconnect */
    bool profile_valid;        /* có profile muốn kết nối (đã lưu/Kconfig) */
    uint8_t retry_num;
    wifi_state_t state;
    char profile_ssid[33];
    char profile_pass[65];
    char ip_str[16];
    /* quét mạng */
    bool scanning;
    bool scan_done;
} wifi_t;
/******************************* VARIABLES *******************************/
static EventGroupHandle_t s_wifi_event_group;
static SemaphoreHandle_t s_wifi_mutex;
static esp_timer_handle_t s_reconnect_timer;
static wifi_t wifi_control;
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void wifi_lock(void)
{
    if (s_wifi_mutex) {
        xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (s_wifi_mutex) {
        xSemaphoreGive(s_wifi_mutex);
    }
}

static void wifi_set_profile(const char *ssid, const char *password)
{
    if (ssid) {
        snprintf(wifi_control.profile_ssid, sizeof(wifi_control.profile_ssid), "%s", ssid);
        wifi_control.profile_valid = (wifi_control.profile_ssid[0] != '\0');
    }
    if (password) {
        snprintf(wifi_control.profile_pass, sizeof(wifi_control.profile_pass), "%s", password);
    }
}

static void wifi_apply_config(const char *ssid, const char *password)
{
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    if (ssid) {
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    }
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    /* Chấp nhận mọi mức bảo mật (kể cả mạng mở), driver tự chọn theo AP. */
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_LOGI(THIS_MODULE_NAME, "apply config: ssid=%s, password_len=%u",
             wifi_config.sta.ssid, (unsigned)strlen((char *)wifi_config.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void profile_save_to_nvs(const char *ssid, const char *password)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, password);
    nvs_commit(h);
    nvs_close(h);
}

static bool profile_load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(wifi_control.profile_ssid);
    bool ok = (nvs_get_str(h, NVS_KEY_SSID, wifi_control.profile_ssid, &len) == ESP_OK);
    if (ok) {
        len = sizeof(wifi_control.profile_pass);
        if (nvs_get_str(h, NVS_KEY_PASS, wifi_control.profile_pass, &len) != ESP_OK) {
            wifi_control.profile_pass[0] = '\0';
        }
        wifi_control.profile_valid = (wifi_control.profile_ssid[0] != '\0');
    }
    nvs_close(h);
    return ok && wifi_control.profile_valid;
}

static void profile_erase_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, NVS_KEY_SSID);
    nvs_erase_key(h, NVS_KEY_PASS);
    nvs_commit(h);
    nvs_close(h);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    wifi_lock();
    bool do_connect = (wifi_control.state == WIFI_STATE_RECONNECTING) &&
                      !wifi_control.is_connected && wifi_control.started;
    wifi_unlock();
    if (do_connect) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(THIS_MODULE_NAME, "reconnect call failed: %s", esp_err_to_name(err));
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_START) {
        wifi_lock();
        if (wifi_control.profile_valid) {
            wifi_control.state = WIFI_STATE_CONNECTING;
            wifi_control.retry_num = 0;
            wifi_unlock();
            esp_wifi_connect();
            ESP_LOGI(THIS_MODULE_NAME, "auto connect to saved profile: %s",
                     wifi_control.profile_ssid);
        } else {
            wifi_control.state = WIFI_STATE_IDLE;
            wifi_unlock();
            ESP_LOGI(THIS_MODULE_NAME, "no profile, waiting for user to pick a network");
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc != NULL) {
            const char *why = "other";
            switch (disc->reason) {
            case WIFI_REASON_AUTH_EXPIRE:          why = "AUTH_EXPIRE"; break;
            case WIFI_REASON_AUTH_FAIL:            why = "AUTH_FAIL"; break;
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: why = "4WAY_HANDSHAKE_TIMEOUT (sai mat khau?)"; break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT:    why = "HANDSHAKE_TIMEOUT"; break;
            case WIFI_REASON_NO_AP_FOUND:          why = "NO_AP_FOUND"; break;
            case WIFI_REASON_ASSOC_EXPIRE:         why = "ASSOC_EXPIRE"; break;
            case WIFI_REASON_BEACON_TIMEOUT:       why = "BEACON_TIMEOUT"; break;
            default:                               why = "other"; break;
            }
            ESP_LOGW(THIS_MODULE_NAME, "disconnected: reason=%d (%s)", disc->reason, why);
        }
        wifi_lock();
        wifi_control.is_connected = false;
        if (wifi_control.user_disconnect) {
            /* Người dùng chủ động ngắt -> dừng hẳn, không tự reconnect. */
            wifi_control.user_disconnect = false;
            wifi_control.state = WIFI_STATE_IDLE;
            wifi_unlock();
            ESP_LOGI(THIS_MODULE_NAME, "disconnected by user");
        } else if (wifi_control.state == WIFI_STATE_CONNECTING) {
            if (wifi_control.retry_num < WIFI_MAX_RETRY) {
                wifi_control.retry_num++;
                wifi_unlock();
                ESP_LOGW(THIS_MODULE_NAME, "connect failed, retrying %d/%d",
                         wifi_control.retry_num, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                wifi_control.state = WIFI_STATE_IDLE;
                wifi_unlock();
                ESP_LOGE(THIS_MODULE_NAME, "connect failed, giving up");
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (wifi_control.state == WIFI_STATE_CONNECTED ||
                   wifi_control.state == WIFI_STATE_RECONNECTING) {
            wifi_control.state = WIFI_STATE_RECONNECTING;
            wifi_unlock();
            if (s_reconnect_timer) {
                esp_timer_stop(s_reconnect_timer);
                esp_timer_start_periodic(s_reconnect_timer, RECONNECT_PERIOD_US);
            }
            ESP_LOGW(THIS_MODULE_NAME, "connection lost, auto reconnecting...");
        } else {
            wifi_unlock();   /* IDLE/OFF: không làm gì */
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        wifi_lock();
        wifi_control.scan_done = true;
        wifi_unlock();
        ESP_LOGI(THIS_MODULE_NAME, "scan done event");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_lock();
        wifi_control.is_connected = true;
        wifi_control.retry_num = 0;
        wifi_control.state = WIFI_STATE_CONNECTED;
        esp_ip4addr_ntoa(&event->ip_info.ip, wifi_control.ip_str, sizeof(wifi_control.ip_str));
        wifi_unlock();
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        ESP_LOGI(THIS_MODULE_NAME, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

APP_RESULT wifi_control_init() {
    APP_RESULT ret = APP_OK;
    memset((void *)&wifi_control, 0, sizeof(wifi_control));
    wifi_control.state = WIFI_STATE_OFF;

    s_wifi_mutex = xSemaphoreCreateMutex();
    ASSERT_CRITICAL(s_wifi_mutex != NULL, 0, APP_ERROR);

    s_wifi_event_group = xEventGroupCreate();
    ASSERT_CRITICAL(s_wifi_event_group != NULL, 0, APP_ERROR);

    // init nvs
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &ip_event_handler, NULL, &instance_got_ip));

    /* Profile: ưu tiên đã lưu trong NVS; nếu chưa có thì dùng Kconfig mặc định. */
    // if (!profile_load_from_nvs()) 
    {
        if (strcmp(KCONFIG_SSID, "YOUR_WIFI_SSID") != 0 && strlen(KCONFIG_SSID) > 0) {
            wifi_set_profile(KCONFIG_SSID, KCONFIG_PASSWORD);
            ESP_LOGI(THIS_MODULE_NAME, "no saved profile, use Kconfig default");
        }
    }

    /* esp_timer định kỳ giúp tự kết nối lại sau khi rớt mạng (chỉ chạy khi RECONNECTING). */
    esp_timer_create_args_t targs = {
        .callback = reconnect_timer_cb,
        .name = "wifi_rc",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_apply_config(wifi_control.profile_valid ? wifi_control.profile_ssid : NULL,
                      wifi_control.profile_valid ? wifi_control.profile_pass : NULL);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(THIS_MODULE_NAME, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return APP_ERROR;
    }

    /* Tắt modem sleep: nhiều router lam ESP32-S3 associate xong roi rot ngay
     * neu de power save mac dinh. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_lock();
    wifi_control.started = true;
    wifi_control.state = WIFI_STATE_IDLE;   /* STA_START handler sẽ chuyển CONNECTING nếu có profile */
    wifi_unlock();

    ESP_LOGI(THIS_MODULE_NAME, "wifi init finished, profile: %s",
             wifi_control.profile_valid ? wifi_control.profile_ssid : "(none)");
    return ret;
}

APP_RESULT wifi_control_wait_connected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return APP_OK;
    }
    return APP_ERROR;
}

bool wifi_control_is_connected() {
    bool r;
    wifi_lock();
    r = wifi_control.is_connected;
    wifi_unlock();
    return r;
}

wifi_state_t wifi_control_get_state() {
    wifi_state_t s;
    wifi_lock();
    s = wifi_control.state;
    wifi_unlock();
    return s;
}

const char *wifi_control_get_connected_ssid() {
    const char *r = "";
    wifi_lock();
    if (wifi_control.is_connected) {
        r = wifi_control.profile_ssid;
    }
    wifi_unlock();
    return r;
}

APP_RESULT wifi_control_get_ip(char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return APP_ERROR;
    }
    buf[0] = '\0';
    wifi_lock();
    bool c = wifi_control.is_connected;
    if (c) {
        snprintf(buf, len, "%s", wifi_control.ip_str);
    }
    wifi_unlock();
    return c ? APP_OK : APP_ERROR;
}

APP_RESULT wifi_control_connect(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        return APP_ERROR;
    }
    const char *pass = (password != NULL) ? password : "";

    wifi_lock();
    if (!wifi_control.started) {
        wifi_unlock();
        return APP_ERROR;
    }
    bool was_connected = wifi_control.is_connected;
    wifi_set_profile(ssid, pass);
    wifi_control.retry_num = 0;
    wifi_control.user_disconnect = false;
    wifi_control.state = WIFI_STATE_CONNECTING;
    wifi_unlock();

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    profile_save_to_nvs(ssid, pass);
    wifi_apply_config(ssid, pass);

    ESP_LOGI(THIS_MODULE_NAME, "connecting to ssid: %s (len pwd=%d)", ssid, (int)strlen(pass));

    if (was_connected) {
        /* Đang kết nối mạng khác -> ngắt để sự kiện DISCONNECTED (với config mới)
           sẽ tự kết nối lại qua handler. */
        esp_wifi_disconnect();
    } else {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(THIS_MODULE_NAME, "esp_wifi_connect: %s", esp_err_to_name(err));
            return APP_ERROR;
        }
    }
    return APP_OK;
}

void wifi_control_disconnect(void) {
    wifi_lock();
    wifi_control.user_disconnect = true;
    wifi_control.state = WIFI_STATE_IDLE;
    wifi_control.is_connected = false;
    wifi_unlock();
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    esp_wifi_disconnect();
    ESP_LOGI(THIS_MODULE_NAME, "disconnect requested");
}

void wifi_control_forget(void) {
    wifi_control_disconnect();
    wifi_lock();
    wifi_control.profile_valid = false;
    wifi_control.profile_ssid[0] = '\0';
    wifi_control.profile_pass[0] = '\0';
    wifi_unlock();
    profile_erase_from_nvs();
    ESP_LOGI(THIS_MODULE_NAME, "forgot saved network");
}

void wifi_control_abort_auto_reconnect(void) {
    wifi_lock();
    wifi_control.retry_num = 0;
    wifi_control.user_disconnect = false;
    wifi_control.state = WIFI_STATE_IDLE;
    wifi_unlock();
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    ESP_LOGI(THIS_MODULE_NAME, "auto reconnect stopped (user wants to scan/change)");
}

APP_RESULT wifi_control_scan_stop(void) {
    esp_err_t err = esp_wifi_scan_stop();
    wifi_lock();
    wifi_control.scanning = false;
    wifi_control.scan_done = false;
    wifi_unlock();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(THIS_MODULE_NAME, "scan stop: %s", esp_err_to_name(err));
    }
    return APP_OK;
}

APP_RESULT wifi_control_scan_start(void) {
    wifi_lock();
    if (!wifi_control.started) {
        wifi_unlock();
        return APP_ERROR;
    }
    if (wifi_control.scanning) {
        wifi_unlock();
        return APP_ERROR;
    }
    wifi_control.scanning = true;
    wifi_control.scan_done = false;
    wifi_unlock();

    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ssid = NULL;
    cfg.bssid = NULL;
    cfg.channel = 0;
    cfg.show_hidden = true;
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 100;
    cfg.scan_time.active.max = 300;

    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        wifi_lock();
        wifi_control.scanning = false;
        wifi_unlock();
        ESP_LOGW(THIS_MODULE_NAME, "scan start failed: %s", esp_err_to_name(err));
        return APP_ERROR;
    }
    ESP_LOGI(THIS_MODULE_NAME, "scan started");
    return APP_OK;
}

bool wifi_control_scan_is_done(void) {
    bool r;
    wifi_lock();
    r = wifi_control.scan_done;
    wifi_unlock();
    return r;
}

uint16_t wifi_control_scan_get_results(wifi_ap_info_t *out, uint16_t max) {
    if (out == NULL || max == 0) {
        return 0;
    }
    wifi_lock();
    if (!wifi_control.scan_done) {
        wifi_unlock();
        return 0;
    }
    wifi_control.scanning = false;
    wifi_control.scan_done = false;
    wifi_unlock();

    uint16_t count = (max < WIFI_MAX_AP) ? max : WIFI_MAX_AP;
    wifi_ap_record_t recs[WIFI_MAX_AP];
    memset(recs, 0, sizeof(recs));
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, recs);
    if (err != ESP_OK) {
        ESP_LOGW(THIS_MODULE_NAME, "get ap records failed: %s", esp_err_to_name(err));
        return 0;
    }
    ESP_LOGI(THIS_MODULE_NAME, "ap records: %u", (unsigned)count);
    for (uint16_t i = 0; i < count; i++) {
        wifi_ap_info_t *d = &out[i];
        memset(d, 0, sizeof(*d));
        strncpy(d->ssid, (const char *)recs[i].ssid, sizeof(d->ssid) - 1);
        memcpy(d->bssid, recs[i].bssid, 6);
        d->rssi = recs[i].rssi;
        d->channel = recs[i].primary;
        d->authmode = recs[i].authmode;
    }
    return count;
}
