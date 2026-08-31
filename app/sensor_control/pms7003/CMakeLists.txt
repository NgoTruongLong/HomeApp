set(PMS7003_DIR ".")

set(PMS7003_SRCS "pms7003.c")

idf_component_register(SRCS ${PMS7003_SRCS}
                       INCLUDE_DIRS ${PMS7003_DIR}
                       REQUIRES "driver"
                       PRIV_REQUIRES "freertos" "log")
