idf_component_register(SRCS "src/spi_init.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_driver_spi esp_driver_gpio)
