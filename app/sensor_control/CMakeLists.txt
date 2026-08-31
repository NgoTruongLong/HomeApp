set(SENSOR_DIR ".")
list(APPEND SENSOR_DIR "${PROJECT_DIR}/app/common")

set(SENSOR_SRCS "sensor_control.c")

idf_component_register(SRCS ${SENSOR_SRCS}
                       INCLUDE_DIRS ${SENSOR_DIR}
                       REQUIRES "i2c_bus" "env_sensor" "pms7003" "scd40" "esp_timer" "esp_driver_i2c"
                       PRIV_REQUIRES "driver")
