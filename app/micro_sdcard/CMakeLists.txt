set(MICRO_SDCARD_DIR ".")
list(APPEND MICRO_SDCARD_DIR "${PROJECT_DIR}/app/common")

set(MICRO_SDCARD_SRCS "micro_sdcard_control.c")

idf_component_register(SRCS ${MICRO_SDCARD_SRCS}
                       INCLUDE_DIRS ${MICRO_SDCARD_DIR}
                       REQUIRES fatfs sdmmc
                       PRIV_REQUIRES "driver" "esp_driver_sdspi")
