# App variant to build. Override at configure time, e.g.:
#   idf.py build -DSUMP_APP=RFuzz_RX
set(SUMP_APP "main" CACHE STRING "app to build: main | RFuzz_RX | RFuzz_TX")

idf_component_register(SRCS "${SUMP_APP}.c"
                    INCLUDE_DIRS ""
                    REQUIRES hw_init cc1101 sump_capture freertos log)
