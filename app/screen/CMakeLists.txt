set(SCREEN_DIR ".")
list(APPEND SCREEN_DIR "${PROJECT_DIR}/app/common")

set(SCREEN_SRCS "screen_control.c")

idf_component_register(SRCS ${SCREEN_SRCS}
                       INCLUDE_DIRS ${SCREEN_DIR}
                       REQUIRES "esp_lcd" "esp_driver_spi" "esp_timer" "sensor_control" "time_control" "teriyakigod__esp_lcd_st7735"
                       PRIV_REQUIRES "freertos")
