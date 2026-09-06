/*
 * lcd_driver.h
 *
 * Low-level driver for the 3.5" TFT ILI9488 (480x320 landscape) + XPT2046
 * touch screen, ported from the old smartClock project into HomeApp.
 *
 * ============================  CHÂN KẾT NỐI  ============================
 * Giữ NGUYÊN các chân HomeApp đã dùng cho màn hình ST7735 cũ:
 *    SCLK = GPIO12, MOSI = GPIO11, CS = GPIO10,
 *    DC   = GPIO9,  RST  = GPIO8,  BL (backlight) = GPIO7
 * Bus SPI: SPI3_HOST (giống HomeApp cũ - không đụng tới SPI2 của thẻ SD).
 *
 * Chân THÊM MỚI (chỉ cần cho touch XPT2046):
 *    MISO        = GPIO13  (đường đọc dữ liệu touch, dùng chung bus SPI3)
 *    CS(touch)   = GPIO14
 * ======================================================================
 */

#ifndef LCD_DRIVER_H
#define LCD_DRIVER_H

#include "debug.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

/******************************* DEFINITIONS *******************************/
/* --- Chân LCD (giữ nguyên như HomeApp cũ) --- */
#define LCD_SPI_HOST          (SPI3_HOST)
#define LCD_SPI_SCLK_PIN      (GPIO_NUM_12)
#define LCD_SPI_MOSI_PIN      (GPIO_NUM_11)
#define LCD_SPI_MISO_PIN      (GPIO_NUM_13)   /* THÊM MỚI: cho touch XPT2046 */
#define LCD_SPI_CS_PIN        (GPIO_NUM_10)
#define LCD_SPI_DC_PIN        (GPIO_NUM_9)
#define LCD_SPI_RST_PIN       (GPIO_NUM_8)
#define LCD_BL_PIN            (GPIO_NUM_7)

/* --- Chân touch (THÊM MỚI) --- */
#define LCD_TOUCH_CS_PIN      (GPIO_NUM_14)

/* --- Kích thước màn hình khi xoay ngang (landscape) --- */
#define LCD_H_RES             (480)
#define LCD_V_RES             (320)

/* --- Độ sâu buffer --- */
#define LCD_DRAW_BUF_LINES    (20)   /* số dòng LVGL vẽ 1 lần (partial flush) */

/******************************* FUNCTIONS PROTOTYPE *******************************/
APP_RESULT lcd_driver_init(void);
esp_lcd_panel_handle_t lcd_driver_get_panel(void);
esp_lcd_touch_handle_t lcd_driver_get_touch(void);

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/

#endif /* LCD_DRIVER_H */
