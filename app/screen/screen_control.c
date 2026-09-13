/*
 * screen_control.c
 *
 * Màn hình HomeApp trên TFT 3.5" ILI9488 480x320 + LVGL v8.
 * UI kiểu smartClock (icon EEZ) nhưng bố cục riêng cho HomeApp:
 *   - Màn hình HOME: header + đồng hồ lớn; container trái = thông số cảm biến
 *     HomeApp (nhỏ gọn); container phải = lưới dịch vụ (Lịch, Bấm giờ, Nhạc,
 *     Wifi, Đèn, Camera...) dùng icon EEZ.
 *   - Chạm icon -> mở màn hình tính năng (mỗi màn hình có nút Back).
 *
 * Phase 1: khung + navigation; các tính năng thật được triển khai dần trong
 * container nội dung (ui_feature_calendar/stopwatch/music/wifi...).
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
#include "wifi_control.h"
#include "eez_icons/eez_icons.h"
#include "ui_calendar.h"
#include "ui_stopwatch.h"
#include "ui_wifi.h"
#include "ui_music.h"
#include "ui_chat.h"
/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "screen_control"

#define UI_TASK_NAME          "screen_task"
#define UI_TASK_STACK_SIZE    (4096)
#define UI_TASK_PRIORITY      (4)
#define UI_REFRESH_PERIOD_MS  (1000)

/* --- Màu (RGB888) --- */
#define COL_BG        lv_color_hex(0x101418)
#define COL_HEADER    lv_color_hex(0x16234F)
#define COL_PANEL     lv_color_hex(0x1C2532)
#define COL_TITLE     lv_color_hex(0xFFFFFF)
#define COL_LABEL     lv_color_hex(0x9AA0A6)
#define COL_DIVIDER   lv_color_hex(0x3A414C)
#define COL_ACCENT    lv_color_hex(0x38B6FF)

#define COL_TEMP      lv_color_hex(0xFF8A00)
#define COL_HUM       lv_color_hex(0x00E5FF)
#define COL_PRES      lv_color_hex(0x38B6FF)
#define COL_CO2       lv_color_hex(0x9CCC65)
#define COL_PM1       lv_color_hex(0x90A4AE)
#define COL_PM25      lv_color_hex(0xFF6FA5)
#define COL_PM10      lv_color_hex(0xFFD54F)
#define COL_SENSOR_VALUE lv_color_hex(0xE8EAED)

/* --- Layout (480x320) --- */
#define HEADER_H       (36)
#define CLOCK_Y        (42)      /* đồng hồ lớn */
#define PANEL_Y        (112)     /* 2 container nâng lên để không cắt chữ */
#define PANEL_H        (320 - PANEL_Y - 6)

#define SENSOR_X       (10)
#define SENSOR_W       (150)
#define SERVICE_X      (SENSOR_X + SENSOR_W + 8)
#define SERVICE_W      (480 - SERVICE_X - 10)

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_36);
LV_FONT_DECLARE(lv_font_montserrat_48);

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void ui_task(void *arg);
static void ui_build_home(void);
static void ui_build_feature_pages(void);
static void ui_page_show(int page);
static void ui_refresh(void);
static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h, const char *title);
static lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                               lv_color_t color, int x, int y);
static void ui_toast(const char *text, uint32_t ms);
static void on_service_clicked(lv_event_t *e);

/******************************* DATA TYPES *******************************/
typedef enum {
    PAGE_HOME = 0,
    PAGE_CALENDAR,
    PAGE_STOPWATCH,
    PAGE_MUSIC,
    PAGE_WIFI,
    PAGE_CHAT,
    PAGE_CAMERA,
    PAGE_AI,
    PAGE_COUNT
} page_id_t;

typedef struct {
    const char *name;
    const lv_img_dsc_t *icon;
    page_id_t page;
} service_t;

typedef struct {
    const char *name;
    lv_color_t color;
    lv_obj_t *value_lbl;
} sensor_row_t;

/******************************* VARIABLES *******************************/
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_page_content[PAGE_COUNT];

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_date_lbl;
static lv_obj_t *s_wifi_icon;
static sensor_row_t s_rows[7];

static char s_time_str[16];
static char s_date_str[16];
static char s_value_buf[7][16];

static lv_obj_t *s_toast;

/* Danh sách dịch vụ trên màn hình HOME. (ASCII để font Montserrat hiển thị đúng) */
static const service_t s_services[] = {
    { "Calendar",  &img_calendar_65,    PAGE_CALENDAR  },
    { "Stopwatch", &img_sand_clock_65,  PAGE_STOPWATCH },
    { "Music",     &img_cassette_65,    PAGE_MUSIC     },
    { "Wifi",      &img_wifi_65,        PAGE_WIFI      },
    { "Chat",      &img_ai_65,          PAGE_CHAT      },
    { "Camera",    &img_camera_65,      PAGE_CAMERA    },
};
#define SERVICE_COUNT (sizeof(s_services) / sizeof(s_services[0]))

static const char *s_sensor_names[7] = { "TEMP", "HUM", "PRES", "CO2", "PM1.0", "PM2.5", "PM10" };

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT screen_init()
{
    APP_RESULT ret = APP_OK;

    ret = lcd_driver_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ret = lvgl_driver_init();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ui_build_home();
    ui_build_feature_pages();
    ui_page_show(PAGE_HOME);

    ret = lvgl_driver_start();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    BaseType_t task_ok = xTaskCreate(ui_task, UI_TASK_NAME,
                                     UI_TASK_STACK_SIZE, NULL,
                                     UI_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to create UI task");
        return APP_ERROR;
    }

    ESP_LOGI(THIS_MODULE_NAME, "UI started");
    return ret;
}

static lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                               lv_color_t color, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

/* Tạo panel bo góc + tiêu đề nhỏ ở góc. */
static lv_obj_t *panel_create(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_DIVIDER, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    if (title) {
        ui_make_label(panel, title, &lv_font_montserrat_14, COL_LABEL, 10, 6);
    }
    return panel;
}

static void ui_build_home(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    s_pages[PAGE_HOME] = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pages[PAGE_HOME]);
    lv_obj_set_pos(s_pages[PAGE_HOME], 0, 0);
    lv_obj_set_size(s_pages[PAGE_HOME], LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(s_pages[PAGE_HOME], COL_BG, 0);
    lv_obj_set_style_bg_opa(s_pages[PAGE_HOME], LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_pages[PAGE_HOME], 0, 0);

    /* Header bar: HOME APP + wifi icon + date */
    lv_obj_t *header = lv_obj_create(s_pages[PAGE_HOME]);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(header, COL_HEADER, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);

    ui_make_label(header, "HOME APP", &lv_font_montserrat_16, COL_TITLE, 12, 8);

    s_wifi_icon = lv_img_create(header);
    lv_img_set_src(s_wifi_icon, &img_wifi_disconnected_25);
    lv_obj_align(s_wifi_icon, LV_ALIGN_RIGHT_MID, -6, 0);

    /* Ngày đặt bên TRÁI icon wifi để không đè/che logo wifi. */
    s_date_lbl = ui_make_label(header, "--/--/----", &lv_font_montserrat_14, COL_LABEL, 0, 10);
    lv_label_set_long_mode(s_date_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_date_lbl, 110);
    lv_obj_align(s_date_lbl, LV_ALIGN_RIGHT_MID, -44, 0);

    /* Đồng hồ lớn */
    s_time_lbl = ui_make_label(s_pages[PAGE_HOME], "--:--:--", &lv_font_montserrat_48, COL_TITLE, 0, CLOCK_Y);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, CLOCK_Y);

    /* ---- Container TRÁI: thông số cảm biến HomeApp (nhỏ gọn) ---- */
    lv_obj_t *sensor_panel = panel_create(s_pages[PAGE_HOME], SENSOR_X, PANEL_Y, SENSOR_W, PANEL_H, "SENSORS");

    /* lv_color_hex() là hàm (không phải hằng số) nên để cục bộ. */
    const lv_color_t colors[7] = {
        COL_TEMP, COL_HUM, COL_PRES, COL_CO2, COL_PM1, COL_PM25, COL_PM10
    };
    const int row_y0 = 30;
    const int row_h = 24;
    for (int i = 0; i < 7; i++) {
        int y = row_y0 + i * row_h;
        /* tên màu accent */
        ui_make_label(sensor_panel, s_sensor_names[i], &lv_font_montserrat_14,
                      colors[i], 12, y);
        s_rows[i].name = s_sensor_names[i];
        s_rows[i].color = colors[i];
        s_rows[i].value_lbl = ui_make_label(sensor_panel, "--", &lv_font_montserrat_16,
                                            COL_SENSOR_VALUE, SENSOR_W - 70, y - 2);
        /* kẻ ngăn cách */
        if (i > 0) {
            lv_obj_t *line = lv_obj_create(sensor_panel);
            lv_obj_remove_style_all(line);
            lv_obj_set_pos(line, 8, y - 1);
            lv_obj_set_size(line, SENSOR_W - 16, 1);
            lv_obj_set_style_bg_color(line, COL_DIVIDER, 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        }
    }

    /* ---- Container PHẢI: các dịch vụ (lưới icon) ---- */
    lv_obj_t *service_panel = panel_create(s_pages[PAGE_HOME], SERVICE_X, PANEL_Y, SERVICE_W, PANEL_H, "APPS");

    const int svc_w = (SERVICE_W - 24) / 3;   /* 3 cột */
    const int svc_h = 76;
    for (int i = 0; i < (int)SERVICE_COUNT; i++) {
        int col = i % 3;
        int row = i / 3;
        int cx = 12 + col * svc_w;
        int cy = 30 + row * svc_h;

        lv_obj_t *btn = lv_obj_create(service_panel);
        lv_obj_remove_style_all(btn);
        lv_obj_set_pos(btn, cx, cy);
        lv_obj_set_size(btn, svc_w, svc_h);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x232C3B), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, on_service_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(size_t)s_services[i].page);

        lv_obj_t *img = lv_img_create(btn);
        lv_img_set_src(img, s_services[i].icon);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 2);
        lv_img_set_zoom(img, 180);   /* ~47px */

        lv_obj_t *cap = lv_label_create(btn);
        lv_obj_set_style_text_color(cap, COL_TITLE, 0);
        lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
        lv_label_set_text(cap, s_services[i].name);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
}

/* Tạo một trang tính năng với header + nút Back + vùng nội dung trống. */
static void feature_page_create(page_id_t id, const char *title)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_size(page, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(page, COL_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    /* header */
    lv_obj_t *header = lv_obj_create(page);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, LCD_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(header, COL_HEADER, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);

    /* tiêu đề */
    ui_make_label(header, title, &lv_font_montserrat_20, COL_TITLE, 0, 7);
    lv_obj_t *tt = lv_obj_get_child(header, lv_obj_get_child_cnt(header) - 1);
    lv_obj_align(tt, LV_ALIGN_TOP_MID, 0, 6);

    /* nút Back */
    lv_obj_t *btn = lv_obj_create(header);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 82, 26);
    lv_obj_align(btn, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3752), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_service_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(size_t)PAGE_HOME);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(bl, COL_TITLE, 0);
    lv_obj_center(bl);

    /* vùng nội dung */
    lv_obj_t *content = lv_obj_create(page);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content, 0, HEADER_H + 2);
    lv_obj_set_size(content, LCD_H_RES, LCD_V_RES - HEADER_H - 2);
    lv_obj_set_style_pad_all(content, 0, 0);

    s_pages[id] = page;
    s_page_content[id] = content;
}

static void ui_build_feature_pages(void)
{
    const char *titles[PAGE_COUNT] = {
        [PAGE_CALENDAR] = "Calendar",
        [PAGE_STOPWATCH] = "Stopwatch",
        [PAGE_MUSIC] = "Music",
        [PAGE_WIFI] = "Wifi",
        [PAGE_CHAT] = "Chat",
        [PAGE_CAMERA] = "Camera",
        [PAGE_AI] = "AI",
    };
    static const char *hints[PAGE_COUNT] = {
        [PAGE_MUSIC] = "Music player (SD) - Phase 3",
        [PAGE_WIFI] = "Manual wifi - Phase 3",
        [PAGE_CAMERA] = "Not supported on HomeApp",
        [PAGE_AI] = "Not supported on HomeApp",
    };

    for (page_id_t p = PAGE_CALENDAR; p < PAGE_COUNT; p++) {
        feature_page_create(p, titles[p]);
        if (p == PAGE_CALENDAR) {
            ui_calendar_create(s_page_content[p]);
        } else if (p == PAGE_STOPWATCH) {
            ui_stopwatch_create(s_page_content[p]);
        } else if (p == PAGE_WIFI) {
            ui_wifi_create(s_page_content[p]);
        } else if (p == PAGE_MUSIC) {
            ui_music_create(s_page_content[p]);
        } else if (p == PAGE_CHAT) {
            ui_chat_create(s_page_content[p]);
        } else {
            lv_obj_t *hint = ui_make_label(s_page_content[p], hints[p],
                                           &lv_font_montserrat_20, COL_LABEL, 0, 0);
            lv_obj_align(hint, LV_ALIGN_CENTER, 0, 0);
        }
    }
}

static void ui_page_show(int page)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (s_pages[i]) {
            if (i == page) {
                lv_obj_clear_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_pages[i]);
            } else {
                lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void on_service_clicked(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    if (v >= 0 && v < PAGE_COUNT) {
        ui_page_show((int)v);
    }
}

/* Toast nhỏ hiện ở giữa dưới màn hình. */
static void toast_hide_cb(lv_timer_t *timer)
{
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    lv_timer_del(timer);
}

/* Toast nhỏ hiện ở giữa dưới màn hình (dùng ở các Phase sau). */
static void __attribute__((unused)) ui_toast(const char *text, uint32_t ms)
{
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_toast, 16, 0);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    lv_obj_set_style_border_color(s_toast, COL_DIVIDER, 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_t *lbl = lv_label_create(s_toast);
    lv_obj_set_style_text_color(lbl, COL_TITLE, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_move_foreground(s_toast);
    lv_timer_create(toast_hide_cb, ms, NULL);
}

static void set_label_if_changed(lv_obj_t *lbl, const char *text)
{
    const char *old = lv_label_get_text(lbl);
    if (old == NULL || strcmp(old, text) != 0) {
        lv_label_set_text(lbl, text);
    }
}

static void ui_refresh(void)
{
    sensor_data_t data;
    memset(&data, 0, sizeof(data));
    sensor_get_data(&data);

    /* giờ/ngày: chỉ hiển thị khi SNTP đã đồng bộ (tránh hiện ngày 1970) */
    if (time_control_is_synced()) {
        if (time_control_get_time_str(s_time_str, sizeof(s_time_str), "%H:%M:%S") == APP_OK) {
            set_label_if_changed(s_time_lbl, s_time_str);
        }
        if (time_control_get_time_str(s_date_str, sizeof(s_date_str), "%d/%m/%Y") == APP_OK) {
            set_label_if_changed(s_date_lbl, s_date_str);
        }
    } else {
        set_label_if_changed(s_time_lbl, "--:--:--");
        set_label_if_changed(s_date_lbl, "--/--/----");
    }

    /* trạng thái wifi */
    bool wifi_on = wifi_control_is_connected();
    lv_img_set_src(s_wifi_icon, wifi_on ? (const void *)&img_wifi_connected_25
                                        : (const void *)&img_wifi_disconnected_25);

    /* các giá trị cảm biến */
    if (data.env_valid) {
        snprintf(s_value_buf[0], sizeof(s_value_buf[0]), "%.1fC", data.temperature);
        snprintf(s_value_buf[1], sizeof(s_value_buf[1]), "%.0f%%", data.humidity);
        snprintf(s_value_buf[2], sizeof(s_value_buf[2]), "%.0f", data.pressure);
    } else {
        snprintf(s_value_buf[0], sizeof(s_value_buf[0]), "--");
        snprintf(s_value_buf[1], sizeof(s_value_buf[1]), "--");
        snprintf(s_value_buf[2], sizeof(s_value_buf[2]), "--");
    }
    if (data.scd40_valid) {
        snprintf(s_value_buf[3], sizeof(s_value_buf[3]), "%u", data.co2);
    } else {
        snprintf(s_value_buf[3], sizeof(s_value_buf[3]), "--");
    }
    if (data.pms_valid) {
        snprintf(s_value_buf[4], sizeof(s_value_buf[4]), "%d", data.pm1_0_atm);
        snprintf(s_value_buf[5], sizeof(s_value_buf[5]), "%d", data.pm2_5_atm);
        snprintf(s_value_buf[6], sizeof(s_value_buf[6]), "%d", data.pm10_atm);
    } else {
        for (int i = 4; i < 7; i++) {
            snprintf(s_value_buf[i], sizeof(s_value_buf[i]), "--");
        }
    }
    for (int i = 0; i < 7; i++) {
        set_label_if_changed(s_rows[i].value_lbl, s_value_buf[i]);
    }
}

static void ui_task(void *arg)
{
    while (1) {
        if (lvgl_driver_lock(-1)) {
            ui_refresh();
            lvgl_driver_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_PERIOD_MS));
    }
}
