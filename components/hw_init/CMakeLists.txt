idf_component_register(SRCS "src/hw_init.c"
                    INCLUDE_DIRS "include"
                    REQUIRES spi_init usb_interface esp_driver_gpio log)
