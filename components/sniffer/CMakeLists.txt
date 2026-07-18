idf_component_register(SRCS "src/sniffer.c"
                    INCLUDE_DIRS "include"
                    REQUIRES cc1101 hw_init driver esp_driver_rmt esp_driver_uart
                             freertos esp_rom log)
