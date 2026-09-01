set(MAIN_DIRS ".")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/common")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/pump_control")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/sensor_control")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/network")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/network/time_control")
list(APPEND MAIN_DIRS "${PROJECT_DIR}/app/screen")

idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash
                       REQUIRES network time_control screen "driver"
                       INCLUDE_DIRS "${MAIN_DIRS}")
