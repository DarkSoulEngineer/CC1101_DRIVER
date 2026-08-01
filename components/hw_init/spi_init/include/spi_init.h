#pragma once

#include "esp_err.h"
#include "driver/spi_master.h"

extern spi_device_handle_t cc1101_handle;

esp_err_t hw_spi_init(int mosi_io, int miso_io, int sclk_io, int cs_io,
                      uint32_t speed_hz);
