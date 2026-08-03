#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "driver/gpio.h"
#include "esp_err.h"

#define CAPTURE_CMD_START  0x01
#define CAPTURE_CMD_TX     0x02
#define CAPTURE_CMD_STREAM 0x03
#define CAPTURE_CMD_STOP   0x04
#define CAPTURE_CMD_SWEEP  0x05

#define CAPTURE_STREAM_BUF_SIZE 2048

typedef struct {
    gpio_num_t  gdo0_pin;
    gpio_num_t  gdo2_pin;
    uint8_t     num_channels;
    uint32_t    sample_rate;
    uint32_t    num_samples;
} capture_state_t;

typedef esp_err_t (*capture_tx_cb_t)(void);
typedef void (*capture_freq_sweep_fn)(uint32_t start_hz, uint32_t end_hz,
                                      uint32_t step_hz);

void capture_set_tx_cb(capture_tx_cb_t cb);
void capture_set_freq_sweep_cb(capture_freq_sweep_fn cb);

/* Send bytes back to the host over the active transport (raw capture data). */
int capture_send(const uint8_t *data, size_t len);

/* True while a finite capture or live stream is active. The application can
 * use this to keep the USB byte stream clean (e.g. pause packet logging). */
bool capture_is_active(void);

esp_err_t capture_init(gpio_num_t gdo0_pin, gpio_num_t gdo2_pin);
