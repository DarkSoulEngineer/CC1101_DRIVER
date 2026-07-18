#ifndef HW_INIT_H
#define HW_INIT_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define PIN_NUM_GDO0 25

extern spi_device_handle_t cc1101_handle;

esp_err_t init_hardware(void);
esp_err_t hw_init_gdo0_input(void);

#endif
