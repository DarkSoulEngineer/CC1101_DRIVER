idf_component_register(SRCS "src/cc1101.c"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_driver_gpio esp_driver_spi
                    PRIV_REQUIRES freertos esp_rom esp_timer log)
