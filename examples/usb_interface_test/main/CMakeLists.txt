idf_component_register(SRCS "usb_interface_test.c"
                    INCLUDE_DIRS ""
                    REQUIRES usb_interface esp_driver_usb_serial_jtag freertos log)
