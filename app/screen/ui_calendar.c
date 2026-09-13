/*
 * ui_calendar.c
 *
 * Màn hình Lịch tháng hiển thị lịch từ giờ hệ thống (SNTP). ASCII-safe
 * (không dùng dấu tiếng Việt vì font LVGL mặc định không có glyph dấu).
 */
#include "ui_calendar.h"
#include "ui_common.h"
#include "time_control.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/******************************* DEFINITIONS *******************************/
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define CAL_COLS       (7)
#define CAL_W          (480)
#define CAL_NAV_H      (40)
#define CAL_DOW_H      (24)
#define CAL_TOP        (CAL_NAV_H + CAL_DOW_H)   /* 64 */
#define CAL_GRID_H     (282 - CAL_TOP)           /* ~218: 6 hàng */
#define CAL_CELL_W     (CAL_W / CAL_COLS)
#define CAL_CELL_H     (CAL_GRID_H / 6)

#define COL_WEEKEND    lv_color_hex(0xFF6F91)
#define COL_WEEKDAY    lv_color_hex(0xE0E0E0)
#define COL_TODAY_BG   lv_color_hex(0x38B6FF)
#define COL_TODAY_FG   lv_color_hex(0x001018)
#define COL_OTHER      lv_color_hex(0x4A4A55)
#define COL_NAV        lv_color_hex(0xFFFFFF)

/******************************* FUNCTIONS PROTOTYPE *******************************/
static int days_in_month(int mon, int year);
static void cal_rebuild(void);
static void cal_show_waiting(void);
static void cal_tick(lv_timer_t *timer);
static void on_nav_clicked(lv_event_t *e);

/******************************* VARIABLES *******************************/
static int s_year;
static int s_mon;          /* 0..11 */
static int s_today;
static bool s_seeded;      /* đã lấy được tháng/năm từ giờ hệ thống (SNTP) chưa */
static int s_last_yday = -1; /* tm_yday lần trước -> phát hiện sang ngày mới */
static lv_obj_t *s_title;
static lv_obj_t *s_grid;

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static int days_in_month(int mon, int year)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon == 1) {
        int leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return d[mon];
}

static void on_nav_clicked(lv_event_t *e)
{
    intptr_t dir = (intptr_t)lv_event_get_user_data(e);

    if (!s_seeded) {
        return;   /* chưa đồng bộ giờ -> chưa có lịch để điều hướng */
    }
    s_mon += (int)dir;
    if (s_mon < 0) {
        s_mon = 11;
        s_year--;
    } else if (s_mon > 11) {
        s_mon = 0;
        s_year++;
    }
    cal_rebuild();
}

static void cal_rebuild(void)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d/%04d", s_mon + 1, s_year);
    ui_set_text(s_title, buf);

    lv_obj_clean(s_grid);

    int dim = days_in_month(s_mon, s_year);
    /* tm_wday: 0=CN. Chuyển về tuần bắt đầu Thứ 2. */
    struct tm t0;
    memset(&t0, 0, sizeof(t0));
    t0.tm_year = s_year - 1900;
    t0.tm_mon  = s_mon;
    t0.tm_mday = 1;
    t0.tm_isdst = -1;
    time_t tt = mktime(&t0);
    localtime_r(&tt, &t0);
    int first_col = (t0.tm_wday + 6) % 7;   /* 0 = Thứ 2 */
    int cell = 0;

    /* ô trống đầu tháng */
    for (int c = 0; c < first_col; c++) {
        lv_obj_t *ph = lv_obj_create(s_grid);
        lv_obj_remove_style_all(ph);
        lv_obj_set_pos(ph, (c % CAL_COLS) * CAL_CELL_W, (cell / CAL_COLS) * CAL_CELL_H);
        lv_obj_set_size(ph, CAL_CELL_W - 4, CAL_CELL_H - 4);
        (void)ph;
        cell++;
    }

    for (int day = 1; day <= dim; day++, cell++) {
        int col = cell % CAL_COLS;
        int row = cell / CAL_COLS;
        bool is_today = (day == s_today);

        lv_obj_t *box = lv_obj_create(s_grid);
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, col * CAL_CELL_W + 2, row * CAL_CELL_H + 2);
        lv_obj_set_size(box, CAL_CELL_W - 4, CAL_CELL_H - 4);
        lv_obj_set_style_radius(box, 6, 0);
        if (is_today) {
            lv_obj_set_style_bg_color(box, COL_TODAY_BG, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        }

        char d[12];
        snprintf(d, sizeof(d), "%d", day);
        lv_obj_t *lbl = lv_label_create(box);
        lv_label_set_text(lbl, d);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, is_today ? COL_TODAY_FG : COL_WEEKDAY, 0);
        lv_obj_center(lbl);
    }
}

/* Trạng thái chờ: chưa có giờ SNTP -> không hiển thị ngày (tránh 01/1970). */
static void cal_show_waiting(void)
{
    ui_set_text(s_title, "--/----");
    lv_obj_clean(s_grid);
    lv_obj_t *msg = ui_make_label(s_grid, "Waiting for time sync...",
                                  &lv_font_montserrat_16, COL_OTHER, 0, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 0);
}

/* Timer: khởi tạo lịch khi SNTP đồng bộ xong và vẽ lại khi sang ngày mới. */
static void cal_tick(lv_timer_t *timer)
{
    (void)timer;

    if (!time_control_is_synced()) {
        return;   /* vẫn đang chờ SNTP (placeholder đã hiển thị lúc tạo) */
    }

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (s_seeded && tm_now.tm_yday == s_last_yday) {
        return;   /* chưa có gì thay đổi */
    }

    s_last_yday = tm_now.tm_yday;
    s_today     = tm_now.tm_mday;

    if (!s_seeded) {
        /* Giờ đã sẵn sàng: nhảy về tháng hiện tại. */
        s_seeded = true;
        s_year   = tm_now.tm_year + 1900;
        s_mon    = tm_now.tm_mon;
        cal_rebuild();
    } else if (s_year == tm_now.tm_year + 1900 && s_mon == tm_now.tm_mon) {
        /* Đang xem tháng hiện tại -> vẽ lại để cập nhật ô "hôm nay". */
        cal_rebuild();
    }
}

void ui_calendar_create(lv_obj_t *content)
{
    /* UI được dựng lúc boot, có thể SNTP CHƯA đồng bộ -> KHÔNG lấy time(NULL)
     * ngay (sẽ ra 01/1970). Nếu chưa có giờ thì hiện placeholder; cal_tick sẽ
     * tự khởi tạo khi SNTP đồng bộ xong. */
    if (time_control_is_synced()) {
        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        s_year      = tm_now.tm_year + 1900;
        s_mon       = tm_now.tm_mon;
        s_today     = tm_now.tm_mday;
        s_last_yday = tm_now.tm_yday;
        s_seeded    = true;
    } else {
        s_seeded    = false;
        s_last_yday = -1;
    }

    /* Hàng điều hướng */
    ui_make_button(content, LV_SYMBOL_LEFT, 44, 30, on_nav_clicked, (void *)(intptr_t)(-1));
    lv_obj_align(lv_obj_get_child(content, 0), LV_ALIGN_TOP_LEFT, 4, 4);

    ui_make_button(content, LV_SYMBOL_RIGHT, 44, 30, on_nav_clicked, (void *)(intptr_t)(1));
    lv_obj_align(lv_obj_get_child(content, 1), LV_ALIGN_TOP_RIGHT, -4, 4);

    s_title = ui_make_label(content, "", &lv_font_montserrat_20, COL_NAV, 0, 8);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 6);

    /* Hàng thứ trong tuần (bắt đầu Thứ 2) */
    static const char *dow[7] = { "T2", "T3", "T4", "T5", "T6", "T7", "CN" };
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = ui_make_label(content, dow[i], &lv_font_montserrat_14,
                                    (i == 6) ? COL_WEEKEND : COL_WEEKDAY, 0, CAL_NAV_H + 4);
        lv_obj_set_width(l, CAL_CELL_W);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_x(l, i * CAL_CELL_W);
    }

    /* Vùng lưới ngày */
    s_grid = lv_obj_create(content);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_pos(s_grid, 0, CAL_TOP);
    lv_obj_set_size(s_grid, CAL_W, CAL_GRID_H);
    lv_obj_set_style_pad_all(s_grid, 0, 0);

    if (s_seeded) {
        cal_rebuild();
    } else {
        cal_show_waiting();
    }

    /* Theo dõi SNTP: khởi tạo lịch khi có giờ + cập nhật khi sang ngày mới. */
    lv_timer_create(cal_tick, 1000, NULL);
}
