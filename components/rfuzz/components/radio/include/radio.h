#ifndef RADIO_H
#define RADIO_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cc1101.h"

esp_err_t rfuzz_radio_init(cc1101_handle_t *dev, const char *name,
                           gpio_num_t cs_pin, gpio_num_t gdo0_pin);
void rfuzz_log_hex(const char *tag, const char *prefix, const uint8_t *buf, size_t len);

#endif
