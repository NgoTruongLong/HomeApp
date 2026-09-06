/*
 * screen_control.c
 *
 * ST7735 128x160 LCD display control.
 *
 * Shows real-time clock (via time_control) and the latest sensor values
 * (via sensor_get_data()) on an ST7735 panel driven through the
 * teriyakigod/esp_lcd_st7735 managed component (standard ESP-IDF
 * esp_lcd panel API).
 *
 * Wiring (defaults, adjust macros below to match your board):
 *   SCLK = GPIO12, MOSI = GPIO11, CS = GPIO10, DC = GPIO9,
 *   RST = GPIO8, BL (backlight) = GPIO7
 *
 * NOTE on RGB565 byte order: the ST7735 panel driver sends the pixel
 * buffer to the controller verbatim (no byte swap), and the panel runs in
 * big-endian mode by default. This module therefore stores every pixel in
 * the frame buffer as big-endian (high byte first) so colors are correct.
 */

#include "screen_control.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7735.h"

#include "sensor_control.h"
#include "time_control.h"

/******************************* DEFINITIONS *******************************/
#define THIS_MODULE_NAME "screen_control"

/* --- Hardware pins (adjust to your wiring) --- */
#define SCREEN_SPI_HOST         (SPI3_HOST)
#define SCREEN_SPI_SCLK_PIN     (GPIO_NUM_12)
#define SCREEN_SPI_MOSI_PIN     (GPIO_NUM_11)
#define SCREEN_SPI_CS_PIN       (GPIO_NUM_10)
#define SCREEN_SPI_DC_PIN       (GPIO_NUM_9)
#define SCREEN_SPI_RST_PIN      (GPIO_NUM_8)
#define SCREEN_BL_PIN           (GPIO_NUM_7)
#define SCREEN_BL_ON_LEVEL      (1)      /* 1 if backlight is active-high */

/* --- Display geometry (128x160 @ RGB565) --- */
#define SCREEN_LCD_H_RES        (128)
#define SCREEN_LCD_V_RES        (160)
#define SCREEN_LCD_MAX_TRANS    (SCREEN_LCD_H_RES * SCREEN_LCD_V_RES * 2)

/* --- Task --- */
#define SCREEN_TASK_STACK_SIZE  (4096)
#define SCREEN_TASK_PRIORITY    (5)
#define SCREEN_REFRESH_PERIOD_MS (1000)

/* --- Colors (RGB565) --- */
#define COLOR_BLACK   (0x0000)
#define COLOR_WHITE   (0xFFFF)
#define COLOR_CYAN    (0x07FF)
#define COLOR_YELLOW  (0xFFE0)
#define COLOR_GREEN   (0x07E0)
#define COLOR_RED     (0xF800)

/* --- Font geometry (5x7 + 1px spacing) --- */
#define FONT_W         (5)
#define FONT_H         (7)
#define CHAR_W         (FONT_W + 1)
#define CHAR_H         (FONT_H + 1)

/* Enable color inversion for the ST7735 (needed on many red-tab modules). */
#define SCREEN_ST7735_INVERT    (1)

/******************************* FUNCTIONS PROTOTYPE *******************************/
static void screen_task(void *arg);
static void fb_clear(uint16_t color);
static void fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
static void fb_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg);
static void fb_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);
static void fb_draw_string_centered(uint16_t y, const char *str, uint16_t fg, uint16_t bg);
static void fb_flush(void);
static void screen_render(sensor_data_t *data, uint8_t frame);

/******************************* DATA TYPES *******************************/
typedef struct {
    esp_lcd_panel_handle_t panel;
} screen_t;

/******************************* VARIABLES *******************************/
static screen_t screen_control;

/* Full-screen frame buffer, stored BIG-ENDIAN (high byte first) so the
 * ST7735 receives correct RGB565 colors. 160 * 128 * 2 = 40 KB. */
static uint8_t fb[SCREEN_LCD_V_RES][SCREEN_LCD_H_RES * 2];

/* 5x7 public-domain ASCII font, one 5-byte glyph per char (0x20..0x7E).
 * bit0 of each column byte = top row. */
static const uint8_t font5x7[][FONT_W] = {
    {0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x00,0x08,0x14,0x22,0x41}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x41,0x22,0x14,0x08,0x00}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */
    {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */
    {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */
    {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */
    {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */
    {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */
    {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */
    {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */
    {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */
    {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */
    {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */
    {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */
    {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */
    {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */
    {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */
    {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x08,0x04,0x08,0x10,0x08}, /* ~ */
};

/* Degree symbol glyph, drawn for char code 0xB0 (e.g. "25.4°C"). */
static const uint8_t degree_glyph[FONT_W] = {0x06, 0x09, 0x09, 0x06, 0x00};

/* 16x16 icons: one row mask per line, bit15 = leftmost pixel. */
#define ICON_W (16)
#define ICON_H (16)

/* Thermometer (temperature). */
static const uint16_t icon_thermo[ICON_H] = {
    0x0000, 0x0200, 0x0200, 0x0200, 0x0200, 0x0200, 0x0200, 0x0300,
    0x0780, 0x0780, 0x0780, 0x0780, 0x0300, 0x0000, 0x0000, 0x0000,
};
/* Water drop (humidity). */
static const uint16_t icon_drop[ICON_H] = {
    0x0000, 0x0100, 0x0380, 0x07C0, 0x0FE0, 0x1FF0, 0x1FF8, 0x3FFC,
    0x3FFC, 0x3FFC, 0x1FF8, 0x1FF0, 0x0FE0, 0x07C0, 0x0380, 0x0000,
};
/* Pressure gauge (barometer). */
static const uint16_t icon_gauge[ICON_H] = {
    0x0000, 0x3FFC, 0x4002, 0x4002, 0x4002, 0x4002, 0x4012, 0x4022,
    0x41C2, 0x4082, 0x4102, 0x4202, 0x4002, 0x3FFC, 0x1FF8, 0x0000,
};
/* Cloud (CO2 / air). */
static const uint16_t icon_cloud[ICON_H] = {
    0x0000, 0x0180, 0x0780, 0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE, 0xFFFF,
    0xFFFF, 0x7FFE, 0x3FFC, 0x1FF8, 0x0FF0, 0x0000, 0x0000, 0x0000,
};
/* Particles (PM1.0 / PM2.5 / PM10). */
static const uint16_t icon_pm[ICON_H] = {
    0x0000, 0x0000, 0x1C00, 0x3E00, 0x3E00, 0x1C00, 0x03E0, 0x07F0,
    0x07F0, 0x07FC, 0x07FE, 0x03FE, 0x00FE, 0x00FE, 0x007C, 0x0000,
};
/* House (header). */
static const uint16_t icon_home[ICON_H] = {
    0x0000, 0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x7FFE,
    0xFFFF, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0000,
};

/* UI palette (RGB565). */
#define COLOR_HEADER_BG    (0x0010) /* dark blue */
#define COLOR_LABEL        (0xBDF7) /* light gray */
#define COLOR_DATE         (0x7BEF) /* gray */
#define COLOR_DIVIDER      (0x2104) /* dark gray */
#define COLOR_PANEL        (0x1084) /* dark blue-gray metric card */
#define COLOR_TEMP         (0xFD20) /* orange */
#define COLOR_HUM          (0x07FF) /* cyan */
#define COLOR_PRES         (0x049F) /* sky blue */
#define COLOR_PM1          (0xBDF7) /* light gray */
#define COLOR_PM25         (0xFA5F) /* soft pink (legible on dark panel) */
#define COLOR_PM10         (0xFFE0) /* yellow */

/* CO2 air-quality colors (RGB565). */
#define COLOR_CO2_GOOD     (0x07E0) /* green  (<1000)     */
#define COLOR_CO2_FAIR     (0xFFE0) /* yellow (1000-1499) */
#define COLOR_CO2_POOR     (0xFD20) /* orange (1500-1999) */
#define COLOR_CO2_BAD      (0xF800) /* red    (>=2000)    */
#define COLOR_CO2_BAD_DIM  (0x8000) /* dim red (blink)    */
#define COLOR_CO2_INVALID  (0x7BEF) /* gray, no data      */

/* CO2 air-quality levels. */
typedef enum {
    CO2_STATUS_GOOD = 0,
    CO2_STATUS_FAIR,
    CO2_STATUS_POOR,
    CO2_STATUS_BAD,
} co2_status_t;

/* --- Metric grid geometry --- */
#define GRID_COLS    (2)
#define GRID_GAP     (2)
#define CELL_W       ((SCREEN_LCD_H_RES - GRID_GAP) / GRID_COLS) /* 63 */
#define CELL_H       (26)
#define GRID_Y0      (80)
#define GRID_ROW_H   (CELL_H + 1)

/******************************* FUNCTIONS IMPLEMENTATION *******************************/
static void fb_set_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    if (x >= SCREEN_LCD_H_RES || y >= SCREEN_LCD_V_RES) {
        return;
    }
    /* Store big-endian (high byte first) for correct ST7735 colors. */
    fb[y][x * 2]     = (color >> 8) & 0xFF;
    fb[y][x * 2 + 1] = color & 0xFF;
}

static void fb_clear(uint16_t color)
{
    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo = color & 0xFF;
    for (int y = 0; y < SCREEN_LCD_V_RES; y++) {
        for (int x = 0; x < SCREEN_LCD_H_RES; x++) {
            fb[y][x * 2]     = hi;
            fb[y][x * 2 + 1] = lo;
        }
    }
}

static void fb_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    for (uint16_t yy = y; yy < y + h && yy < SCREEN_LCD_V_RES; yy++) {
        for (uint16_t xx = x; xx < x + w && xx < SCREEN_LCD_H_RES; xx++) {
            fb_set_pixel(xx, yy, color);
        }
    }
}

static void fb_fill_round_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               uint16_t r, uint16_t color)
{
    if (r == 0 || 2 * r >= w || 2 * r >= h) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }
    /* Center + horizontal/vertical bands. */
    fb_fill_rect(x + r, y, w - 2 * r, h, color);
    fb_fill_rect(x, y + r, r, h - 2 * r, color);
    fb_fill_rect(x + w - r, y + r, r, h - 2 * r, color);
    /* Four rounded corners (quarter circles). */
    uint16_t rr = (uint16_t)((r - 1) * (r - 1) + (r - 1));
    for (uint16_t cy = 0; cy < r; cy++) {
        for (uint16_t cx = 0; cx < r; cx++) {
            uint16_t dx = (r - 1) - cx;
            uint16_t dy = (r - 1) - cy;
            if (dx * dx + dy * dy <= rr) {
                fb_set_pixel(x + cx,         y + cy, color);          /* TL */
                fb_set_pixel(x + w - 1 - cx, y + cy, color);          /* TR */
                fb_set_pixel(x + cx,         y + h - 1 - cy, color);  /* BL */
                fb_set_pixel(x + w - 1 - cx, y + h - 1 - cy, color);  /* BR */
            }
        }
    }
}

static void fb_draw_char(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg)
{
    const uint8_t *glyph;
    if (c == 0xB0) {
        glyph = degree_glyph;
    } else if (c < 0x20 || c > 0x7E) {
        glyph = font5x7['?' - 0x20];
    } else {
        glyph = font5x7[c - 0x20];
    }
    if (bg != COLOR_BLACK) {
        fb_fill_rect(x, y, FONT_W, FONT_H, bg);
    }
    for (int col = 0; col < FONT_W; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            fb_set_pixel(x + col, y + row, (line & (1 << row)) ? fg : bg);
        }
    }
}

static void fb_draw_string(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        fb_draw_char(x, y, *str, fg, bg);
        x += CHAR_W;
        str++;
    }
}

static void fb_draw_string_centered(uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    size_t len = strlen(str);
    uint16_t w = len * CHAR_W;
    uint16_t x = (w >= SCREEN_LCD_H_RES) ? 0 : (SCREEN_LCD_H_RES - w) / 2;
    fb_draw_string(x, y, str, fg, bg);
}

static void fb_draw_string_right(uint16_t x_right, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    size_t len = strlen(str);
    uint16_t w = len * CHAR_W;
    uint16_t x = (w > x_right) ? 0 : x_right - w;
    fb_draw_string(x, y, str, fg, bg);
}

static void fb_draw_char_scaled(uint16_t x, uint16_t y, char c, uint16_t fg, uint16_t bg, uint8_t scale)
{
    const uint8_t *glyph;
    if (c == 0xB0) {
        glyph = degree_glyph;
    } else if (c < 0x20 || c > 0x7E) {
        glyph = font5x7['?' - 0x20];
    } else {
        glyph = font5x7[c - 0x20];
    }
    if (bg != COLOR_BLACK) {
        fb_fill_rect(x, y, FONT_W * scale, FONT_H * scale, bg);
    }
    for (int col = 0; col < FONT_W; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < FONT_H; row++) {
            if (line & (1 << row)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        fb_set_pixel(x + col * scale + sx, y + row * scale + sy, fg);
                    }
                }
            }
        }
    }
}

static void fb_draw_string_scaled(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
    while (*str) {
        fb_draw_char_scaled(x, y, *str, fg, bg, scale);
        x += CHAR_W * scale;
        str++;
    }
}

static void fb_draw_string_scaled_centered(uint16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t scale)
{
    size_t len = strlen(str);
    uint16_t w = len * CHAR_W * scale;
    uint16_t x = (w >= SCREEN_LCD_H_RES) ? 0 : (SCREEN_LCD_H_RES - w) / 2;
    fb_draw_string_scaled(x, y, str, fg, bg, scale);
}

static void fb_draw_icon(uint16_t x, uint16_t y, const uint16_t *rows, uint16_t color)
{
    for (int r = 0; r < ICON_H; r++) {
        uint16_t mask = rows[r];
        for (int c = 0; c < ICON_W; c++) {
            if (mask & (0x8000 >> c)) {
                fb_set_pixel(x + c, y + r, color);
            }
        }
    }
}

static void fb_flush(void)
{
    esp_lcd_panel_draw_bitmap(screen_control.panel, 0, 0,
                              SCREEN_LCD_H_RES, SCREEN_LCD_V_RES, fb);
}

/* CO2 air-quality helpers. */
static co2_status_t co2_get_status(int co2)
{
    if (co2 >= 2000) {
        return CO2_STATUS_BAD;
    }
    if (co2 >= 1500) {
        return CO2_STATUS_POOR;
    }
    if (co2 >= 1000) {
        return CO2_STATUS_FAIR;
    }
    return CO2_STATUS_GOOD;
}

static uint16_t co2_status_color(co2_status_t status)
{
    switch (status) {
    case CO2_STATUS_GOOD: return COLOR_CO2_GOOD;
    case CO2_STATUS_FAIR: return COLOR_CO2_FAIR;
    case CO2_STATUS_POOR: return COLOR_CO2_POOR;
    case CO2_STATUS_BAD:  return COLOR_CO2_BAD;
    default:              return COLOR_CO2_GOOD;
    }
}

static const char *co2_status_text(co2_status_t status)
{
    switch (status) {
    case CO2_STATUS_GOOD: return "GOOD";
    case CO2_STATUS_FAIR: return "FAIR";
    case CO2_STATUS_POOR: return "POOR";
    case CO2_STATUS_BAD:  return "BAD";
    default:              return "????";
    }
}

/* Draw one metric cell of the 2-column grid. */
static void render_metric_cell(uint16_t x, uint16_t y, const uint16_t *icon,
                               const char *label, const char *value, uint16_t color)
{
    fb_fill_rect(x, y, CELL_W, CELL_H, COLOR_PANEL);
    fb_fill_rect(x, y, 2, CELL_H, color);              /* left accent bar */
    fb_draw_icon(x + 5, y + 1, icon, color);
    fb_draw_string(x + 24, y + 4, label, COLOR_LABEL, COLOR_PANEL);
    fb_draw_string(x + 5, y + 18, value, color, COLOR_PANEL);
}

static void screen_render(sensor_data_t *data, uint8_t frame)
{
    char line[24];
    char time_str[16];
    char date_str[16];

    fb_clear(COLOR_BLACK);

    /* ---- Header bar ---- */
    fb_fill_rect(0, 0, SCREEN_LCD_H_RES, 16, COLOR_HEADER_BG);
    fb_draw_icon(4, 0, icon_home, COLOR_CYAN);
    fb_draw_string(24, 4, "HOME APP", COLOR_WHITE, COLOR_HEADER_BG);

    /* Air-quality dot in header (CO2 status). */
    co2_status_t head_status = CO2_STATUS_GOOD;
    uint16_t dot_color = COLOR_CO2_INVALID;
    if (data->scd40_valid) {
        head_status = co2_get_status(data->co2);
        dot_color = co2_status_color(head_status);
        if (head_status == CO2_STATUS_BAD && (frame & 1)) {
            dot_color = COLOR_CO2_BAD_DIM;
        }
    }
    fb_fill_round_rect(118, 4, 8, 8, 3, dot_color);

    /* ---- Clock (large) ---- */
    if (time_control_get_time_str(time_str, sizeof(time_str), "%H:%M:%S") == APP_OK) {
        fb_draw_string_scaled_centered(18, time_str, COLOR_WHITE, COLOR_BLACK, 2);
    } else {
        fb_draw_string_scaled_centered(18, "--:--:--", COLOR_WHITE, COLOR_BLACK, 2);
    }

    /* ---- Date ---- */
    if (time_control_get_time_str(date_str, sizeof(date_str), "%d/%m/%Y") == APP_OK) {
        fb_draw_string_centered(36, date_str, COLOR_DATE, COLOR_BLACK);
    }

    /* ---- Divider ---- */
    fb_fill_rect(0, 44, SCREEN_LCD_H_RES, 1, COLOR_DIVIDER);

    /* ---- CO2 hero card (color-coded by air quality) ---- */
    const uint16_t card_x = 2;
    const uint16_t card_y = 46;
    const uint16_t card_w = SCREEN_LCD_H_RES - 4;   /* 124 */
    const uint16_t card_h = 32;
    co2_status_t co2_status = CO2_STATUS_GOOD;
    uint16_t card_bg = COLOR_CO2_INVALID;

    if (data->scd40_valid) {
        co2_status = co2_get_status(data->co2);
        card_bg = co2_status_color(co2_status);
        if (co2_status == CO2_STATUS_BAD && (frame & 1)) {
            card_bg = COLOR_CO2_BAD_DIM;   /* blink on bad air */
        }
    }
    fb_fill_round_rect(card_x, card_y, card_w, card_h, 4, card_bg);

    /* Icon + label + status text (left side of card). */
    fb_draw_icon(card_x + 5, card_y + 2, icon_cloud, COLOR_BLACK);
    fb_draw_string(card_x + 23, card_y + 2, "CO2", COLOR_BLACK, card_bg);
    if (data->scd40_valid) {
        fb_draw_string(card_x + 23, card_y + 11,
                       co2_status_text(co2_status), COLOR_BLACK, card_bg);
    }

    /* Big value + unit (right side of card). */
    if (data->scd40_valid) {
        snprintf(line, sizeof(line), "%d", data->co2);
    } else {
        strcpy(line, "--");
    }
    uint16_t val_w = (uint16_t)(strlen(line) * CHAR_W * 2);
    uint16_t val_x = card_x + card_w - 6 - val_w;
    fb_draw_string_scaled(val_x, card_y + 1, line, COLOR_BLACK, card_bg, 2);
    fb_draw_string_right(card_x + card_w - 6, card_y + 24, "ppm",
                         COLOR_BLACK, card_bg);

    /* ---- Metric grid (2 columns) ---- */
    char temp_str[12], hum_str[12], pres_str[12];
    char pm1_str[8], pm25_str[8], pm10_str[8];

    if (data->env_valid) {
        snprintf(temp_str, sizeof(temp_str), "%.1f", data->temperature);
        size_t n = strlen(temp_str);
        temp_str[n]     = 0xB0;
        temp_str[n + 1] = 'C';
        temp_str[n + 2] = '\0';
        snprintf(hum_str, sizeof(hum_str), "%.1f%%", data->humidity);
        snprintf(pres_str, sizeof(pres_str), "%.0f hPa", data->pressure);
    } else {
        strcpy(temp_str, "--.-");
        strcpy(hum_str, "--.-%");
        strcpy(pres_str, "-- hPa");
    }

    if (data->pms_valid) {
        snprintf(pm1_str,  sizeof(pm1_str),  "%d", data->pm1_0_atm);
        snprintf(pm25_str, sizeof(pm25_str), "%d", data->pm2_5_atm);
        snprintf(pm10_str, sizeof(pm10_str), "%d", data->pm10_atm);
    } else {
        strcpy(pm1_str,  "--");
        strcpy(pm25_str, "--");
        strcpy(pm10_str, "--");
    }

    uint16_t gy = GRID_Y0;
    render_metric_cell(0, gy, icon_thermo, "TEMP", temp_str, COLOR_TEMP);
    render_metric_cell(CELL_W + GRID_GAP, gy, icon_drop, "HUM", hum_str, COLOR_HUM);
    gy += GRID_ROW_H;

    render_metric_cell(0, gy, icon_gauge, "PRES", pres_str, COLOR_PRES);
    render_metric_cell(CELL_W + GRID_GAP, gy, icon_pm, "PM1.0", pm1_str, COLOR_PM1);
    gy += GRID_ROW_H;

    render_metric_cell(0, gy, icon_pm, "PM2.5", pm25_str, COLOR_PM25);
    render_metric_cell(CELL_W + GRID_GAP, gy, icon_pm, "PM10", pm10_str, COLOR_PM10);
}

static void screen_task(void *arg)
{
    sensor_data_t data;
    uint8_t frame = 0;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        memset(&data, 0, sizeof(data));
        sensor_get_data(&data);

        screen_render(&data, frame++);
        fb_flush();

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SCREEN_REFRESH_PERIOD_MS));
    }
}

APP_RESULT screen_init()
{
    APP_RESULT ret = APP_OK;
    esp_err_t err = ESP_OK;

    /* --- Backlight --- */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = (1ULL << SCREEN_BL_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(SCREEN_BL_PIN, SCREEN_BL_ON_LEVEL);

    /* --- SPI bus --- */
    spi_bus_config_t bus_cfg = st7735_PANEL_BUS_SPI_CONFIG(
        SCREEN_SPI_SCLK_PIN, SCREEN_SPI_MOSI_PIN, SCREEN_LCD_MAX_TRANS);
    err = spi_bus_initialize(SCREEN_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    /* --- Panel IO --- */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = st7735_PANEL_IO_SPI_CONFIG(
        SCREEN_SPI_CS_PIN, SCREEN_SPI_DC_PIN, NULL, NULL);
    err = esp_lcd_new_panel_io_spi(SCREEN_SPI_HOST, &io_cfg, &io_handle);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    /* --- Panel --- */
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = SCREEN_SPI_RST_PIN,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .vendor_config  = NULL,   /* use the library's default init sequence */
    };
    err = esp_lcd_new_panel_st7735(io_handle, &panel_cfg, &screen_control.panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    err = esp_lcd_panel_reset(screen_control.panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    err = esp_lcd_panel_init(screen_control.panel);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

#if SCREEN_ST7735_INVERT
    err = esp_lcd_panel_invert_color(screen_control.panel, true);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);
#endif

    err = esp_lcd_panel_disp_on_off(screen_control.panel, true);
    ASSERT_CRITICAL(err == ESP_OK, err, APP_ERROR);

    ESP_LOGI(THIS_MODULE_NAME, "ST7735 initialized (128x160)");

    /* --- Display task --- */
    BaseType_t task_ok = xTaskCreate(screen_task, "screen_task",
                                     SCREEN_TASK_STACK_SIZE, NULL,
                                     SCREEN_TASK_PRIORITY, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(THIS_MODULE_NAME, "Failed to create screen task");
        return APP_ERROR;
    }

    return ret;
}
