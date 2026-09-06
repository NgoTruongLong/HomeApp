/*
 * lvgl_driver.c
 *
 * LVGL v8 glue for HomeApp (C port of smartClock/source/lcd/lvgl_control.cpp).
 * - lv_disp_drv đăng ký với callback flush -> esp_lcd_panel_draw_bitmap
 * - esp_timer cấp tick cho LVGL (2 ms)
 * - indev pointer đọc từ XPT2046 (esp_lcd_touch)
 * - Task riêng chạy lv_timer_handler; mọi thao tác widget phải qua
 *   lvgl_driver_lock()/unlock() vì LVGL không thread-safe.
 */

#include "lvgl_driver.h"
#include "lcd_driver.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "lvgl_driver"

#define LVGL_DRAW_BUF_PIXELS  (LCD_H_RES * LCD_DRAW_BUF_LINES)   /* 480*20 */

/* Mapping toạ độ touch -> màn hình (hiệu chỉnh theo smartClock). */
#define TOUCH_X_RES_MIN (15)
#define TOUCH_X_RES_MAX (454)
#define TOUCH_Y_RES_MIN (16)
#define TOUCH_Y_RES_MAX (298)

#define TOUCH_DEBUG_PRINT (0)  /* bật = in toạ độ touch ra console */

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void lvgl_task(void *arg);
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);
static void lvgl_tick_cb(void *arg);
static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data);
static uint16_t lvgl_map_value(uint16_t x, uint16_t in_min, uint16_t in_max,
                               uint16_t out_min, uint16_t out_max);

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp = NULL;
static lv_indev_drv_t s_indev_drv;
static SemaphoreHandle_t s_lvgl_mutex = NULL;

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT lvgl_driver_init(void)
{
    lv_init();

    /* 2 draw buffer (double buffer) để LVGL vẽ trong khi SPI đang flush. */
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LVGL_DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA);
    ASSERT_CRITICAL(buf1, APP_ERROR, APP_ERROR);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(LVGL_DRAW_BUF_PIXELS * sizeof(lv_color_t), MALLOC_CAP_DMA);
    ASSERT_CRITICAL(buf2, APP_ERROR, APP_ERROR);

    lv_disp_draw_buf_init(&s_disp_buf, buf1, buf2, LVGL_DRAW_BUF_PIXELS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res    = LCD_H_RES;
    s_disp_drv.ver_res    = LCD_V_RES;
    s_disp_drv.flush_cb   = lvgl_flush_cb;
    s_disp_drv.draw_buf   = &s_disp_buf;
    s_disp_drv.user_data  = lcd_driver_get_panel();
    s_disp = lv_disp_drv_register(&s_disp_drv);

    /* Tick cho LVGL bằng esp_timer (2 ms). */
    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_err_t err = esp_timer_create(&tick_args, &tick_timer);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    /* Input device: touch XPT2046. */
    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type     = LV_INDEV_TYPE_POINTER;
    s_indev_drv.disp     = s_disp;
    s_indev_drv.read_cb  = lvgl_touch_read_cb;
    s_indev_drv.user_data = lcd_driver_get_touch();
    lv_indev_drv_register(&s_indev_drv);

    /* Mutex đệ quy cho LVGL. */
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ASSERT_CRITICAL(s_lvgl_mutex, APP_ERROR, APP_ERROR);

    ESP_LOGI(THIS_MODULE_NAME, "LVGL init OK (%dx%d)", LCD_H_RES, LCD_V_RES);
    return APP_OK;
}

APP_RESULT lvgl_driver_start(void)
{
    BaseType_t ret = xTaskCreate(lvgl_task, LVGL_TASK_NAME, LVGL_TASK_STACK_SIZE,
                                 NULL, LVGL_TASK_PRIORITY, NULL);
    ASSERT_CRITICAL(ret == pdPASS, ret, APP_ERROR);
    ESP_LOGI(THIS_MODULE_NAME, "LVGL task started");
    return APP_OK;
}

bool lvgl_driver_lock(int timeout_ms)
{
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

void lvgl_driver_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

lv_disp_drv_t *lvgl_driver_get_disp_drv(void)
{
    return &s_disp_drv;
}

lv_disp_t *lvgl_driver_get_disp(void)
{
    return s_disp;
}

static void lvgl_task(void *arg)
{
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_driver_lock(-1)) {
            delay_ms = lv_timer_handler();
            lvgl_driver_unlock();
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) {
            delay_ms = LVGL_TASK_MAX_DELAY_MS;
        } else if (delay_ms < LVGL_TASK_MIN_DELAY_MS) {
            delay_ms = LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = lcd_driver_get_panel();
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    if (tp == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_read_data(tp);

    esp_lcd_touch_point_data_t point = {0};
    uint8_t cnt = 0;
    if (esp_lcd_touch_get_data(tp, &point, &cnt, 1) == ESP_OK && cnt > 0) {
        uint16_t mapped_x = lvgl_map_value(point.x, TOUCH_X_RES_MIN, TOUCH_X_RES_MAX, 0, LCD_H_RES - 1);
        uint16_t mapped_y = lvgl_map_value(point.y, TOUCH_Y_RES_MIN, TOUCH_Y_RES_MAX, 0, LCD_V_RES - 1);
        data->point.x = mapped_x;
        data->point.y = mapped_y;
        data->state   = LV_INDEV_STATE_PRESSED;
#if TOUCH_DEBUG_PRINT
        printf("Touch raw(%d,%d) -> (%d,%d)\r\n", point.x, point.y, mapped_x, mapped_y);
#endif
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint16_t lvgl_map_value(uint16_t x, uint16_t in_min, uint16_t in_max,
                               uint16_t out_min, uint16_t out_max)
{
    if (in_max <= in_min) {
        return out_min;
    }
    if (x < in_min) {
        x = in_min;
    }
    if (x > in_max) {
        x = in_max;
    }
    uint32_t run  = in_max - in_min;
    uint32_t rise = out_max - out_min;
    uint32_t delta = x - in_min;
    return (uint16_t)((delta * rise) / run + out_min);
}
