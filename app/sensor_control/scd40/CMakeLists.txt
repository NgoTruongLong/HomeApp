set(SCD40_DIR ".")

set(SCD40_SRCS "scd40.c")

idf_component_register(SRCS ${SCD40_SRCS}
                       INCLUDE_DIRS ${SCD40_DIR}
                       REQUIRES "esp_driver_i2c" "i2c_bus" "common"
                       PRIV_REQUIRES "freertos" "log")
