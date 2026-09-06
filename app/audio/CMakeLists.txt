set(AUDIO_DIRS ".")
list(APPEND AUDIO_DIRS "${PROJECT_DIR}/app/common")

set(AUDIO_SRCS "audio_control.c" "max98357a.c")

idf_component_register(SRCS ${AUDIO_SRCS}
                       INCLUDE_DIRS ${AUDIO_DIRS}
                       REQUIRES "esp_driver_i2s" "micro_sdcard"
                       PRIV_REQUIRES "freertos" "log")
