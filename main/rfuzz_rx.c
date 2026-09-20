#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw_init.h"
#include "cc1101.h"
#include "rfuzz_rx.h"

static const char *TAG = "RFUZZ_RX";

#define RX_FREQUENCY_HZ 433920000
#define RX_DATARATE     38400
#define RX_CHANNEL      0
#define RX_POLL_MS      10
#define RX_BUS_LOCK_MS  50

static cc1101_handle_t s_radio;

static void log_hex(const char *prefix, const uint8_t *buf, size_t len)
{
    char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(hex) - 3; i++)
        pos += sprintf(&hex[pos], "%02X ", buf[i]);

    ESP_LOGI(TAG, "%s [%s]", prefix, hex);
}

static float rssi_to_dbm(int8_t raw)
{
    return (raw / 2.0f) - 74.0f;
}

esp_err_t rfuzz_rx_init(void)
{
    if (cc1101_init(&s_radio, cc1101_handle, PIN_NUM_CS_B, PIN_NUM_MISO, PIN_NUM_GDO0_B) != ESP_OK)
        return ESP_FAIL;

    ESP_LOGI(TAG, "Radio B PARTNUM=0x%02X VERSION=0x%02X",
             cc1101_read_status_reg(&s_radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&s_radio, CC1101_VERSION));

    cc1101_config(&s_radio);
    cc1101_set_frequency(&s_radio, RX_FREQUENCY_HZ);
    cc1101_set_channel(&s_radio, RX_CHANNEL);
    cc1101_set_datarate(&s_radio, RX_DATARATE);
    cc1101_set_rx_mode(&s_radio);

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

    ESP_LOGI(TAG, "RX window open (%u ms), waiting for %u-byte reference packet...",
             (unsigned)window_ms, (unsigned)expected_len);

    while ((esp_timer_get_time() - window_start) < (int64_t)window_ms * 1000)
    {
        size_t len = CC1101_MAX_PACKET_LEN;

        if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(RX_BUS_LOCK_MS)) != pdTRUE)
            break;

        bool crc_ok = false;
        bool got = cc1101_poll_packet(&s_radio, report->payload, &len, &crc_ok);
        xSemaphoreGive(cc1101_bus_mutex);

        if (!got)
        {
            vTaskDelay(pdMS_TO_TICKS(RX_POLL_MS));
            continue;
        }

        report->len = len;
        report->latency_us = esp_timer_get_time() - window_start;
        report->rssi_dbm = rssi_to_dbm((int8_t)cc1101_read_status_reg(&s_radio, CC1101_RSSI));
        report->lqi = (int8_t)cc1101_read_status_reg(&s_radio, CC1101_LQI);

        if (!crc_ok)
        {
            report->result = RFUZZ_RX_CRC_FAIL;
            ESP_LOGW(TAG, "RX CRC-FAIL: frame dropped at RF (len=%u) rssi=%.1fdBm lqi=%d",
                     (unsigned)len, report->rssi_dbm, report->lqi);
            log_hex("CRC-FAIL payload:", report->payload, len);
            return ESP_OK;
        }

        if (len == expected_len && memcmp(report->payload, expected, expected_len) == 0)
        {
            report->result = RFUZZ_RX_VALID;
            ESP_LOGI(TAG, "RX HIT after %lldus: len=%u crc=OK rssi=%.1fdBm lqi=%d",
                     (long long)report->latency_us, (unsigned)len,
                     report->rssi_dbm, report->lqi);
            log_hex("RX payload: ", report->payload, len);
        }
        else
        {
            report->result = RFUZZ_RX_CORRUPT;
            ESP_LOGW(TAG, "RX CORRUPT packet (len=%u) rssi=%.1fdBm lqi=%d",
                     (unsigned)len, report->rssi_dbm, report->lqi);
            log_hex("CORRUPT payload:", report->payload, len);
        }
        return ESP_OK;
    }

    report->window_us = esp_timer_get_time() - window_start;
    report->rssi_dbm = rssi_to_dbm((int8_t)cc1101_read_status_reg(&s_radio, CC1101_RSSI));
    report->marcstate = cc1101_read_status_reg(&s_radio, CC1101_MARCSTATE);
    report->rxbytes = cc1101_read_status_reg(&s_radio, CC1101_RXBYTES);
    cc1101_set_rx_mode(&s_radio);

    ESP_LOGW(TAG, "RX window closed: no reference packet "
                  "(window=%lldus rssi=%.1fdBm marcstate=0x%02X rxbytes=0x%02X)",
             (long long)report->window_us, report->rssi_dbm,
             report->marcstate, report->rxbytes);

    return ESP_OK;
}