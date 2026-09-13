set(VOICE_DIRS ".")
list(APPEND VOICE_DIRS "${PROJECT_DIR}/app/common")

set(VOICE_SRCS "mic_inmp441.c" "xiaozhi_ws.c" "voice_chat.c")

idf_component_register(SRCS ${VOICE_SRCS}
                       INCLUDE_DIRS ${VOICE_DIRS}
                       REQUIRES "esp_driver_i2s" "esp_wifi" "nvs_flash" "esp_http_client"
                                "json" "esp_websocket_client" "78__esp-opus" "mbedtls"
                                "esp-tls" "esp_app_format" "spi_flash" "audio"
                       PRIV_REQUIRES "freertos" "log")
