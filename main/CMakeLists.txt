idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ""
                    REQUIRES hw_init spi_init usb_interface cc1101 sump_capture freertos log)
