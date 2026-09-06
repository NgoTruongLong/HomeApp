/*
 * lcd_driver.c
 *
 * Low-level ILI9488 (SPI) + XPT2046 (SPI touch) driver for HomeApp,
 * ported from smartClock/source/lcd/lcd_control.cpp (rewritten in C).
 *
 * Wiring: see lcd_driver.h - giữ nguyên chân HomeApp (12/11/10/9/8/7),
 * thêm GPIO13 (MISO) + GPIO14 (CS touch) cho cảm ứng.
 */

#include "lcd_driver.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9488.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_xpt2046.h"

#include "lvgl.h"
#include "lvgl_driver.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "lcd_driver"

#define LCD_PCLK_HZ            (40 * 1000 * 1000)  /* 40 MHz */
#define LCD_SPI_QUEUE_DEPTH    (10)
#define LCD_SPI_MAX_TRANSFER_SZ (LCD_H_RES * LCD_DRAW_BUF_LINES * 3)  /* ~28.8 KB */

/* Số pixel tối đa LVGL flush trong 1 lần = 1 draw buffer -> đủ cho bộ đệm
 * chuyển RGB565 -> RGB666 bên trong driver ILI9488. */
#define LCD_PANEL_CONVERT_PIXELS (LCD_H_RES * LCD_DRAW_BUF_LINES)

#define LCD_CMD_BITS           (8)
#define LCD_PARAM_BITS         (8)

/* --- Xoay màn hình (điều chỉnh nếu lắp ngược) --- */
#define LCD_SWAP_XY            (1)   /* 1: xoay ngang 480x320 */
#define LCD_MIRROR_X           (1)
#define LCD_MIRROR_Y           (0)

/* --- Touch: thông số mapping (kinh nghiệm từ smartClock, chỉnh nếu lệch) --- */
#define TOUCH_CLOCK_HZ         (1 * 1000 * 1000)
#define TOUCH_X_RES_MIN        (15)
#define TOUCH_X_RES_MAX        (454)
#define TOUCH_Y_RES_MIN        (16)
#define TOUCH_Y_RES_MAX        (298)

/******************************* FUNCTIONS PROTOTYPE *******************************/
static APP_RESULT lcd_init_backlight(void);
static APP_RESULT lcd_init_spi_bus(void);
static APP_RESULT lcd_init_display(void);
static APP_RESULT lcd_init_touch(void);
static bool lcd_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx);

/******************************* DATA TYPES *******************************/

/******************************* VARIABLES *******************************/
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
APP_RESULT lcd_driver_init(void)
{
    APP_RESULT ret = APP_OK;

    ret = lcd_init_backlight();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    ret = lcd_init_spi_bus();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    ret = lcd_init_display();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);
    ret = lcd_init_touch();
    ASSERT_CRITICAL(ret == APP_OK, ret, ret);

    ESP_LOGI(THIS_MODULE_NAME, "ILI9488 480x320 + XPT2046 touch initialized");
    return APP_OK;
}

esp_lcd_panel_handle_t lcd_driver_get_panel(void)
{
    return s_panel;
}

esp_lcd_touch_handle_t lcd_driver_get_touch(void)
{
    return s_touch;
}

static APP_RESULT lcd_init_backlight(void)
{
    /* BL = GPIO7, bật mức cao (giống cách HomeApp cũ điều khiển). */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LCD_BL_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = gpio_set_level(LCD_BL_PIN, 1);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    return APP_OK;
}

static APP_RESULT lcd_init_spi_bus(void)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = LCD_SPI_MOSI_PIN,
        .miso_io_num     = LCD_SPI_MISO_PIN,   /* THÊM MỚI: cho touch đọc */
        .sclk_io_num     = LCD_SPI_SCLK_PIN,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .data4_io_num    = GPIO_NUM_NC,
        .data5_io_num    = GPIO_NUM_NC,
        .data6_io_num    = GPIO_NUM_NC,
        .data7_io_num    = GPIO_NUM_NC,
        .max_transfer_sz = LCD_SPI_MAX_TRANSFER_SZ,
        .flags           = SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MISO |
                           SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MASTER |
                           SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id      = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags      = 0,
    };

    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    return APP_OK;
}

static APP_RESULT lcd_init_display(void)
{
    esp_err_t err = ESP_OK;
    esp_lcd_panel_io_handle_t io_handle = NULL;

    /* Panel IO: CS=10, DC=9; callback báo LVGL khi 1 flush hoàn tất. */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num        = LCD_SPI_CS_PIN,
        .dc_gpio_num        = LCD_SPI_DC_PIN,
        .spi_mode           = 0,
        .pclk_hz            = LCD_PCLK_HZ,
        .trans_queue_depth  = LCD_SPI_QUEUE_DEPTH,
        .on_color_trans_done = lcd_notify_lvgl_flush_ready,
        .user_ctx           = (void *)lvgl_driver_get_disp_drv(),
        .lcd_cmd_bits       = LCD_CMD_BITS,
        .lcd_param_bits     = LCD_PARAM_BITS,
        .flags              = {0},
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                   &io_config, &io_handle);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    /* Panel ILI9488 (SPI bắt buộc 18-bit; driver tự chuyển RGB565 -> RGB666). */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_SPI_RST_PIN,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 18,                     /* 18-bit qua SPI */
        .flags          = { .reset_active_high = 0 },
        .vendor_config  = NULL,
    };
    err = esp_lcd_new_panel_ili9488(io_handle, &panel_config,
                                    LCD_PANEL_CONVERT_PIXELS, &s_panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    err = esp_lcd_panel_reset(s_panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_lcd_panel_init(s_panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_lcd_panel_invert_color(s_panel, false);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    /* Xoay ngang: landscape 480x320 như smartClock. */
    err = esp_lcd_panel_swap_xy(s_panel, (LCD_SWAP_XY != 0));
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_lcd_panel_mirror(s_panel, (LCD_MIRROR_X != 0), (LCD_MIRROR_Y != 0));
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_lcd_panel_set_gap(s_panel, 0, 0);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
    return APP_OK;
}

static APP_RESULT lcd_init_touch(void)
{
    esp_err_t err = ESP_OK;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;

    /* Touch dùng chung bus SPI3 với màn hình, CS riêng = GPIO14. */
    esp_lcd_panel_io_spi_config_t tp_io_config = {
        .cs_gpio_num        = LCD_TOUCH_CS_PIN,
        .dc_gpio_num        = GPIO_NUM_NC,
        .spi_mode           = 0,
        .pclk_hz            = TOUCH_CLOCK_HZ,
        .trans_queue_depth  = 3,
        .on_color_trans_done = NULL,
        .user_ctx           = NULL,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .flags              = {0},
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                   &tp_io_config, &tp_io_handle);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    esp_lcd_touch_config_t tp_cfg = {
        .x_max            = LCD_V_RES,      /* 320 - sau swap x/y */
        .y_max            = LCD_H_RES,      /* 480 */
        .rst_gpio_num     = GPIO_NUM_NC,
        .int_gpio_num     = GPIO_NUM_NC,
        .levels           = { .reset = 0, .interrupt = 0 },
        .flags            = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 0 },
        .process_coordinates = NULL,
        .interrupt_callback = NULL,
        .user_data        = NULL,
        .driver_data      = NULL,
    };
    err = esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &s_touch);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    ESP_LOGI(THIS_MODULE_NAME, "Touch XPT2046 init OK");
    return APP_OK;
}

/* Được esp_lcd gọi sau khi chuyển xong 1 khối màu (context SPI post-trans).
 * Báo LVGL rằng buffer đã flush xong để tái sử dụng. */
static bool lcd_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                        esp_lcd_panel_io_event_data_t *edata,
                                        void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}
