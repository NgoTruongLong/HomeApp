set(WIFI_DIR ".")
list(APPEND WIFI_DIR "${PROJECT_DIR}/app/common")

set(WIFI_SRCS "wifi_control.c")

idf_component_register(SRCS ${WIFI_SRCS}
                       INCLUDE_DIRS ${WIFI_DIR}
                       REQUIRES "nvs_flash" "esp_wifi" "esp_netif" "esp_event"
                       PRIV_REQUIRES "")
