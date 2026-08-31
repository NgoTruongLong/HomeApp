#include "wifi_control.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "wifi_control"

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1

#define WIFI_SSID            CONFIG_NETWORK_WIFI_SSID
#define WIFI_PASSWORD        CONFIG_NETWORK_WIFI_PASSWORD
#define WIFI_MAX_RETRY       CONFIG_NETWORK_WIFI_MAXIMUM_RETRY
/******************************* FUNCTIONS PROTOTYPE *******************************/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);
/******************************* DATA TYPES *******************************/
typedef struct {
    bool is_connected;
    uint8_t retry_num;
} wifi_t;
/******************************* VARIABLES *******************************/
static EventGroupHandle_t s_wifi_event_group;
static wifi_t wifi_control;
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_control.is_connected = false;
        if (wifi_control.retry_num < WIFI_MAX_RETRY) {
            wifi_control.retry_num++;
            ESP_LOGW(THIS_MODULE_NAME, "connect to AP failed, retrying %d/%d",
                     wifi_control.retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(THIS_MODULE_NAME, "connect to AP failed, giving up");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_control.is_connected = true;
        wifi_control.retry_num = 0;
        ESP_LOGI(THIS_MODULE_NAME, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

APP_RESULT wifi_control_init() {
    APP_RESULT ret = APP_OK;
    memset((void *)&wifi_control, 0, sizeof(wifi_control));

    s_wifi_event_group = xEventGroupCreate();
    ASSERT_CRITICAL(s_wifi_event_group != NULL, (int)s_wifi_event_group, APP_ERROR);

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

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(THIS_MODULE_NAME, "wifi init finished, connecting to ssid: %s", WIFI_SSID);

    return ret;
}

APP_RESULT wifi_control_wait_connected(uint32_t timeout_ms) {
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & WIFI_CONNECTED_BIT) != 0) {
        return APP_OK;
    }
    return APP_ERROR;
}

bool wifi_control_is_connected() {
    return wifi_control.is_connected;
}
