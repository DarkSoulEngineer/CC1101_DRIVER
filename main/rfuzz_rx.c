/*
 * RFuzz RX module - Radio B (target / receiver side).
 * Imported by main.c as part of the single translation unit.
 * Shared helpers (rfuzz_log_hex, rfuzz_radio_init, RFUZZ_LINK_*)
 * are defined in main.c BEFORE this file is #included.
 */
#ifndef RFUZZ_RX_C
#define RFUZZ_RX_C

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw_init.h"
#include "cc1101.h"

/* ============================================================
 * MODULE INTERFACE (types + API, visible to main.c via include)
 * ============================================================ */
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

/* ============================================================
 * IMPLEMENTATION (private to module)
 * ============================================================ */
static const char *RX_TAG = "RFUZZ_RX";

#define RX_POLL_MS      10
#define RX_BUS_LOCK_MS  50

static cc1101_handle_t s_rx_radio;

static float rssi_to_dbm(int8_t raw)
{
    return (raw / 2.0f) - 74.0f;
}

esp_err_t rfuzz_rx_init(void)
{
    if (rfuzz_radio_init(&s_rx_radio, "Radio B", PIN_NUM_CS_B, PIN_NUM_GDO0_B) != ESP_OK)
        return ESP_FAIL;

    cc1101_set_rx_mode(&s_rx_radio);

    return ESP_OK;
}

esp_err_t rfuzz_rx_wait_window(uint32_t window_ms,
                               const uint8_t *expected,
                               size_t expected_len,
                               rfuzz_rx_report_t *report)
{
    int64_t window_start = esp_timer_get_time();

    memset(report, 0, sizeof(*report));
    report->result = RFUZZ_RX_TIMEOUT;
    report->lqi = -1;

    ESP_LOGI(RX_TAG, "RX window open (%u ms), waiting for %u-byte reference packet...",
             (unsigned)window_ms, (unsigned)expected_len);

    while ((esp_timer_get_time() - window_start) < (int64_t)window_ms * 1000)
    {
        size_t len = CC1101_MAX_PACKET_LEN;

        if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(RX_BUS_LOCK_MS)) != pdTRUE)
            break;

        bool crc_ok = false;
        bool got = cc1101_poll_packet(&s_rx_radio, report->payload, &len, &crc_ok);
        xSemaphoreGive(cc1101_bus_mutex);

        if (!got)
        {
            vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
            continue;
        }

        report->len = len;
        report->latency_us = esp_timer_get_time() - window_start;
        report->rssi_dbm = rssi_to_dbm((int8_t)cc1101_read_status_reg(&s_rx_radio, CC1101_RSSI));
        report->lqi = (int8_t)cc1101_read_status_reg(&s_rx_radio, CC1101_LQI);

        if (!crc_ok)
        {
            report->result = RFUZZ_RX_CRC_FAIL;
            ESP_LOGW(RX_TAG, "RX CRC-FAIL: frame dropped at RF (len=%u) rssi=%.1fdBm lqi=%d",
                     (unsigned)len, report->rssi_dbm, report->lqi);
            rfuzz_log_hex(RX_TAG, "CRC-FAIL payload:", report->payload, len);
            return ESP_OK;
        }

        if (len == expected_len && memcmp(report->payload, expected, expected_len) == 0)
        {
            report->result = RFUZZ_RX_VALID;
            ESP_LOGI(RX_TAG, "RX HIT after %lldus: len=%u crc=OK rssi=%.1fdBm lqi=%d",
                     (long long)report->latency_us, (unsigned)len,
                     report->rssi_dbm, report->lqi);
            rfuzz_log_hex(RX_TAG, "RX payload:", report->payload, len);
        }
        else
        {
            report->result = RFUZZ_RX_CORRUPT;
            ESP_LOGW(RX_TAG, "RX CORRUPT packet (len=%u) rssi=%.1fdBm lqi=%d",
                     (unsigned)len, report->rssi_dbm, report->lqi);
            rfuzz_log_hex(RX_TAG, "CORRUPT payload:", report->payload, len);
        }
        return ESP_OK;
    }

    report->window_us = esp_timer_get_time() - window_start;
    report->rssi_dbm = rssi_to_dbm((int8_t)cc1101_read_status_reg(&s_rx_radio, CC1101_RSSI));
    report->marcstate = cc1101_read_status_reg(&s_rx_radio, CC1101_MARCSTATE);
    report->rxbytes = cc1101_read_status_reg(&s_rx_radio, CC1101_RXBYTES);
    cc1101_set_rx_mode(&s_rx_radio);

    ESP_LOGW(RX_TAG, "RX window closed: no reference packet "
                  "(window=%lldus rssi=%.1fdBm marcstate=0x%02X rxbytes=0x%02X)",
             (long long)report->window_us, report->rssi_dbm,
             report->marcstate, report->rxbytes);

    return ESP_OK;
}

#endif /* RFUZZ_RX_C */