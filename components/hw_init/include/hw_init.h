#ifndef HW_INIT_H
#define HW_INIT_H

#include "driver/gpio.h"
#include "driver/spi_master.h" // SPI
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

// Shared SPI bus (both radios on SPI3_HOST)
#define PIN_NUM_MISO   13
#define PIN_NUM_MOSI   11
#define PIN_NUM_CLK    12

// Radio A - transmitter / fuzzer
#define PIN_NUM_CS_A   9
#define PIN_NUM_GDO0_A 4

// Radio B - receiver / target
#define PIN_NUM_CS_B   10
#define PIN_NUM_GDO0_B 5

extern spi_device_handle_t cc1101_handle;

extern SemaphoreHandle_t tx_done_sem_a;
extern SemaphoreHandle_t tx_done_sem_b;

extern SemaphoreHandle_t cc1101_bus_mutex;

esp_err_t init_hardware(void);

#endif // HW_INIT_H
