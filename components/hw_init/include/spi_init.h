#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

/* Initialize the SPI bus and add one generic device (software-managed CS).
 * On success, *out_dev is filled with the device handle for the caller
 * (typically the CC1101 driver). */
esp_err_t hw_spi_init(int mosi_io, int miso_io, int sclk_io, int cs_io,
                      uint32_t speed_hz, spi_device_handle_t *out_dev);
