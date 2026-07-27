#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define SUMP_ID_RESPONSE    "1ALS"
#define SUMP_VERSION        0x31
#define SUMP_MAX_CHANNELS   2

#define SUMP_CMD_RESET              0x00
#define SUMP_CMD_RUN                0x01
#define SUMP_CMD_ID                 0x02
#define SUMP_CMD_GET_METADATA       0x04
#define SUMP_CMD_SET_DIVIDER        0x80
#define SUMP_CMD_SET_COUNT          0x81
#define SUMP_CMD_SET_FLAGS          0x82
#define SUMP_CMD_TRIGGER_MASK0      0xC0
#define SUMP_CMD_TRIGGER_VALUE0     0xC1
#define SUMP_CMD_TRIGGER_CONFIG0    0xC2
#define SUMP_CMD_TRIGGER_MASK1      0xC3
#define SUMP_CMD_TRIGGER_VALUE1     0xC4
#define SUMP_CMD_TRIGGER_CONFIG1    0xC5
#define SUMP_CMD_TRIGGER_MASK2      0xC6
#define SUMP_CMD_TRIGGER_VALUE2     0xC7
#define SUMP_CMD_TRIGGER_CONFIG2    0xC8
#define SUMP_CMD_TRIGGER_MASK3      0xC9
#define SUMP_CMD_TRIGGER_VALUE3     0xCA
#define SUMP_CMD_TRIGGER_CONFIG3    0xCB

typedef struct {
    gpio_num_t          gdo0_pin;
    gpio_num_t          gdo2_pin;
    uint8_t             num_channels;
    uint32_t            clock_freq;
    uint32_t            sample_rate;
    uint32_t            divider;
    uint32_t            read_count;
    uint32_t            delay_count;
    uint8_t             trigger_mask;
    uint8_t             trigger_value;
    volatile bool       armed;
    volatile bool       sampling;
    volatile bool       capture_done;
} sump_state_t;

esp_err_t sump_capture_init(gpio_num_t gdo0_pin, gpio_num_t gdo2_pin);
esp_err_t sump_capture_start(void);
esp_err_t sump_capture_stop(void);
