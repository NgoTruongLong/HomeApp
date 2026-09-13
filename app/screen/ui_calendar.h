/*
 * ui_calendar.c - màn hình Lịch tháng (nguồn giờ SNTP của HomeApp).
 * Lưới tháng: tiêu đề Tháng/Năm + nút < >, thứ trong tuần (T2..CN) và các ngày;
 * ngày hôm nay được tô sáng.
 */
#ifndef UI_CALENDAR_H
#define UI_CALENDAR_H

#include "lvgl.h"

void ui_calendar_create(lv_obj_t *content);

#endif /* UI_CALENDAR_H */
