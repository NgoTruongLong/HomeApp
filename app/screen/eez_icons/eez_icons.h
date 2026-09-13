/*
 * eez_icons.h
 *
 * Icon EEZ Studio (từ smartClock) được đưa sang HomeApp để dùng lại cho UI.
 * Mỗi file ui_image_*.c là mảng pixel RGB565 thuần (chỉ variant
 * LV_COLOR_DEPTH==16 được biên dịch).
 */

#ifndef EEZ_ICONS_H
#define EEZ_ICONS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_img_dsc_t img_lamp_65;
extern const lv_img_dsc_t img_calendar_65;
extern const lv_img_dsc_t img_cassette_65;
extern const lv_img_dsc_t img_sand_clock_65;
extern const lv_img_dsc_t img_wifi_65;
extern const lv_img_dsc_t img_camera_65;
extern const lv_img_dsc_t img_ai_65;
extern const lv_img_dsc_t img_clock_65;
extern const lv_img_dsc_t img_wifi_connected_25;
extern const lv_img_dsc_t img_wifi_disconnected_25;

#ifdef __cplusplus
}
#endif

#endif /* EEZ_ICONS_H */
