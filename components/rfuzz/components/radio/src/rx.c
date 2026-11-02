#include "rx.h"
#include "radio.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hw_init.h"
#include "hw_capture.h"

static const char *RX_TAG = "RFUZZ_RX";
static cc1101_handle_t s_rx_radio;
static cc1101_modulation_t s_modulation = CC1101_PROFILE_OOK;
static cc1101_rx_format_t s_format = CC1101_RX_PACKET;
static bool s_mode_valid;
static int64_t s_raw_report_us;
static uint64_t s_processed_blocks, s_processed_symbols, s_processed_pulses;
static uint64_t s_output_suppressed_blocks, s_output_suppressed_pulses;

bool rfuzz_rx_is_raw(void) { return s_mode_valid && s_format == CC1101_RX_RAW; }
bool rfuzz_rx_is_packet(void) { return s_mode_valid && s_format == CC1101_RX_PACKET; }

esp_err_t rfuzz_rx_capture_stop(void)
{
    s_mode_valid = false;
    return rfuzz_b_capture_stop();
}

esp_err_t rfuzz_rx_idle_locked(void)
{
    s_mode_valid = false;
    return cc1101_idle_locked(&s_rx_radio);
}

esp_err_t rfuzz_rx_apply_profile_locked(cc1101_modulation_t modulation,
                                        cc1101_rx_format_t format)
{
    s_mode_valid = false;
    esp_err_t err = cc1101_apply_profile_locked(&s_rx_radio, modulation, format);
    if (err == ESP_OK) {
        s_modulation = modulation;
        s_format = format;
    }
    return err;
}

esp_err_t rfuzz_rx_start_locked(void)
{
    s_mode_valid = false;
    esp_err_t err = rfuzz_b_capture_prepare(s_format == CC1101_RX_RAW);
    if (err == ESP_OK) err = cc1101_start_rx_locked(&s_rx_radio);
    if (err == ESP_OK) err = rfuzz_b_capture_arm();
    if (err == ESP_OK) s_mode_valid = true;
    else {
        rfuzz_b_capture_stop();
        cc1101_idle_locked(&s_rx_radio);
    }
    return err;
}

esp_err_t rfuzz_rx_profile_readback_locked(cc1101_profile_status_t *out)
{
    return cc1101_profile_readback_locked(&s_rx_radio, out);
}


void rfuzz_rx_status(void)
{
    rfuzz_raw_stats_t stats;
    rfuzz_b_raw_stats(&stats);
    ESP_LOGI(RX_TAG, "capture epoch=%lu sequence=%lu raw=%d active=%d paused=%d valid=%d backend=%s copied_blocks=%llu copied_symbols=%llu event_drops=%llu known_dropped_symbols=%llu rearm=%llu gaps=%llu hardware_loss_unknown=%d",
             (unsigned long)stats.epoch, (unsigned long)stats.sequence,
             stats.raw_mode, stats.active, stats.paused, stats.valid,
             esp_err_to_name(stats.backend_error),
             (unsigned long long)stats.copied_blocks, (unsigned long long)stats.copied_symbols,
             (unsigned long long)stats.event_drops, (unsigned long long)stats.known_dropped_symbols,
             (unsigned long long)stats.rearm_count, (unsigned long long)stats.gap_count,
             stats.hardware_loss_unknown);
    ESP_LOGI(RX_TAG, "processed_blocks=%llu symbols=%llu pulses=%llu output_suppressed_blocks=%llu output_suppressed_pulses=%llu (output omission, NOT capture loss; cumulative)",
             (unsigned long long)s_processed_blocks, (unsigned long long)s_processed_symbols,
             (unsigned long long)s_processed_pulses, (unsigned long long)s_output_suppressed_blocks,
             (unsigned long long)s_output_suppressed_pulses);
}

void rfuzz_rx_raw_service(void)
{
    /* Rearm completed receives before any console formatting or block draining. */
    rfuzz_b_capture_service();
    if (!rfuzz_rx_is_raw()) return;
    rfuzz_raw_stats_t stats;
    rfuzz_b_raw_stats(&stats);
    if (!stats.valid || stats.backend_error != ESP_OK) {
        rfuzz_rx_capture_stop();
        ESP_LOGE(RX_TAG, "capture backend failed; profile recovery required");
        return;
    }
    for (unsigned batch = 0; batch < 2; batch++) {
        const rfuzz_raw_block_t *block;
        if (!rfuzz_b_raw_acquire(&block)) break;
        int64_t now = esp_timer_get_time();
        bool emit = now - s_raw_report_us >= 1000000;
        char preview[256] = {0};
        size_t used = 0, shown = 0, pulses = 0;
        /* No block copies: the backend owns ~12 KiB of static capture buffers. */
        for (size_t i = 0; i < block->count; i++) {
            const rmt_symbol_word_t *symbol = &block->symbols[i];
            unsigned duration[2] = {symbol->duration0, symbol->duration1};
            unsigned level[2] = {symbol->level0, symbol->level1};
            for (unsigned half = 0; half < 2; half++) {
                if (!duration[half]) continue;
                pulses++;
                if (emit && shown < 16) {
                    int n = snprintf(preview + used, sizeof(preview) - used,
                                     " %u:%uus", level[half], duration[half]);
                    if (n > 0 && (size_t)n < sizeof(preview) - used) {
                        used += (size_t)n;
                        shown++;
                    }
                }
            }
        }
        s_processed_blocks++;
        s_processed_symbols += block->count;
        s_processed_pulses += pulses;
        s_output_suppressed_pulses += pulses - shown;
        if (emit) {
            ESP_LOGI(RX_TAG, "RAW epoch=%lu seq=%lu is_last=%d callback_notification_us=%lld symbols=%u pulses=%u preview(L:duration):%s; partial framing is NOT a packet boundary; gaps/loss unknown, no IQ/decoded bits",
                     (unsigned long)block->epoch, (unsigned long)block->sequence,
                     block->is_last, (long long)block->callback_us,
                     (unsigned)block->count, (unsigned)pulses, preview);
            s_raw_report_us = now;
        } else s_output_suppressed_blocks++;
        rfuzz_b_raw_release();
    }
}

esp_err_t rfuzz_rx_init(void)
{
    s_mode_valid = false;
    return rfuzz_radio_init(&s_rx_radio, "Radio B", PIN_NUM_CS_B, PIN_NUM_GDO0_B);
}

static bool rfuzz_rx_poll(rfuzz_rx_report_t *report)
{
    if (!rfuzz_rx_is_packet()) return false;
    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    size_t len = sizeof(report->payload);
    bool crc_ok = false;
    bool got = cc1101_poll_packet(&s_rx_radio, report->payload, &len, &crc_ok);
    if (got) {
        report->len = len;
        report->result = crc_ok ? RFUZZ_RX_VALID : RFUZZ_RX_CRC_FAIL;
        report->rssi_dbm = ((int8_t)cc1101_read_status_reg(&s_rx_radio, CC1101_RSSI) / 2.0f) - 74;
        report->lqi = (int8_t)cc1101_read_status_reg(&s_rx_radio, CC1101_LQI);
    }
    xSemaphoreGive(cc1101_bus_mutex);
    return got;
}
void rfuzz_rx_listen_poll(void)
{
    rfuzz_rx_report_t report = {0};
    if (!rfuzz_rx_poll(&report)) return;
    ESP_LOGI(RX_TAG, "RX EXTERNAL len=%u crc=%s (no golden comparison)",
             (unsigned)report.len, report.result == RFUZZ_RX_VALID ? "OK" : "FAIL");
    rfuzz_log_hex(RX_TAG, "RX external payload:", report.payload, report.len);
}
