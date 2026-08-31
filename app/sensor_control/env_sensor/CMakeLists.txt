set(ENV_SENSOR_DIR ".")

set(ENV_SENSOR_SRCS "env_sensor.c")

idf_component_register(SRCS ${ENV_SENSOR_SRCS}
                       INCLUDE_DIRS ${ENV_SENSOR_DIR}
                       REQUIRES "esp_driver_i2c" "i2c_bus"
                       PRIV_REQUIRES "freertos" "log")
