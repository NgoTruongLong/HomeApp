set(COMMON_DIR ".")
set(COMMON_SRCS "commons.c")

idf_component_register(SRCS ${COMMON_SRCS}
                       INCLUDE_DIRS ${COMMON_DIR}
                       REQUIRES "" 
                       PRIV_REQUIRES "")
