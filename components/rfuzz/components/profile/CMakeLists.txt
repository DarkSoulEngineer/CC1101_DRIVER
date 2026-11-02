idf_component_register(SRCS "src/profile.c"
                     INCLUDE_DIRS "include"
                     REQUIRES radio cc1101 hw_init freertos log)