idf_component_register(SRCS "src/cc1101.c" # CC1101 Driver
                    INCLUDE_DIRS "include"
                    REQUIRES hw_init driver freertos esp_rom log)
