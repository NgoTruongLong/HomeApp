set(PUMP_DIR ".")
list(APPEND PUMP_DIR "${PROJECT_DIR}/app/common")

set(PUMP_SRCS "pump_control.c")

idf_component_register(SRCS ${PUMP_SRCS}
                       INCLUDE_DIRS ${PUMP_DIR}
                       REQUIRES "esp_lcd" "esp_timer"
                       PRIV_REQUIRES "driver")
