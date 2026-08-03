idf_component_register(
    SRCS "src/hw_init.c"
         "src/spi_init.c"
         "src/usb_interface.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_usb_serial_jtag
             freertos log)
