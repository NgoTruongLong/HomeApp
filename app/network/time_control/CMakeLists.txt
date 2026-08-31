set(TIME_DIR ".")
list(APPEND TIME_DIR "${PROJECT_DIR}/app/common")

set(TIME_SRCS "time_control.c")

idf_component_register(SRCS ${TIME_SRCS}
                       INCLUDE_DIRS ${TIME_DIR}
                       REQUIRES "esp_netif" "lwip"
                       PRIV_REQUIRES "")
