#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define CAPTURE_CMD_START  0x01

typedef struct {
    gpio_num_t  gdo0_pin;
    gpio_num_t  gdo2_pin;
    uint8_t     num_channels;
    uint32_t    sample_rate;
    uint32_t    num_samples;
} capture_state_t;

esp_err_t capture_init(gpio_num_t gdo0_pin, gpio_num_t gdo2_pin);
