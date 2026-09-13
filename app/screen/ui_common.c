/*
 * ui_common.c - tiện ích LVGL dùng chung.
 */
#include "ui_common.h"
#include <string.h>

lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                        lv_color_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

lv_obj_t *ui_make_button(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                         lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A3752), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);
    return btn;
}

void ui_set_text(lv_obj_t *lbl, const char *text)
{
    const char *old = lv_label_get_text(lbl);
    if (old == NULL || strcmp(old, text) != 0) {
        lv_label_set_text(lbl, text);
    }
}
