idf_component_register(
    SRCS "src/sump_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_driver_dma esp_hw_support soc esp_rom
               esp_driver_ledc esp_driver_gpio
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_uart
)
