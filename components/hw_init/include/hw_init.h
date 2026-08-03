#ifndef HW_INIT_H
#define HW_INIT_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "sdkconfig.h"

#include "spi_init.h"
#include "usb_interface.h"

#ifdef CONFIG_CC1101_PIN_GDO0
#define PIN_NUM_GDO0 CONFIG_CC1101_PIN_GDO0
#else
#define PIN_NUM_GDO0 3
#endif

#ifdef CONFIG_CC1101_PIN_GDO2
#define PIN_NUM_GDO2 CONFIG_CC1101_PIN_GDO2
#else
#define PIN_NUM_GDO2 4
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

/* SPI device handle for the CC1101, created by init_hardware(). */
extern spi_device_handle_t hw_cc1101_spi;

/* Initialize board peripherals: SPI bus + CC1101 device, native USB. */
esp_err_t init_hardware(void);

/* Configure the GDO status/data pins as digital inputs. */
esp_err_t hw_init_gdo0_input(void);
esp_err_t hw_init_gdo2_input(void);

#endif
