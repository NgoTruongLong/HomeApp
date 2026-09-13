/*
 * ui_common.h - các tiện ích LVGL dùng chung cho các màn hình HomeApp.
 */
#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "lvgl.h"

lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                        lv_color_t color, lv_coord_t x, lv_coord_t y);
lv_obj_t *ui_make_button(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                         lv_event_cb_t cb, void *user_data);
void ui_set_text(lv_obj_t *lbl, const char *text);

#endif /* UI_COMMON_H */
