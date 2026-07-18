idf_component_register(SRCS "src/cc1101.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_driver_spi esp_driver_gpio freertos esp_rom log)
