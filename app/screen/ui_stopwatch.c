/*
 * ui_stopwatch.c
 *
 * Đồng hồ bấm giờ: hiển thị MM:SS.d (0,1s) cập nhật bằng lv_timer 20ms
 * nhưng chỉ invalidate khi giá trị thay đổi (10 lần/s) -> nhẹ.
 * Nút: Start/Pause, Lap, Reset. Các vòng Lap hiển thị ở phần dưới.
 */
#include "ui_stopwatch.h"
#include "ui_common.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/******************************* DEFINITIONS *******************************/
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_48);

#define SW_TIMER_MS       (20)
#define SW_MAX_LAPS       (6)

#define COL_TIME      lv_color_hex(0xE6F7FF)
#define COL_LAP       lv_color_hex(0x9AA0A6)
#define COL_ACCENT    lv_color_hex(0x38B6FF)

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void sw_on_btn(lv_event_t *e);
static void sw_tick(lv_timer_t *timer);
static int64_t sw_elapsed_us(void);
static void sw_format(int64_t us, char *buf, size_t len);
static void sw_update_laps(void);

/******************************* VARIABLES *******************************/
static bool s_running = false;
static bool s_paused  = false;
static int64_t s_start_us = 0;
static int64_t s_accum_us = 0;
static int64_t s_laps_us[SW_MAX_LAPS];
static int s_lap_cnt = 0;

static lv_obj_t *s_time_lbl;
static lv_obj_t *s_laps_lbl;
static lv_obj_t *s_main_lbl;    /* label của nút Start/Pause */
static char s_last[16];

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void sw_format(int64_t us, char *buf, size_t len)
{
    int64_t cs = us / 10000;                 /* centi-giây: 1s = 100 cs (10ms/1cs) */
    int min = (int)(cs / 6000);              /* 1 phút = 6000 cs */
    int s   = (int)((cs / 100) % 60);        /* giây */
    int d   = (int)((cs % 100) / 10);        /* 0,1s (hàng chục của phần dư cs) */
    snprintf(buf, len, "%02d:%02d.%d", min, s, d);
}

static int64_t sw_elapsed_us(void)
{
    if (s_running) {
        return s_accum_us + (esp_timer_get_time() - s_start_us);
    }
    return s_accum_us;
}

static void sw_on_btn(lv_event_t *e)
{
    intptr_t act = (intptr_t)lv_event_get_user_data(e);
    switch (act) {
    case 0:   /* Start / Pause */
        if (!s_running && !s_paused) {      /* Start từ 0 */
            s_start_us = esp_timer_get_time();
            s_running = true;
            ui_set_text(s_main_lbl, "Pause");
        } else if (s_running) {              /* Pause */
            s_accum_us += esp_timer_get_time() - s_start_us;
            s_running = false;
            s_paused = true;
            ui_set_text(s_main_lbl, "Resume");
        } else {                             /* Resume */
            s_start_us = esp_timer_get_time();
            s_running = true;
            s_paused = false;
            ui_set_text(s_main_lbl, "Pause");
        }
        break;
    case 1:   /* Lap */
        if (s_running && s_lap_cnt < SW_MAX_LAPS) {
            s_laps_us[s_lap_cnt++] = sw_elapsed_us();
            sw_update_laps();
        }
        break;
    case 2:   /* Reset */
        s_running = false;
        s_paused = false;
        s_accum_us = 0;
        s_lap_cnt = 0;
        ui_set_text(s_main_lbl, "Start");
        sw_update_laps();
        break;
    }
}

static void sw_update_laps(void)
{
    char buf[SW_MAX_LAPS * 16 + 8];
    buf[0] = '\0';
    for (int i = 0; i < s_lap_cnt; i++) {
        char t[16];
        sw_format(s_laps_us[i], t, sizeof(t));
        char line[24];
        snprintf(line, sizeof(line), "%02d  %s\n", i + 1, t);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
    }
    if (s_lap_cnt == 0) {
        strncat(buf, "Chua co lap nao.", sizeof(buf) - strlen(buf) - 1);
    }
    ui_set_text(s_laps_lbl, buf);
}

static void sw_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_running) {
        return;
    }
    char buf[16];
    sw_format(sw_elapsed_us(), buf, sizeof(buf));
    if (strcmp(buf, s_last) != 0) {
        snprintf(s_last, sizeof(s_last), "%s", buf);
        ui_set_text(s_time_lbl, buf);
    }
}

void ui_stopwatch_create(lv_obj_t *content)
{
    /* Giờ lớn */
    s_time_lbl = ui_make_label(content, "00:00.0", &lv_font_montserrat_48, COL_TIME, 0, 6);
    lv_obj_align(s_time_lbl, LV_ALIGN_TOP_MID, 0, 8);

    /* Nút điều khiển */
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, 130);
    lv_obj_set_size(row, 480, 56);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *btn_start = ui_make_button(row, "Start", 110, 44, sw_on_btn, (void *)(intptr_t)0);
    lv_obj_align(btn_start, LV_ALIGN_CENTER, -150, 0);
    s_main_lbl = lv_obj_get_child(btn_start, 0);   /* label để đổi chữ Start/Pause */
    lv_obj_t *btn_lap = ui_make_button(row, "Lap", 110, 44, sw_on_btn, (void *)(intptr_t)1);
    lv_obj_align(btn_lap, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *btn_reset = ui_make_button(row, "Reset", 110, 44, sw_on_btn, (void *)(intptr_t)2);
    lv_obj_align(btn_reset, LV_ALIGN_CENTER, 150, 0);

    /* Danh sách Lap */
    s_laps_lbl = ui_make_label(content, "", &lv_font_montserrat_14, COL_LAP, 12, 200);
    lv_label_set_long_mode(s_laps_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_laps_lbl, 450);
    sw_update_laps();

    /* Tạo lv_timer cập nhật (chạy trong LVGL task) */
    lv_timer_t *timer = lv_timer_create(sw_tick, SW_TIMER_MS, NULL);
    (void)timer;
    s_last[0] = '\0';
}
