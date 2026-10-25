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
static cc1101_rx_format_t s_requested = CC1101_RX_PACKET;
static cc1101_rx_format_t s_applied = CC1101_RX_PACKET;
static bool s_mode_valid;
static int64_t s_raw_report_us;
static rfuzz_raw_edge_t s_previous;
static bool s_have_previous;
static char s_raw_samples[768];
static size_t s_raw_sample_len, s_raw_sample_count;
static uint64_t s_sample_drops;

bool rfuzz_rx_is_raw(void) { return s_mode_valid && s_applied == CC1101_RX_RAW_OOK; }
bool rfuzz_rx_is_packet(void) { return s_mode_valid && s_applied == CC1101_RX_PACKET; }

esp_err_t rfuzz_rx_select(cc1101_rx_format_t format)
{
    s_requested = format;
    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(RX_BUS_LOCK_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    rfuzz_b_capture_stop();
    s_mode_valid = false;
    s_have_previous = false;
    s_raw_sample_len = s_raw_sample_count = 0;
    s_raw_samples[0] = '\0';
    s_sample_drops = 0;
    s_raw_report_us = esp_timer_get_time();
    esp_err_t err = rfuzz_b_capture_prepare(format == CC1101_RX_RAW_OOK);
    if (err == ESP_OK) err = cc1101_set_rx_format(&s_rx_radio, format);
    if (err == ESP_OK) err = rfuzz_b_capture_arm();
    if (err == ESP_OK) {
        s_applied = format;
        s_mode_valid = true;
    } else {
        /* No further packet polling/capture until a successful explicit selection. */
        rfuzz_b_capture_stop();
        cc1101_strobe(&s_rx_radio, CC1101_SIDLE);
    }
    xSemaphoreGive(cc1101_bus_mutex);
    ESP_LOGI(RX_TAG, "RX selection %s: %s (failed transition leaves capture stopped)",
             format == CC1101_RX_PACKET ? "packet" : "raw", esp_err_to_name(err));
    return err;
}

void rfuzz_rx_status(void)
{
    rfuzz_raw_stats_t stats;
    rfuzz_b_raw_stats(&stats);
    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(RX_BUS_LOCK_MS)) != pdTRUE) {
        ESP_LOGW(RX_TAG, "status: bus timeout; readback unavailable");
        return;
    }
    cc1101_rx_format_status_t actual;
    cc1101_get_rx_format_status(&s_rx_radio, &actual);
    xSemaphoreGive(cc1101_bus_mutex);
    ESP_LOGI(RX_TAG, "requested=%s applied=%s PKTCTRL0=%02X MDMCFG2=%02X IOCFG0=%02X MARCSTATE=%02X",
             s_requested == CC1101_RX_PACKET ? "packet" : "raw",
             !s_mode_valid ? "unknown/stopped" : s_applied == CC1101_RX_PACKET ? "packet" : "raw",
             actual.pktctrl0, actual.mdmcfg2, actual.iocfg0, actual.marcstate);
    ESP_LOGI(RX_TAG, "raw digital demodulated OOK (not IQ/decoded bits): overflow=%llu observed_drop=%llu sample_drop=%llu paused=%d; paused/ISR-missed edges unknown",
             (unsigned long long)stats.overflow_events, (unsigned long long)stats.dropped_edges,
             (unsigned long long)s_sample_drops, stats.paused);
}

void rfuzz_rx_raw_service(void)
{
    if (!rfuzz_rx_is_raw()) return;
    rfuzz_raw_edge_t edges[64];
    /* At most one full ring per call, even under continuous noise. */
    for (unsigned batch = 0; batch < 4; batch++) {
        size_t count = rfuzz_b_raw_drain(edges, 64);
        for (size_t i = 0; i < count; i++) {
            rfuzz_raw_edge_t edge = edges[i];
            bool continuous = s_have_previous && edge.epoch == s_previous.epoch;
            if (s_raw_sample_count < 8) {
                int used = snprintf(s_raw_samples + s_raw_sample_len,
                                    sizeof(s_raw_samples) - s_raw_sample_len,
                                    " t=%lld L=%u prev=%d dt=%lld;",
                                    (long long)edge.time_us, edge.level,
                                    continuous ? s_previous.level : -1,
                                    continuous ? (long long)(edge.time_us - s_previous.time_us) : -1LL);
                if (used > 0 && (size_t)used < sizeof(s_raw_samples) - s_raw_sample_len) {
                    s_raw_sample_len += used;
                    s_raw_sample_count++;
                } else s_sample_drops++;
            } else s_sample_drops++;
            s_previous = edge;
            s_have_previous = true;
        }
        if (count < 64) break;
    }
    rfuzz_raw_stats_t stats;
    rfuzz_b_raw_stats(&stats);
    if (stats.paused && rfuzz_b_capture_arm() != ESP_OK) {
        s_mode_valid = false;
        ESP_LOGE(RX_TAG, "raw capture rearm failed; stopped");
    }
    int64_t now = esp_timer_get_time();
    if (now - s_raw_report_us >= 1000000) {
        ESP_LOGI(RX_TAG, "RAW t/dt=us since boot/previous edge, L=after, prev=-1/dt=-1=gap; n=%u overflow=%llu observed_drop=%llu sample_drop=%llu paused_losses=unknown:%s",
                 (unsigned)s_raw_sample_count, (unsigned long long)stats.overflow_events,
                 (unsigned long long)stats.dropped_edges, (unsigned long long)s_sample_drops,
                 s_raw_samples);
        s_raw_report_us = esp_timer_get_time();
        s_raw_sample_len = s_raw_sample_count = 0;
        s_raw_samples[0] = '\0';
    }
}

static float rssi_to_dbm(int8_t raw)
{
    return (raw / 2.0f) - 74.0f;
}

esp_err_t rfuzz_rx_init(void)
{
    if (rfuzz_radio_init(&s_rx_radio, "Radio B", PIN_NUM_CS_B, PIN_NUM_GDO0_B) != ESP_OK)
        return ESP_FAIL;

    return rfuzz_rx_select(CC1101_RX_PACKET);
}

esp_err_t rfuzz_rx_wait_window(uint32_t window_ms,
                               const uint8_t *expected,
                               size_t expected_len,
                               rfuzz_rx_report_t *report)
{
    if (!rfuzz_rx_is_packet()) return ESP_ERR_INVALID_STATE;
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
