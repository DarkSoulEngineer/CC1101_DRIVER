#ifndef RX_H
#define RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cc1101.h"

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

esp_err_t rfuzz_rx_capture_stop(void);
esp_err_t rfuzz_rx_apply_profile_locked(cc1101_modulation_t modulation,
                                        cc1101_rx_format_t format);
esp_err_t rfuzz_rx_start_locked(void);
esp_err_t rfuzz_rx_idle_locked(void);
esp_err_t rfuzz_rx_profile_readback_locked(cc1101_profile_status_t *out);
bool rfuzz_rx_is_raw(void);
bool rfuzz_rx_is_packet(void);
void rfuzz_rx_status(void);
void rfuzz_rx_raw_service(void);
void rfuzz_rx_listen_poll(void);
void rfuzz_rx_set_paused(bool paused);
bool rfuzz_rx_is_paused(void);

#endif
