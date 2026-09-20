#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool confirmed;
    int64_t spi_transfer_us;
    int64_t gdo0_confirm_us;
} rfuzz_tx_result_t;

esp_err_t rfuzz_tx_init(void);

esp_err_t rfuzz_tx_send(const uint8_t *payload, size_t len, rfuzz_tx_result_t *result);

#ifdef __cplusplus
}
#endif