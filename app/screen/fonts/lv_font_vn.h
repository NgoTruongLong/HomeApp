/*
 * lv_font_vn.h
 *
 * Font tiếng Việt cho LVGL v8, tạo từ Montserrat-Medium.ttf bằng lv_font_conv
 * (cùng typeface với lv_font_montserrat_* nên giao diện không bị lệch kiểu chữ).
 *
 * Dải glyph phủ:
 *   0x20-0x7F  ASCII
 *   0xA0-0xFF  Latin-1 Supplement   (à á â è é ê ì í ò ó ô ù ú ý ...)
 *   0x100-0x17F Latin Extended-A    (Ă ă Đ đ Ĩ ĩ Ũ ũ ...)
 *   0x1A0-0x1B0 Latin Extended-B    (Ơ ơ Ư ư)
 *   0x1EA0-0x1EF9 Latin Ext. Add.   (ạ ả ấ ầ ẩ ẫ ậ ẹ ế ệ ố ồ ớ ờ ợ ụ ứ ự ...)
 *   0x2013-0x2026 dấu gạch ngang, nháy, bullet, ellipsis
 *
 * Dùng cho các label hiển thị text ĐỘNG có thể chứa tiếng Việt:
 *   - ui_chat.c : câu STT/TTS trả về từ server
 *   - ui_music.c: tên file bài hát trên thẻ SD
 *   - ui_wifi.c : tên SSID (có thể có dấu)
 * Label tĩnh (ASCII) vẫn dùng lv_font_montserrat_* như cũ.
 */
#ifndef LV_FONT_VN_H
#define LV_FONT_VN_H

#include "lvgl.h"

LV_FONT_DECLARE(lv_font_vn_14);
LV_FONT_DECLARE(lv_font_vn_16);
LV_FONT_DECLARE(lv_font_vn_20);

#endif /* LV_FONT_VN_H */
