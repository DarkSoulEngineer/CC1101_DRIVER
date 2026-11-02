idf_component_register(SRCS "src/radio.c" "src/tx.c" "src/rx.c"
                     INCLUDE_DIRS "include"
                     REQUIRES hw_init cc1101 freertos esp_timer log)