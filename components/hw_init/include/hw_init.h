#ifndef HW_INIT_H
#define HW_INIT_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef CONFIG_CC1101_PIN_GDO0
#define PIN_NUM_GDO0 CONFIG_CC1101_PIN_GDO0
#else
#define PIN_NUM_GDO0 4
#endif

#ifdef CONFIG_CC1101_PIN_CS
#define PIN_NUM_CS CONFIG_CC1101_PIN_CS
#else
#define PIN_NUM_CS 5
#endif

#ifdef CONFIG_CC1101_PIN_SCK
#define PIN_NUM_CLK CONFIG_CC1101_PIN_SCK
#else
#define PIN_NUM_CLK 15
#endif

#ifdef CONFIG_CC1101_PIN_MOSI
#define PIN_NUM_MOSI CONFIG_CC1101_PIN_MOSI
#else
#define PIN_NUM_MOSI 7
#endif

#ifdef CONFIG_CC1101_PIN_MISO
#define PIN_NUM_MISO CONFIG_CC1101_PIN_MISO
#else
#define PIN_NUM_MISO 6
#endif

#ifdef CONFIG_CC1101_SPI_HZ
#define CC1101_SPI_SPEED_HZ CONFIG_CC1101_SPI_HZ
#else
#define CC1101_SPI_SPEED_HZ 1000000
#endif

extern spi_device_handle_t cc1101_handle;

esp_err_t init_hardware(void);
esp_err_t hw_init_gdo0_input(void);

#endif
