set(SCREEN_DIR ".")
list(APPEND SCREEN_DIR "${PROJECT_DIR}/app/common")
list(APPEND SCREEN_DIR "${PROJECT_DIR}/app/screen/eez_icons")
list(APPEND SCREEN_DIR "${PROJECT_DIR}/app/screen/fonts")

# NOTE: Code ST7735 cũ để ở thư mục st7735_legacy/ (KHÔNG build).
set(SCREEN_SRCS "ui_chat.c" "ui_music.c" "ui_wifi.c" "screen_control.c" "lcd_driver.c" "lvgl_driver.c"
                "ui_common.c" "ui_calendar.c" "ui_stopwatch.c" "ui_wifi.c" "ui_music.c"
                "fonts/lv_font_vn_14.c"
                "fonts/lv_font_vn_16.c"
                "fonts/lv_font_vn_20.c"
                "eez_icons/ui_image_lamp_65.c"
                "eez_icons/ui_image_calendar_65.c"
                "eez_icons/ui_image_cassette_65.c"
                "eez_icons/ui_image_sand_clock_65.c"
                "eez_icons/ui_image_wifi_65.c"
                "eez_icons/ui_image_camera_65.c"
                "eez_icons/ui_image_ai_65.c"
                "eez_icons/ui_image_clock_65.c"
                "eez_icons/ui_image_wifi_connected_25.c"
                "eez_icons/ui_image_wifi_disconnected_25.c")

idf_component_register(SRCS ${SCREEN_SRCS}
                       INCLUDE_DIRS ${SCREEN_DIR}
                       REQUIRES "esp_lcd" "esp_driver_spi" "esp_driver_gpio" "esp_timer"
                                "sensor_control" "time_control" "network" "micro_sdcard"
                                "atanisoft__esp_lcd_ili9488" "lvgl__lvgl" "audio" "voice"
                                "espressif__esp_lcd_touch" "atanisoft__esp_lcd_touch_xpt2046"
                       PRIV_REQUIRES "freertos" "heap" "log")
