set(SCREEN_DIR ".")
list(APPEND SCREEN_DIR "${PROJECT_DIR}/app/common")

# NOTE: Code ST7735 cũ để ở thư mục st7735_legacy/ (KHÔNG build).
set(SCREEN_SRCS "screen_control.c" "lcd_driver.c" "lvgl_driver.c")

idf_component_register(SRCS ${SCREEN_SRCS}
                       INCLUDE_DIRS ${SCREEN_DIR}
                       REQUIRES "esp_lcd" "esp_driver_spi" "esp_driver_gpio" "esp_timer"
                                "sensor_control" "time_control"
                                "atanisoft__esp_lcd_ili9488" "lvgl__lvgl"
                                "espressif__esp_lcd_touch" "atanisoft__esp_lcd_touch_xpt2046"
                       PRIV_REQUIRES "freertos" "heap" "log")
