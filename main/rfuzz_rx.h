#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cc1101.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RFUZZ_RX_TIMEOUT = 0,
    RFUZZ_RX_VALID,
    RFUZZ_RX_CORRUPT,
    RFUZZ_RX_CRC_FAIL
} rfuzz_rx_result_t;

typedef struct {
    rfuzz_rx_result_t result;
    size_t len;
    uint8_t payload[CC1101_MAX_PACKET_LEN];
    float rssi_dbm;
    int8_t lqi;
    int64_t latency_us;
    int64_t window_us;
    uint8_t marcstate;
    uint8_t rxbytes;
} rfuzz_rx_report_t;

esp_err_t rfuzz_rx_init(void);

esp_err_t rfuzz_rx_wait_window(uint32_t window_ms,
                               const uint8_t *expected,
                               size_t expected_len,
                               rfuzz_rx_report_t *report);

#ifdef __cplusplus
}
#endif