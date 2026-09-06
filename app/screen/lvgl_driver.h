/*
 * lvgl_driver.h
 *
 * LVGL v8 glue for HomeApp (ported from smartClock/source/lcd/lvgl_control).
 * Khởi tạo display driver, tick timer và input device (touch XPT2046).
 */

#ifndef LVGL_DRIVER_H
#define LVGL_DRIVER_H

#include "debug.h"
#include "lvgl.h"

/******************************* DEFINITIONS *******************************/
#define LVGL_TASK_NAME          "LVGLTask"
#define LVGL_TASK_STACK_SIZE    (4 * 1024)
#define LVGL_TASK_PRIORITY      (2)
#define LVGL_TASK_MAX_DELAY_MS  (30)
#define LVGL_TASK_MIN_DELAY_MS  (1)
#define LVGL_TICK_PERIOD_MS     (2)

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT lvgl_driver_init(void);   /* lv_init + display drv + tick + touch indev */
APP_RESULT lvgl_driver_start(void);  /* chạy task vòng lặp lv_timer_handler */
bool lvgl_driver_lock(int timeout_ms);
void lvgl_driver_unlock(void);
lv_disp_drv_t *lvgl_driver_get_disp_drv(void);
lv_disp_t *lvgl_driver_get_disp(void);

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif /* LVGL_DRIVER_H */
