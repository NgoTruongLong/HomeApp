set(I2C_BUS_DIR ".")

set(I2C_BUS_SRCS "i2c_bus.c")

idf_component_register(SRCS ${I2C_BUS_SRCS}
                       INCLUDE_DIRS ${I2C_BUS_DIR}
                       REQUIRES "esp_driver_i2c" "driver"
                       PRIV_REQUIRES "log")
