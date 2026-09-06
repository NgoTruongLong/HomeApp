/*
 * screen_control.c
 *
 * Màn hình HomeApp trên TFT 3.5" ILI9488 480x320 + LVGL v8
 * (port từ smartClock). UI dashboard hiển thị:
 *   - Header "HOME APP" + ngày
 *   - Đồng hồ thời gian thực (time_control)
 *   - Thẻ CO2 (đổi màu theo chất lượng không khí)
 *   - Lưới 6 thông số cảm biến (sensor_get_data)
 *
 * File ST7735 cũ được giữ tại app/screen/st7735_legacy/ (KHÔNG build).
 */

#include "screen_control.h"
#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

#include "lcd_driver.h"
#include "lvgl_driver.h"
#include "sensor_control.h"
#include "time_control.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "screen_control"

#define DASH_TASK_NAME         "screen_task"
#define DASH_TASK_STACK_SIZE   (4096)
#define DASH_TASK_PRIORITY     (4)
#define DASH_REFRESH_PERIOD_MS (1000)

/* --- Màu (RGB888 cho LVGL) --- */
#define COLOR_BG        lv_color_hex(0x000000)  /* nền đen */
#define COLOR_HEADER    lv_color_hex(0x10206B)  /* xanh navy */
#define COLOR_TITLE     lv_color_hex(0xFFFFFF)
#define COLOR_DATE      lv_color_hex(0xB0B0B0)
#define COLOR_LABEL     lv_color_hex(0x9E9E9E)
#define COLOR_PANEL     lv_color_hex(0x171D2A)  /* nền card metric */
#define COLOR_DIVIDER   lv_color_hex(0x3A3A3A)

#define COLOR_TEMP      lv_color_hex(0xFF8A00)  /* cam */
#define COLOR_HUM       lv_color_hex(0x00E5FF)  /* cyan */
#define COLOR_PRES      lv_color_hex(0x38B6FF)  /* xanh dương nhạt */
#define COLOR_PM1       lv_color_hex(0x90A4AE)
#define COLOR_PM25      lv_color_hex(0xFF6FA5)  /* hồng */
#define COLOR_PM10      lv_color_hex(0xFFD54F)  /* vàng */

/* Màu CO2 theo mức chất lượng không khí */
#define CO2_COLOR_GOOD    lv_color_hex(0x00E676)
#define CO2_COLOR_FAIR    lv_color_hex(0xFFEA00)
#define CO2_COLOR_POOR    lv_color_hex(0xFF9100)
#define CO2_COLOR_BAD     lv_color_hex(0xFF1744)
#define CO2_COLOR_INVALID lv_color_hex(0x607D8B)

#define CO2_TEXT_COLOR    lv_color_hex(0x000000)

/* --- Layout (480x320) --- */
#define HEADER_H        (44)
#define GRID_X0         (10)
#define GRID_Y0         (210)
#define GRID_GAP_X      (8)
#define GRID_GAP_Y      (8)
#define GRID_W          ((LCD_H_RES - 2 * GRID_X0 - 2 * GRID_GAP_X) / 3)  /* 148 */
#define GRID_H          (44)

/* --- Font chữ (bật trong sdkconfig.defaults: 14,16,20,36,48) --- */
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_36);
LV_FONT_DECLARE(lv_font_montserrat_48);

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void dash_task(void *arg);
static void ui_create(void);
static void ui_create_metric_cards(void);
static void ui_refresh(void);

/* CO2 quality helpers (giữ logic như bản ST7735 cũ). */
typedef enum {
    CO2_STATUS_GOOD = 0,
    CO2_STATUS_FAIR,
    CO2_STATUS_POOR,
    CO2_STATUS_BAD,
    CO2_STATUS_INVALID,
} co2_status_t;

static co2_status_t co2_get_status(bool valid, int co2);
static lv_color_t co2_status_color(co2_status_t s);
static const char *co2_status_text(co2_status_t s);
static void set_label_if_changed(lv_obj_t *lbl, const char *text);

/******************************* DATA TYPES *******************************/
typedef struct {
    lv_obj_t *value_lbl;
    lv_color_t accent;
} metric_card_t;

/******************************* VARIABLES *******************************/
static lv_obj_t *s_time_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_co2_card;
static lv_obj_t *s_co2_value_lbl;
static lv_obj_t *s_co2_status_lbl;
static metric_card_t s_metrics[6];

/* Buffer văn bản tĩnh (lv_label_set_text giữ con trỏ, không copy). */
static char s_time_str[16];
static char s_date_str[16];
static char s_co2_value[8];
static char s_metric_str[6][16];

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT screen_init()
{
    APP_RESULT ret = APP_OK;

    ret = lcd_driver_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = lvgl_driver_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    /* LVGL task chưa chạy nên tạo UI an toàn (1 luồng). */
    ui_create();

    ret = lvgl_driver_start();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    /* Task cập nhật dữ liệu định kỳ. */
    BaseType_t task_ok = xTaskCreate(dash_task, DASH_TASK_NAME,
                                     DASH_TASK_STACK_SIZE, NULL,
                                     DASH_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to create dashboard task");
        return APP_ERROR;
    }

    ESP_LOGI(THIS_MODULE_NAME, "Dashboard UI started");
    return ret;
}

/* Tạo toàn bộ UI dashboard. Chỉ gọi 1 lần (trước khi LVGL task chạy). */
static void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ---------- Header ---------- */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(header, COLOR_HEADER, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_obj_set_style_text_color(title_lbl, COLOR_TITLE, 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(title_lbl, "HOME APP");
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 14, 0);

    s_date_lbl = lv_label_create(header);
    lv_obj_set_style_text_color(s_date_lbl, COLOR_DATE, 0);
    lv_obj_set_style_text_font(s_date_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_date_lbl, "--/--/----");
    lv_obj_align(s_date_lbl, LV_ALIGN_RIGHT_MID, -12, 0);

    /* ---------- Đồng hồ lớn ---------- */
    s_time_lbl = lv_label_create(scr);
    lv_obj_set_style_text_color(s_time_lbl, COLOR_TITLE, 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_time_lbl, "--:--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 52);

    /* ---------- Thẻ CO2 ---------- */
    s_co2_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_co2_card);
    lv_obj_set_pos(s_co2_card, GRID_X0, 142);
    lv_obj_set_size(s_co2_card, LCD_H_RES - 2 * GRID_X0, 60);
    lv_obj_set_style_bg_color(s_co2_card, CO2_COLOR_INVALID, 0);
    lv_obj_set_style_bg_opa(s_co2_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_co2_card, 8, 0);
    lv_obj_set_style_pad_all(s_co2_card, 0, 0);

    lv_obj_t *co2_name = lv_label_create(s_co2_card);
    lv_obj_set_style_text_color(co2_name, CO2_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(co2_name, &lv_font_montserrat_20, 0);
    lv_label_set_text(co2_name, "CO2");
    lv_obj_align(co2_name, LV_ALIGN_TOP_LEFT, 16, 6);

    s_co2_status_lbl = lv_label_create(s_co2_card);
    lv_obj_set_style_text_color(s_co2_status_lbl, CO2_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(s_co2_status_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_co2_status_lbl, "NO DATA");
    lv_obj_align(s_co2_status_lbl, LV_ALIGN_TOP_LEFT, 16, 32);

    s_co2_value_lbl = lv_label_create(s_co2_card);
    lv_obj_set_style_text_color(s_co2_value_lbl, CO2_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(s_co2_value_lbl, &lv_font_montserrat_36, 0);
    lv_label_set_text(s_co2_value_lbl, "--");
    lv_obj_align(s_co2_value_lbl, LV_ALIGN_RIGHT_MID, -70, 0);

    lv_obj_t *co2_unit = lv_label_create(s_co2_card);
    lv_obj_set_style_text_color(co2_unit, CO2_TEXT_COLOR, 0);
    lv_obj_set_style_text_font(co2_unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(co2_unit, "ppm");
    lv_obj_align(co2_unit, LV_ALIGN_BOTTOM_RIGHT, -14, -4);

    /* ---------- Lưới metric ---------- */
    ui_create_metric_cards();

    lv_obj_invalidate(scr);
}

static lv_obj_t *metric_create_card(lv_obj_t *parent, int x, int y,
                                    const char *name, lv_color_t accent,
                                    lv_obj_t **value_lbl_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, GRID_W, GRID_H);
    lv_obj_set_style_bg_color(card, COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    /* Thanh accent bên trái. */
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, 4, GRID_H);
    lv_obj_set_style_bg_color(bar, accent, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 0, 0);

    lv_obj_t *name_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(name_lbl, COLOR_LABEL, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(name_lbl, name);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 10, 4);

    lv_obj_t *value_lbl = lv_label_create(card);
    lv_obj_set_style_text_color(value_lbl, accent, 0);
    lv_obj_set_style_text_font(value_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(value_lbl, "--");
    lv_obj_align(value_lbl, LV_ALIGN_BOTTOM_LEFT, 10, -3);

    if (value_lbl_out) {
        *value_lbl_out = value_lbl;
    }
    return card;
}

static void ui_create_metric_cards(void)
{
    lv_obj_t *scr = lv_scr_act();
    /* Lưu ý: không dùng 'static' vì lv_color_hex() không phải hằng số
     * (static initializer bắt buộc phải là constant expression). */
    const char *names[6] = {
        "TEMP", "HUM", "PRES", "PM1.0", "PM2.5", "PM10"
    };
    const lv_color_t accents[6] = {
        COLOR_TEMP, COLOR_HUM, COLOR_PRES, COLOR_PM1, COLOR_PM25, COLOR_PM10
    };

    for (int i = 0; i < 6; i++) {
        int row = i / 3;
        int col = i % 3;
        int x = GRID_X0 + col * (GRID_W + GRID_GAP_X);
        int y = GRID_Y0 + row * (GRID_H + GRID_GAP_Y);
        metric_create_card(scr, x, y, names[i], accents[i], &s_metrics[i].value_lbl);
        s_metrics[i].accent = accents[i];
    }
}

static void set_label_if_changed(lv_obj_t *lbl, const char *text)
{
    const char *old = lv_label_get_text(lbl);
    if (old == NULL || strcmp(old, text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

/* Cập nhật toàn bộ giá trị lên UI. Gọi dưới lvgl_driver_lock(). */
static void ui_refresh(void)
{
    sensor_data_t data;
    memset(&data, 0, sizeof(data));
    sensor_get_data(&data);

    /* --- Thời gian --- */
    if (time_control_get_time_str(s_time_str, sizeof(s_time_str), "%H:%M:%S") == APP_OK) {
        set_label_if_changed(s_time_lbl, s_time_str);
    } else {
        set_label_if_changed(s_time_lbl, "--:--:--");
    }
    if (time_control_get_time_str(s_date_str, sizeof(s_date_str), "%d/%m/%Y") == APP_OK) {
        set_label_if_changed(s_date_lbl, s_date_str);
    }

    /* --- CO2 --- */
    co2_status_t st = co2_get_status(data.scd40_valid, data.co2);
    lv_obj_set_style_bg_color(s_co2_card, co2_status_color(st), 0);

    if (data.scd40_valid) {
        snprintf(s_co2_value, sizeof(s_co2_value), "%u", data.co2);
        set_label_if_changed(s_co2_value_lbl, s_co2_value);
        set_label_if_changed(s_co2_status_lbl, co2_status_text(st));
    } else {
        set_label_if_changed(s_co2_value_lbl, "--");
        set_label_if_changed(s_co2_status_lbl, "NO DATA");
    }

    /* --- 6 metric --- */
    /* TEMP */
    if (data.env_valid) {
        snprintf(s_metric_str[0], sizeof(s_metric_str[0]), "%.1f C", data.temperature);
    } else {
        strncpy(s_metric_str[0], "--.-", sizeof(s_metric_str[0]));
    }
    /* HUM */
    if (data.env_valid) {
        snprintf(s_metric_str[1], sizeof(s_metric_str[1]), "%.1f%%", data.humidity);
    } else {
        strncpy(s_metric_str[1], "--.-", sizeof(s_metric_str[1]));
    }
    /* PRES */
    if (data.env_valid) {
        snprintf(s_metric_str[2], sizeof(s_metric_str[2]), "%.0f hPa", data.pressure);
    } else {
        strncpy(s_metric_str[2], "--", sizeof(s_metric_str[2]));
    }
    /* PM1.0 / PM2.5 / PM10 */
    if (data.pms_valid) {
        snprintf(s_metric_str[3], sizeof(s_metric_str[3]), "%d", data.pm1_0_atm);
        snprintf(s_metric_str[4], sizeof(s_metric_str[4]), "%d", data.pm2_5_atm);
        snprintf(s_metric_str[5], sizeof(s_metric_str[5]), "%d", data.pm10_atm);
    } else {
        strncpy(s_metric_str[3], "--", sizeof(s_metric_str[3]));
        strncpy(s_metric_str[4], "--", sizeof(s_metric_str[4]));
        strncpy(s_metric_str[5], "--", sizeof(s_metric_str[5]));
    }

    for (int i = 0; i < 6; i++) {
        s_metric_str[i][sizeof(s_metric_str[i]) - 1] = '\0';
        set_label_if_changed(s_metrics[i].value_lbl, s_metric_str[i]);
    }
}

static void dash_task(void *arg)
{
    while (1) {
        if (lvgl_driver_lock(-1)) {
            ui_refresh();
            lvgl_driver_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(DASH_REFRESH_PERIOD_MS));
    }
}

static co2_status_t co2_get_status(bool valid, int co2)
{
    if (!valid) {
        return CO2_STATUS_INVALID;
    }
    if (co2 >= 2000) {
        return CO2_STATUS_BAD;
    }
    if (co2 >= 1500) {
        return CO2_STATUS_POOR;
    }
    if (co2 >= 1000) {
        return CO2_STATUS_FAIR;
    }
    return CO2_STATUS_GOOD;
}

static lv_color_t co2_status_color(co2_status_t s)
{
    switch (s) {
    case CO2_STATUS_GOOD:    return CO2_COLOR_GOOD;
    case CO2_STATUS_FAIR:    return CO2_COLOR_FAIR;
    case CO2_STATUS_POOR:    return CO2_COLOR_POOR;
    case CO2_STATUS_BAD:     return CO2_COLOR_BAD;
    case CO2_STATUS_INVALID:
    default:                 return CO2_COLOR_INVALID;
    }
}

static const char *co2_status_text(co2_status_t s)
{
    switch (s) {
    case CO2_STATUS_GOOD:    return "GOOD";
    case CO2_STATUS_FAIR:    return "FAIR";
    case CO2_STATUS_POOR:    return "POOR";
    case CO2_STATUS_BAD:     return "BAD";
    case CO2_STATUS_INVALID:
    default:                 return "NO DATA";
    }
}
