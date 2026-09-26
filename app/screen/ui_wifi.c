/*
 * ui_wifi.c
 *
 * Màn hình WiFi (Phase 3 / port từ smartClock):
 *  - Tự quét mạng khi mở trang, liệt kê các AP (SSID, RSSI, mở/bảo mật).
 *  - Chạm 1 mạng: mạng mở -> kết nối ngay; mạng có mật khẩu -> hộp thoại
 *    nhập mật khẩu bằng bàn phím ảo (lv_keyboard) rồi bấm Connect.
 *  - Nút Scan (quét lại), Disconnect (ngắt nhưng giữ cấu hình),
 *    Forget (ngắt + xoá mạng đã lưu).
 *  - Mọi thao tác trạng thái được theo dõi bằng lv_timer (chạy trong LVGL task).
 */
#include "ui_wifi.h"
#include "ui_common.h"
#include "wifi_control.h"
#include "lv_font_vn.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

/******************************* DEFINITIONS *******************************/
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define THIS_MODULE_NAME  "ui_wifi"
#define UI_TICK_MS        (150)
#define WIFI_ROW_H        (44)
#define WIFI_MAX_AP       (16)
#define MAX_ROWS          (16)
#define SCAN_TIMEOUT_US   (10 * 1000 * 1000)   /* 10 s */

#define COL_TITLE   lv_color_hex(0xFFFFFF)
#define COL_LABEL   lv_color_hex(0x9AA0A6)
#define COL_ACCENT  lv_color_hex(0x38B6FF)
#define COL_GREEN   lv_color_hex(0x4CAF50)
#define COL_ORANGE  lv_color_hex(0xFFA726)
#define COL_RED     lv_color_hex(0xEF5350)
#define COL_BTN     lv_color_hex(0x2A3752)
#define COL_ROW     lv_color_hex(0x232C3B)
#define COL_ROW_PRS lv_color_hex(0x16234F)
#define COL_PANEL   lv_color_hex(0x1C2532)

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void wifi_tick(lv_timer_t *timer);
static void wifi_start_scan(void);
static void wifi_build_list(void);
static void wifi_update_status(void);
static void wifi_connect_start(const char *ssid, const char *password);
static void wifi_row_click_cb(lv_event_t *e);
static void wifi_btn_click_cb(lv_event_t *e);
static void wifi_show_loading(void);
static void wifi_show_empty(void);
static void wifi_show_list(void);

/* Hộp thoại nhập mật khẩu */
static void wifi_dlg_open(const char *ssid);
static void wifi_dlg_close(void);
static void wifi_dlg_connect_cb(lv_event_t *e);
static void wifi_dlg_cancel_cb(lv_event_t *e);
static void wifi_dlg_ready_cb(lv_event_t *e);
static void wifi_dlg_do_connect(void);
static void wifi_dlg_kbmode_cb(lv_event_t *e);

/******************************* DATA TYPES *******************************/
typedef struct {
    lv_obj_t *page;        /* parent page (chứa content) */
    lv_obj_t *content;
    lv_obj_t *status_lbl;  /* dòng trạng thái trên cùng */
    lv_obj_t *list;        /* vùng cuộn danh sách mạng */
    lv_obj_t *loading;     /* overlay: đang quét */
    lv_obj_t *empty;       /* overlay: không có mạng */
    lv_obj_t *btn_scan;
    lv_obj_t *btn_disconnect;
    lv_obj_t *btn_forget;

    wifi_ap_info_t aps[WIFI_MAX_AP];
    uint16_t ap_cnt;

    bool page_was_visible;
    bool scanning;
    bool connecting_now;   /* đang có 1 yêu cầu kết nối do người dùng */
    bool failed;           /* lần kết nối gần nhất thất bại */
    int64_t scan_start_us; /* thời điểm bắt đầu quét (cho timeout) */
    wifi_state_t last_state;
    char last_attempt[33]; /* ssid đang/thử kết nối */

    /* hộp thoại nhập mật khẩu */
    lv_obj_t *dlg;
    lv_obj_t *dlg_ta;
    lv_obj_t *dlg_err;
    lv_obj_t *dlg_kb;      /* bàn phím ảo */
    lv_obj_t *dlg_kb_lbl;  /* nhãn nút đổi chế độ bàn phím */
    char dlg_ssid[33];
} wifi_ui_t;

/******************************* VARIABLES *******************************/
static wifi_ui_t g;

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static lv_color_t wifi_sig_color(int8_t rssi)
{
    if (rssi > -55) return COL_GREEN;
    if (rssi > -70) return COL_ORANGE;
    return COL_RED;
}

/* ---------- view helpers ---------- */
static void wifi_show_loading(void)
{
    lv_obj_clear_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_show_empty(void)
{
    lv_obj_add_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_show_list(void)
{
    lv_obj_add_flag(g.loading, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(g.list, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- bắt đầu quét ---------- */
static void wifi_start_scan(void)
{
    if (g.scanning) {
        return;
    }
    wifi_state_t st = wifi_control_get_state();
    /* Đang tự kết nối lại / đang thử kết nối (không tới) -> dừng lại
       để người dùng quét & chọn mạng khác được. */
    if (st == WIFI_STATE_RECONNECTING || st == WIFI_STATE_CONNECTING) {
        wifi_control_abort_auto_reconnect();
        g.connecting_now = false;
        g.failed = false;
        g.last_state = WIFI_STATE_IDLE;
    }
    if (wifi_control_scan_start() != APP_OK) {
        /* Có thể WiFi chưa sẵn sàng -> hiện thông báo ngắn. */
        ui_set_text(g.status_lbl, "Cannot start scan");
        lv_obj_set_style_text_color(g.status_lbl, COL_RED, 0);
        return;
    }
    g.scanning = true;
    g.failed = false;
    g.scan_start_us = esp_timer_get_time();
    ESP_LOGI(THIS_MODULE_NAME, "scanning...");
    wifi_show_loading();
}

/* ---------- trạng thái ---------- */
static void wifi_update_status(void)
{
    char buf[110];
    lv_color_t col = COL_LABEL;
    wifi_state_t st = wifi_control_get_state();

    switch (st) {
    case WIFI_STATE_CONNECTED: {
        char ip[16] = "";
        if (wifi_control_get_ip(ip, sizeof(ip)) == APP_OK) {
            snprintf(buf, sizeof(buf), "Connected: %s  (%s)",
                     wifi_control_get_connected_ssid(), ip);
        } else {
            snprintf(buf, sizeof(buf), "Connected: %s",
                     wifi_control_get_connected_ssid());
        }
        col = COL_GREEN;
        break;
    }
    case WIFI_STATE_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting to %s...",
                 g.last_attempt[0] ? g.last_attempt : "network");
        col = COL_ORANGE;
        break;
    case WIFI_STATE_RECONNECTING:
        snprintf(buf, sizeof(buf), "Connection lost, reconnecting...");
        col = COL_ORANGE;
        break;
    default: /* IDLE / OFF */
        if (g.failed && g.last_attempt[0]) {
            snprintf(buf, sizeof(buf), "Connect failed: %s", g.last_attempt);
            col = COL_RED;
        } else {
            snprintf(buf, sizeof(buf), "Not connected - scan & pick a network");
            col = COL_LABEL;
        }
        break;
    }
    ui_set_text(g.status_lbl, buf);
    lv_obj_set_style_text_color(g.status_lbl, col, 0);
}

/* ---------- tạo 1 dòng mạng ---------- */
static void wifi_row_create(lv_obj_t *list, uint16_t idx)
{
    const wifi_ap_info_t *ap = &g.aps[idx];
    bool is_connected = false;

    if (wifi_control_is_connected()) {
        const char *cs = wifi_control_get_connected_ssid();
        if (cs[0] != '\0' && strcmp(cs, ap->ssid) == 0) {
            is_connected = true;
        }
    }

    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), WIFI_ROW_H);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_color(row, is_connected ? lv_color_hex(0x20364A) : COL_ROW, 0);
    lv_obj_set_style_bg_color(row, COL_ROW_PRS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, wifi_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    /* icon sóng (màu theo cường độ) */
    lv_obj_t *sig = ui_make_label(row, LV_SYMBOL_WIFI, &lv_font_montserrat_20,
                                  is_connected ? COL_GREEN : wifi_sig_color(ap->rssi), 0, 0);
    lv_obj_align(sig, LV_ALIGN_LEFT_MID, 14, 0);

    /* tên SSID */
    /* SSID có thể chứa dấu tiếng Việt -> dùng font VN. */
    lv_obj_t *name = ui_make_label(row, ap->ssid[0] ? ap->ssid : "(hidden)",
                                   &lv_font_vn_16, COL_TITLE, 0, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, 250);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 46, 0);

    /* thông tin bên phải */
    char info[48];
    lv_color_t info_col;
    if (is_connected) {
        snprintf(info, sizeof(info), "Connected");
        info_col = COL_GREEN;
    } else {
        snprintf(info, sizeof(info), "%s  |  %d dBm",
                 (ap->authmode == 0) ? "Open" : "Secured", ap->rssi);
        info_col = (ap->authmode == 0) ? COL_ACCENT : COL_ORANGE;
    }
    lv_obj_t *info_lbl = ui_make_label(row, info, &lv_font_montserrat_14, info_col, 0, 0);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(info_lbl, 150);
    lv_obj_align(info_lbl, LV_ALIGN_RIGHT_MID, -10, 0);
}

/* ---------- dựng lại danh sách từ snapshot đã quét ---------- */
static void wifi_build_list(void)
{
    lv_obj_clean(g.list);

    /* Chọn mạng tốt nhất cho từng SSID (tránh lặp AP cùng tên). */
    uint16_t best[MAX_ROWS];
    uint16_t best_n = 0;
    for (uint16_t i = 0; i < g.ap_cnt && best_n < MAX_ROWS; i++) {
        bool found = false;
        for (uint16_t j = 0; j < best_n; j++) {
            if (strcmp(g.aps[best[j]].ssid, g.aps[i].ssid) == 0) {
                if (g.aps[i].rssi > g.aps[best[j]].rssi) {
                    best[j] = i;
                }
                found = true;
                break;
            }
        }
        if (!found) {
            best[best_n++] = i;
        }
    }
    /* sắp theo RSSI giảm dần */
    for (uint16_t i = 0; i < best_n; i++) {
        for (uint16_t j = i + 1; j < best_n; j++) {
            if (g.aps[best[j]].rssi > g.aps[best[i]].rssi) {
                uint16_t t = best[i];
                best[i] = best[j];
                best[j] = t;
            }
        }
    }

    if (best_n == 0) {
        wifi_show_empty();
        return;
    }
    wifi_show_list();
    for (uint16_t i = 0; i < best_n; i++) {
        wifi_row_create(g.list, best[i]);
    }
    lv_obj_update_layout(g.list);
}

/* ---------- bắt đầu kết nối từ giao diện ---------- */
static void wifi_connect_start(const char *ssid, const char *password)
{
    wifi_state_t st = wifi_control_get_state();
    if (st == WIFI_STATE_CONNECTING || st == WIFI_STATE_RECONNECTING) {
        return;
    }
    if (ssid == NULL) {
        return;
    }
    snprintf(g.last_attempt, sizeof(g.last_attempt), "%s", ssid);
    g.failed = false;
    g.connecting_now = true;

    if (wifi_control_connect(ssid, password) != APP_OK) {
        g.connecting_now = false;
        g.failed = true;
    }
    wifi_update_status();
    wifi_build_list();
}

/* ---------- chạm vào 1 mạng ---------- */
static void wifi_row_click_cb(lv_event_t *e)
{
    uint16_t idx = (uint16_t)(intptr_t)lv_event_get_user_data(e);
    if (idx >= g.ap_cnt || g.dlg != NULL) {
        return;
    }
    wifi_state_t st = wifi_control_get_state();
    if (st == WIFI_STATE_CONNECTING || st == WIFI_STATE_RECONNECTING) {
        return;
    }
    const wifi_ap_info_t *ap = &g.aps[idx];

    /* đang kết nối chính mạng này rồi thì bỏ qua */
    if (wifi_control_is_connected()) {
        const char *cs = wifi_control_get_connected_ssid();
        if (cs[0] != '\0' && strcmp(cs, ap->ssid) == 0) {
            return;
        }
    }

    if (ap->authmode == 0) {
        /* Mạng mở: kết nối thẳng. */
        wifi_connect_start(ap->ssid, "");
    } else {
        wifi_dlg_open(ap->ssid);
    }
}

/* ---------- nút Scan / Disconnect / Forget ---------- */
static void wifi_btn_click_cb(lv_event_t *e)
{
    intptr_t act = (intptr_t)lv_event_get_user_data(e);
    switch (act) {
    case 0:   /* Scan */
        wifi_start_scan();
        break;
    case 1:   /* Disconnect */
        wifi_control_disconnect();
        g.connecting_now = false;
        g.failed = false;
        g.last_attempt[0] = '\0';
        wifi_update_status();
        wifi_build_list();
        break;
    case 2:   /* Forget */
        wifi_control_forget();
        g.connecting_now = false;
        g.failed = false;
        g.last_attempt[0] = '\0';
        wifi_update_status();
        wifi_build_list();
        break;
    }
}

/* ==================== Hộp thoại nhập mật khẩu ==================== */
/* nút đổi chế độ bàn phím: abc -> ABC -> 123 */
static void wifi_dlg_kbmode_cb(lv_event_t *e)
{
    (void)e;
    if (g.dlg_kb == NULL) {
        return;
    }
    if (lv_keyboard_get_mode(g.dlg_kb) == LV_KEYBOARD_MODE_TEXT_LOWER) {
        lv_keyboard_set_mode(g.dlg_kb, LV_KEYBOARD_MODE_TEXT_UPPER);
        ui_set_text(g.dlg_kb_lbl, "ABC");
    } else if (lv_keyboard_get_mode(g.dlg_kb) == LV_KEYBOARD_MODE_TEXT_UPPER) {
        lv_keyboard_set_mode(g.dlg_kb, LV_KEYBOARD_MODE_NUMBER);
        ui_set_text(g.dlg_kb_lbl, "123");
    } else {
        lv_keyboard_set_mode(g.dlg_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
        ui_set_text(g.dlg_kb_lbl, "abc");
    }
}

static void wifi_dlg_close(void)
{
    if (g.dlg) {
        lv_obj_del(g.dlg);
        g.dlg = NULL;
        g.dlg_ta = NULL;
        g.dlg_err = NULL;
        g.dlg_kb = NULL;
        g.dlg_kb_lbl = NULL;
    }
}

static void wifi_dlg_ready_cb(lv_event_t *e)
{
    (void)e;
    wifi_dlg_do_connect();
}

static void wifi_dlg_cancel_cb(lv_event_t *e)
{
    (void)e;
    wifi_dlg_close();
}

static void wifi_dlg_connect_cb(lv_event_t *e)
{
    (void)e;
    wifi_dlg_do_connect();
}

static void wifi_dlg_do_connect(void)
{
    if (g.dlg == NULL) {
        return;
    }
    const char *pass = lv_textarea_get_text(g.dlg_ta);
    /* Hộp thoại chỉ mở cho mạng có mật khẩu (mạng mở kết nối thẳng). */
    if (pass == NULL || pass[0] == '\0') {
        ui_set_text(g.dlg_err, "Please enter password!");
        return;
    }
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", g.dlg_ssid);
    wifi_dlg_close();
    wifi_connect_start(ssid, pass);
}

static void wifi_dlg_open(const char *ssid)
{
    if (g.dlg != NULL) {
        return;
    }
    snprintf(g.dlg_ssid, sizeof(g.dlg_ssid), "%s", ssid);

    lv_obj_t *scr = lv_scr_act();
    lv_coord_t W = lv_obj_get_width(scr);
    lv_coord_t H = lv_obj_get_height(scr);

    /* overlay toàn màn hình */
    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_remove_style_all(ov);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_size(ov, W, H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_90, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(ov);

    /* tiêu đề */
    lv_obj_t *title = ui_make_label(ov, LV_SYMBOL_WIFI "  Connect to network",
                                    &lv_font_montserrat_16, COL_TITLE, 0, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    /* SSID cần nhập */
    lv_obj_t *ssid_lbl = ui_make_label(ov, ssid, &lv_font_vn_20, COL_ACCENT, 0, 0);
    lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ssid_lbl, 420);
    lv_obj_set_style_text_align(ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ssid_lbl, LV_ALIGN_TOP_MID, 0, 30);

    /* ô nhập mật khẩu */
    g.dlg_ta = lv_textarea_create(ov);
    lv_obj_set_size(g.dlg_ta, 388, 42);
    lv_obj_align(g.dlg_ta, LV_ALIGN_TOP_MID, 26, 62);
    lv_textarea_set_placeholder_text(g.dlg_ta, "WiFi password...");
    lv_textarea_set_password_mode(g.dlg_ta, true);
    lv_textarea_set_one_line(g.dlg_ta, true);
    lv_textarea_set_max_length(g.dlg_ta, 63);
    lv_textarea_set_text(g.dlg_ta, "");
    lv_obj_set_style_bg_color(g.dlg_ta, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(g.dlg_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.dlg_ta, 6, 0);
    lv_obj_set_style_border_width(g.dlg_ta, 1, 0);
    lv_obj_set_style_border_color(g.dlg_ta, COL_ACCENT, 0);
    lv_obj_set_style_pad_all(g.dlg_ta, 6, 0);
    lv_obj_set_style_text_color(g.dlg_ta, COL_TITLE, 0);
    lv_obj_set_style_text_font(g.dlg_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g.dlg_ta, COL_LABEL, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(g.dlg_ta, COL_ACCENT, LV_PART_CURSOR);
    lv_obj_add_event_cb(g.dlg_ta, wifi_dlg_ready_cb, LV_EVENT_READY, NULL);

    /* nút đổi chế độ bàn phím: abc -> ABC -> 123 (cho phép gõ chữ HOA / số) */
    lv_obj_t *kb_btn = ui_make_button(ov, "abc", 44, 42, wifi_dlg_kbmode_cb, NULL);
    lv_obj_align(kb_btn, LV_ALIGN_TOP_LEFT, 8, 62);
    lv_obj_set_style_bg_color(kb_btn, lv_color_hex(0x37424F), 0);
    g.dlg_kb_lbl = lv_obj_get_child(kb_btn, 0);

    /* thông báo lỗi (ẩn mặc định) */
    g.dlg_err = ui_make_label(ov, "", &lv_font_montserrat_14, COL_RED, 0, 0);
    lv_obj_set_width(g.dlg_err, 440);
    lv_obj_set_style_text_align(g.dlg_err, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g.dlg_err, LV_ALIGN_TOP_MID, 0, 108);

    /* Cancel */
    lv_obj_t *btn_cancel = ui_make_button(ov, "Cancel", 190, 38, wifi_dlg_cancel_cb, NULL);
    lv_obj_align(btn_cancel, LV_ALIGN_TOP_MID, -105, 130);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x37424F), 0);

    /* Connect */
    lv_obj_t *btn_ok = ui_make_button(ov, LV_SYMBOL_OK "  Connect", 190, 38,
                                      wifi_dlg_connect_cb, NULL);
    lv_obj_align(btn_ok, LV_ALIGN_TOP_MID, 105, 130);
    lv_obj_set_style_bg_color(btn_ok, lv_color_hex(0x1E7A3C), 0);

    /* bàn phím ảo */
    lv_obj_t *kb = lv_keyboard_create(ov);
    lv_obj_set_size(kb, 464, 142);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_keyboard_set_textarea(kb, g.dlg_ta);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    g.dlg_kb = kb;
    ui_set_text(g.dlg_kb_lbl, "abc");
    lv_obj_set_style_bg_color(kb, COL_PANEL, 0);
    lv_obj_set_style_radius(kb, 8, 0);
    lv_obj_set_style_pad_all(kb, 4, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    /* phím */
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x243042), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x16234F), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, COL_TITLE, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_14, LV_PART_ITEMS);
    /* phím chức năng (checked) */
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x1E3A5F), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, COL_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);

    g.dlg = ov;
}

/* ==================== lv_timer định kỳ ==================== */
static void wifi_tick(lv_timer_t *timer)
{
    (void)timer;
    if (g.page == NULL || g.content == NULL) {
        return;
    }
    bool vis = !lv_obj_has_flag(g.page, LV_OBJ_FLAG_HIDDEN);

    if (vis && !g.page_was_visible) {
        /* vừa mở trang WiFi */
        g.page_was_visible = true;
        wifi_state_t st = wifi_control_get_state();
        g.last_state = st;
        g.connecting_now = (st == WIFI_STATE_CONNECTING || st == WIFI_STATE_RECONNECTING);
        if (!g.connecting_now) {
            g.failed = false;
        }
        /* Tự quét khi mở trang: IDLE / CONNECTED / RECONNECTING.
           (Đang CONNECTING do vừa bấm kết nối thì không quét đè.) */
        if (st != WIFI_STATE_CONNECTING && g.ap_cnt == 0) {
            wifi_start_scan();
        }
        if (!g.scanning) {
            wifi_update_status();
            wifi_build_list();
        }
    } else if (!vis && g.page_was_visible) {
        /* rời trang WiFi */
        g.page_was_visible = false;
        if (g.dlg != NULL) {
            wifi_dlg_close();
        }
        return;
    }

    if (!vis) {
        return;
    }
    if (g.dlg != NULL) {
        return;   /* đang nhập mật khẩu */
    }

    /* chờ scan xong */
    if (g.scanning) {
        if (wifi_control_scan_is_done()) {
            uint16_t n = wifi_control_scan_get_results(g.aps, WIFI_MAX_AP);
            g.ap_cnt = n;
            g.scanning = false;
            ESP_LOGI(THIS_MODULE_NAME, "scan done, found %u network(s)", (unsigned)n);
            wifi_build_list();
            wifi_update_status();
        } else if (esp_timer_get_time() - g.scan_start_us > SCAN_TIMEOUT_US) {
            /* Quét quá lâu -> huỷ để người dùng có thể bấm lại. */
            wifi_control_scan_stop();
            g.scanning = false;
            ESP_LOGW(THIS_MODULE_NAME, "scan timed out");
            ui_set_text(g.status_lbl, "Scan timed out - tap Scan to retry");
            lv_obj_set_style_text_color(g.status_lbl, COL_RED, 0);
            if (g.ap_cnt > 0) {
                wifi_show_list();
            } else {
                wifi_show_empty();
            }
        }
        return;
    }

    /* theo dõi thay đổi trạng thái kết nối */
    wifi_state_t st = wifi_control_get_state();
    if (st != g.last_state) {
        wifi_state_t prev = g.last_state;
        g.last_state = st;
        if (st == WIFI_STATE_CONNECTED) {
            g.connecting_now = false;
            g.failed = false;
            wifi_build_list();
        } else if (st == WIFI_STATE_IDLE && prev == WIFI_STATE_CONNECTING) {
            g.connecting_now = false;
            if (!wifi_control_is_connected()) {
                g.failed = true;
            }
            wifi_build_list();
        }
        wifi_update_status();
    }
}

/* ==================== tạo màn hình ==================== */
void ui_wifi_create(lv_obj_t *content)
{
    memset(&g, 0, sizeof(g));
    g.content = content;
    g.page = lv_obj_get_parent(content);

    /* LƯU Ý: lúc dựng UI (trước refresh đầu tiên của LVGL) coords của object
       chưa được tính nên lv_obj_get_width/height có thể trả 0. Nếu để size
       = 0 thì container danh sách / loading sẽ vô hình. Vì vậy cần kiểm tra
       và dùng kích thước thực của vùng nội dung (480 x 282) làm giá trị an toàn. */
    lv_coord_t W = lv_obj_get_width(content);
    lv_coord_t H = lv_obj_get_height(content);
    if (W <= 20 || W > 600) {
        W = 480;   /* LCD_H_RES */
    }
    if (H <= 20 || H > 340) {
        H = 282;   /* LCD_V_RES - HEADER_H - 2 */
    }

    /* ---- dòng trạng thái + 3 nút ---- */
    g.status_lbl = ui_make_label(content, "Not connected - scan & pick a network",
                                 &lv_font_montserrat_14, COL_LABEL, 12, 10);
    lv_label_set_long_mode(g.status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(g.status_lbl, W - 290);

    g.btn_scan = ui_make_button(content, "Scan", 70, 30, wifi_btn_click_cb, (void *)(intptr_t)0);
    lv_obj_set_pos(g.btn_scan, W - 12 - 70, 8);
    lv_obj_set_style_bg_color(g.btn_scan, COL_ACCENT, 0);

    g.btn_disconnect = ui_make_button(content, "Disconnect", 96, 30, wifi_btn_click_cb,
                                      (void *)(intptr_t)1);
    lv_obj_set_pos(g.btn_disconnect, W - 12 - 70 - 6 - 96, 8);
    lv_obj_set_style_bg_color(g.btn_disconnect, lv_color_hex(0x9A2A2A), 0);

    g.btn_forget = ui_make_button(content, "Forget", 72, 30, wifi_btn_click_cb, (void *)(intptr_t)2);
    lv_obj_set_pos(g.btn_forget, W - 12 - 70 - 6 - 96 - 6 - 72, 8);
    lv_obj_set_style_bg_color(g.btn_forget, lv_color_hex(0x37424F), 0);

    /* ---- vùng danh sách ---- */
    const lv_coord_t list_y = 46;
    const lv_coord_t list_h = H - list_y - 8;

    g.list = lv_obj_create(content);
    lv_obj_remove_style_all(g.list);
    lv_obj_set_pos(g.list, 8, list_y);
    lv_obj_set_size(g.list, W - 16, list_h);
    lv_obj_set_style_radius(g.list, 8, 0);
    lv_obj_set_style_bg_color(g.list, lv_color_hex(0x12171E), 0);
    lv_obj_set_style_bg_opa(g.list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.list, 1, 0);
    lv_obj_set_style_border_color(g.list, lv_color_hex(0x3A414C), 0);
    lv_obj_set_style_pad_all(g.list, 6, 0);
    lv_obj_set_style_pad_row(g.list, 6, 0);
    lv_obj_set_style_pad_column(g.list, 0, 0);
    lv_obj_set_layout(g.list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g.list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(g.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g.list, LV_SCROLLBAR_MODE_AUTO);

    /* ---- overlay: đang quét ---- */
    g.loading = lv_obj_create(content);
    lv_obj_remove_style_all(g.loading);
    lv_obj_set_pos(g.loading, 8, list_y);
    lv_obj_set_size(g.loading, W - 16, list_h);
    lv_obj_set_style_radius(g.loading, 8, 0);
    lv_obj_set_style_bg_color(g.loading, lv_color_hex(0x12171E), 0);
    lv_obj_set_style_bg_opa(g.loading, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g.loading, 1, 0);
    lv_obj_set_style_border_color(g.loading, lv_color_hex(0x3A414C), 0);

    lv_obj_t *spin = lv_spinner_create(g.loading, 900, 60);
    lv_obj_set_size(spin, 34, 34);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_arc_color(spin, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spin, 3, LV_PART_INDICATOR);

    lv_obj_t *scan_lbl = ui_make_label(g.loading, "Scanning WiFi...", &lv_font_montserrat_14,
                                       COL_LABEL, 0, 0);
    lv_obj_set_width(scan_lbl, lv_pct(100));
    lv_obj_set_style_text_align(scan_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(scan_lbl, LV_ALIGN_CENTER, 0, 26);

    /* ---- overlay: không có mạng ---- */
    g.empty = lv_obj_create(content);
    lv_obj_remove_style_all(g.empty);
    lv_obj_set_pos(g.empty, 8, list_y);
    lv_obj_set_size(g.empty, W - 16, list_h);
    lv_obj_set_style_radius(g.empty, 8, 0);
    lv_obj_set_style_bg_color(g.empty, lv_color_hex(0x12171E), 0);
    lv_obj_set_style_bg_opa(g.empty, LV_OPA_90, 0);
    lv_obj_set_style_border_width(g.empty, 1, 0);
    lv_obj_set_style_border_color(g.empty, lv_color_hex(0x3A414C), 0);

    lv_obj_t *empty_lbl = ui_make_label(g.empty, "No networks found\nTap Scan to retry",
                                        &lv_font_montserrat_16, COL_LABEL, 0, 0);
    lv_obj_set_width(empty_lbl, lv_pct(100));
    lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);

    /* hiển thị danh sách trống trước khi quét */
    wifi_show_empty();

    /* lv_timer theo dõi trạng thái (chạy trong LVGL task) */
    lv_timer_create(wifi_tick, UI_TICK_MS, NULL);
}
