idf_component_register(SRCS "src/rfuzz.c"
                     INCLUDE_DIRS "include"
                     REQUIRES terminal profile radio hw_init cc1101 freertos log)