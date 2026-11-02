idf_component_register(SRCS "src/hw_init.c" "src/hw_capture.c"
                    INCLUDE_DIRS "include"
                    REQUIRES driver esp_driver_spi esp_driver_gpio esp_driver_rmt heap esp_rom esp_timer freertos log)
