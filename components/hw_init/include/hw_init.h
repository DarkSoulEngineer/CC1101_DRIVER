#ifndef HW_INIT_H
#define HW_INIT_H

#include "driver/gpio.h"
#include "driver/spi_master.h" // SPI
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

// Pin Definitions
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5
#define PIN_NUM_GDO0 25

extern spi_device_handle_t cc1101_handle;
extern SemaphoreHandle_t tx_done_sem;

esp_err_t init_hardware(void);

#endif // HW_INIT_H
