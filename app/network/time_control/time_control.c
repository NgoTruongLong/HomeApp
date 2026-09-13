#include "time_control.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "time_control"

#define TIME_ZONE              "ICT-7"              /* Asia/Ho_Chi_Minh (UTC+7) */
#define TIME_SYNC_RETRY_MS     (3000)               /* thử lại SNTP mỗi 3s khi chưa sync */
#define TIME_SYNC_TASK_STACK   (4096)
#define TIME_SYNC_TASK_PRIO    (1)
/******************************* FUNCTIONS PROTOTYPE *******************************/
static void time_sync_notification_cb(struct timeval *tv);
static void time_sync_task(void *arg);
/******************************* DATA TYPES *******************************/
/******************************* VARIABLES *******************************/
static time_control_t time_control;
/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void time_sync_notification_cb(struct timeval *tv) {
    (void)tv;
    /* Đồng bộ thành công (có thể xảy ra muộn, sau khi người dùng nối WiFi). */
    time_control.is_synced = true;
    ESP_LOGI(THIS_MODULE_NAME, "time synchronization event received");
}

/* Task nền: "đánh thức" SNTP liên tục tới khi đồng bộ xong.
   Cần vì WiFi có thể được kết nối muộn (qua màn hình) sau khi boot. */
static void time_sync_task(void *arg) {
    (void)arg;
    while (!time_control.is_synced) {
        esp_sntp_restart();
        vTaskDelay(pdMS_TO_TICKS(TIME_SYNC_RETRY_MS));
    }
    ESP_LOGI(THIS_MODULE_NAME, "time synchronized with server %s", CONFIG_NETWORK_SNTP_SERVER);
    time_control_print_current_time();
    vTaskDelete(NULL);
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
    // sync every hour (3600000 ms) to avoid time drift
    esp_sntp_set_sync_interval(3600000);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    /* Task nền đồng bộ giờ - KHÔNG block boot.
       is_synced sẽ được set true bởi callback khi SNTP thành công. */
    BaseType_t task_ok = xTaskCreate(time_sync_task, "time_sync", TIME_SYNC_TASK_STACK,
                                     NULL, TIME_SYNC_TASK_PRIO, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "failed to create time sync task");
        return APP_ERROR;
    }

    ESP_LOGI(THIS_MODULE_NAME, "SNTP started (async), waiting for network...");
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

APP_RESULT time_control_get_time_str(char *buf, size_t len, const char *fmt) {
    if (buf == NULL || len == 0) {
        return APP_ERROR;
    }

    time_t now;
    time(&now);
    localtime_r(&now, &time_control.current_time);

    // If no format provided, use the default date+time format.
    if (fmt == NULL) {
        fmt = "%Y-%m-%d %H:%M:%S";
    }

    size_t written = strftime(buf, len, fmt, &time_control.current_time);
    if (written == 0) {
        return APP_ERROR;
    }

    return APP_OK;
}
