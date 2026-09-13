/*
 * ui_wifi.h - màn hình chọn & kết nối WiFi trên màn hình (LVGL).
 * Port từ smartClock: quét danh sách AP, chọn mạng, nhập mật khẩu bằng bàn phím ảo.
 */
#ifndef UI_WIFI_H
#define UI_WIFI_H

#include "lvgl.h"

void ui_wifi_create(lv_obj_t *content);

#endif /* UI_WIFI_H */
