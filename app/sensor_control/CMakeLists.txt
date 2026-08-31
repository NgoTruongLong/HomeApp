set(SENSOR_DIR ".")
list(APPEND SENSOR_DIR "${PROJECT_DIR}/app/common")

set(SENSOR_SRCS "sensor_control.c")

idf_component_register(SRCS ${SENSOR_SRCS}
                       INCLUDE_DIRS ${SENSOR_DIR}
                       REQUIRES "lutzbellmann__esp32_aht20_bmp280" "petrovgp__esp-pms ""esp_timer"
                       PRIV_REQUIRES "driver")
