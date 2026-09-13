/*
 * ui_chat.c
 *
 * Màn hình Chat với Xiaozhi AI:
 *  - Nút START/STOP (toggle): bật/tắt phiên trò chuyện (mic + websocket + loa).
 *  - Dòng trạng thái: Idle / Connecting / Listening / Thinking / Speaking.
 *  - Khung hiển thị câu nói gần nhất (STT của người dùng hoặc câu AI đang nói).
 *  - ASCII-safe (font LVGL mặc định không có glyph dấu tiếng Việt).
 */
#include "ui_chat.h"
#include "ui_common.h"
#include "voice_chat.h"
#include <stdio.h>
#include <string.h>

/******************************* DEFINITIONS *******************************/
LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
LV_FONT_DECLARE(lv_font_montserrat_20);

#define CHAT_TICK_MS   (200)

#define COL_TITLE      lv_color_hex(0xFFFFFF)
#define COL_LABEL      lv_color_hex(0x9AA0A6)
#define COL_ACCENT     lv_color_hex(0x38B6FF)
#define COL_RED        lv_color_hex(0xEF5350)
#define COL_PANEL      lv_color_hex(0x1C2532)
#define COL_BORDER     lv_color_hex(0x3A414C)

/******************************* VARIABLES *******************************/
static lv_obj_t *s_state_lbl;
static lv_obj_t *s_text_lbl;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_lbl;

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void chat_btn_cb(lv_event_t *e);
static void chat_tick(lv_timer_t *timer);

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void chat_btn_cb(lv_event_t *e)
{
    (void)e;
    voice_chat_toggle();
}

static void chat_tick(lv_timer_t *timer)
{
    (void)timer;
    const char *txt;
    bool active = voice_chat_is_active();

    ui_set_text(s_state_lbl, voice_chat_state_text());

    txt = voice_chat_get_last_text();
    if (txt == NULL || txt[0] == '\0') {
        txt = "(No conversation yet)";
    }
    ui_set_text(s_text_lbl, txt);

    ui_set_text(s_btn_lbl, active ? "STOP" : "START");
    lv_obj_set_style_bg_color(s_btn, active ? COL_RED : COL_ACCENT, 0);
}

void ui_chat_create(lv_obj_t *content)
{
    lv_coord_t W, H;
    lv_obj_t *panel;

    W = lv_obj_get_width(content);
    H = lv_obj_get_height(content);
    if (W <= 20 || W > 600) {
        W = 480;   /* LCD_H_RES */
    }
    if (H <= 20 || H > 340) {
        H = 282;   /* LCD_V_RES - HEADER_H - 2 */
    }

    /* Dòng trạng thái */
    s_state_lbl = ui_make_label(content, "Tap START to talk", &lv_font_montserrat_20,
                                COL_TITLE, 0, 0);
    lv_obj_set_width(s_state_lbl, W);
    lv_obj_set_style_text_align(s_state_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, 10);

    /* Khung hội thoại */
    panel = lv_obj_create(content);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 12, 46);
    lv_obj_set_size(panel, W - 24, H - 46 - 72);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, COL_BORDER, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);

    s_text_lbl = ui_make_label(panel, "(No conversation yet)", &lv_font_montserrat_16,
                               COL_LABEL, 0, 0);
    lv_obj_set_width(s_text_lbl, W - 24 - 24);
    lv_label_set_long_mode(s_text_lbl, LV_LABEL_LONG_WRAP);

    /* Nút START/STOP */
    s_btn = ui_make_button(content, "START", 200, 48, chat_btn_cb, NULL);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_color(s_btn, COL_ACCENT, 0);
    s_btn_lbl = lv_obj_get_child(s_btn, 0);
    lv_obj_set_style_text_font(s_btn_lbl, &lv_font_montserrat_20, 0);

    lv_timer_create(chat_tick, CHAT_TICK_MS, NULL);
}
