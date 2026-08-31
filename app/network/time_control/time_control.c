#include "time_control.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_log.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "time_control"

#define TIME_ZONE              "ICT-7"              /* Asia/Ho_Chi_Minh (UTC+7) */
#define TIME_SYNC_MAX_RETRY    (50)                 /* 50 * 500ms = 25s */
#define TIME_SYNC_RETRY_DELAY  (500)
/******************************* FUNCTIONS PROTOTYPE *******************************/
static void time_sync_notification_cb(struct timeval *tv);
/******************************* DATA TYPES *******************************/
typedef struct {
    bool is_synced;
} time_control_state_t;
/******************************* VARIABLES *******************************/
static time_control_state_t time_control;
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void time_sync_notification_cb(struct timeval *tv) {
    ESP_LOGI(THIS_MODULE_NAME, "time synchronization event received");
}

APP_RESULT time_control_init() {
    APP_RESULT ret = APP_OK;
    memset((void *)&time_control, 0, sizeof(time_control));

    // set timezone
    setenv("TZ", TIME_ZONE, 1);
    tzset();

    // init sntp
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_NETWORK_SNTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // wait for time sync
    uint8_t retry = 0;
    sntp_sync_status_t sync_status = esp_sntp_get_sync_status();
    while (sync_status == SNTP_SYNC_STATUS_RESET && retry < TIME_SYNC_MAX_RETRY) {
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_DELAY));
        retry++;
        sync_status = esp_sntp_get_sync_status();
    }

    if (sync_status == SNTP_SYNC_STATUS_RESET) {
        ESP_LOGW(THIS_MODULE_NAME, "time sync failed after %d retries", retry);
        return APP_ERROR;
    }

    time_control.is_synced = true;
    ESP_LOGI(THIS_MODULE_NAME, "time synchronized with server %s", CONFIG_NETWORK_SNTP_SERVER);
    time_control_print_current_time();

    return ret;
}

bool time_control_is_synced() {
    return time_control.is_synced;
}

void time_control_print_current_time() {
    time_t now;
    struct tm timeinfo;
    char time_buf[64];

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    printf("Current time: %s\n", time_buf);
}
