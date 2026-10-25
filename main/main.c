/*
 * RFuzz Dual-Radio Link Validation
 *
 * ├── main.c       application + shared link profile (freq/datarate/radio bring-up)
 * ├── rfuzz_tx.c   TX module - Radio A (fuzzer)    imported via #include below
 * └── rfuzz_rx.c   RX module - Radio B (target)    imported via #include below
 *
 * Both modules compile as a single translation unit: main.c defines the
 * shared link profile and helpers first, then imports each module.
 */
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ";

/* ============================================================
 * SHARED RF LINK PROFILE — single source of truth.
 * Visible to the imported modules below (same translation unit).
 * ============================================================ */
#define RFUZZ_LINK_FREQUENCY_HZ 433920000
#define RFUZZ_LINK_DATARATE     38400
#define RFUZZ_LINK_CHANNEL      0

void rfuzz_log_hex(const char *tag,
                   const char *prefix,
                   const uint8_t *buf,
                   size_t len)
{
    char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(hex) - 3; i++)
        pos += sprintf(&hex[pos], "%02X ", buf[i]);

    ESP_LOGI(tag, "%s [%s]", prefix, hex);
}

esp_err_t rfuzz_radio_init(cc1101_handle_t *dev,
                           const char *name,
                           gpio_num_t cs_pin,
                           gpio_num_t gdo0_pin)
{
    if (cc1101_init(dev, cc1101_handle, cs_pin, PIN_NUM_MISO, gdo0_pin) != ESP_OK)
        return ESP_FAIL;

    ESP_LOGI(TAG, "%s PARTNUM=0x%02X VERSION=0x%02X",
             name,
             cc1101_read_status_reg(dev, CC1101_PARTNUM),
             cc1101_read_status_reg(dev, CC1101_VERSION));

    cc1101_config(dev);
    cc1101_set_frequency(dev, RFUZZ_LINK_FREQUENCY_HZ);
    cc1101_set_channel(dev, RFUZZ_LINK_CHANNEL);
    cc1101_set_datarate(dev, RFUZZ_LINK_DATARATE);

    return ESP_OK;
}

/* ============================================================
 * MODULE IMPORTS
 * ============================================================ */
#include "rfuzz_tx.c"
#include "rfuzz_rx.c"

/* ============================================================
 * APPLICATION
 * ============================================================ */
#define RX_WINDOW_MS     250
#define ITER_INTERVAL_MS 5000
#define MAX_MISSES       5

#define GOLDEN_LEN 4
static const uint8_t GOLDEN_PAYLOAD[GOLDEN_LEN] = {0xDE, 0xAD, 0xBE, 0xEF};

static void hang_on_error(const char *what)
{
    ESP_LOGE(TAG, "%s failed", what);
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

void app_main(void)
{
    uint32_t iter = 0;
    uint32_t total_ok = 0;
    uint32_t total_corrupt = 0;
    uint32_t total_crcfail = 0;
    uint32_t misses = 0;
    int64_t rx_min_us = INT64_MAX;
    int64_t rx_max_us = 0;
    int64_t rx_sum_us = 0;

    ESP_LOGI(TAG, "Starting RFuzz Dual-Radio Link Validation...");

    if (init_hardware() != ESP_OK)
        hang_on_error("hardware init");
    if (rfuzz_tx_init() != ESP_OK)
        hang_on_error("TX module init");
    if (rfuzz_rx_init() != ESP_OK)
        hang_on_error("RX module init");

    while (1)
    {
        rfuzz_tx_result_t txr;
        rfuzz_rx_report_t rxr;

        iter++;

        if (rfuzz_tx_send(GOLDEN_PAYLOAD, GOLDEN_LEN, &txr) != ESP_OK)
            continue;

        rfuzz_rx_wait_window(RX_WINDOW_MS, GOLDEN_PAYLOAD, GOLDEN_LEN, &rxr);

        switch (rxr.result)
        {
        case RFUZZ_RX_VALID:
            if (rxr.latency_us < rx_min_us) rx_min_us = rxr.latency_us;
            if (rxr.latency_us > rx_max_us) rx_max_us = rxr.latency_us;
            rx_sum_us += rxr.latency_us;
            total_ok++;

            ESP_LOGI(TAG,
                     "[iter=%lu] LINK OK (ok=%lu corrupt=%lu crc_fail=%lu miss=%lu | "
                     "rx_lat avg=%lldus min=%lldus max=%lldus)",
                     (unsigned long)iter, (unsigned long)total_ok,
                     (unsigned long)total_corrupt, (unsigned long)total_crcfail,
                     (unsigned long)misses,
                     (long long)(rx_sum_us / total_ok),
                     (long long)rx_min_us, (long long)rx_max_us);
            break;

        case RFUZZ_RX_CRC_FAIL:
            total_crcfail++;
            ESP_LOGW(TAG,
                     "[iter=%lu] LINK DEGRADED: frame lost at RF (CRC fail) "
                     "rssi=%.1fdBm lqi=%d crc_fail=%lu",
                     (unsigned long)iter, rxr.rssi_dbm, rxr.lqi,
                     (unsigned long)total_crcfail);
            break;

        default:
            if (rxr.result == RFUZZ_RX_CORRUPT)
                total_corrupt++;

            misses++;
            ESP_LOGE(TAG,
                     "[iter=%lu] SUSPECTED TARGET FAILURE: tx_confirmed=%d "
                     "miss=%lu rssi=%.1fdBm marcstate=0x%02X",
                     (unsigned long)iter, txr.confirmed, (unsigned long)misses,
                     rxr.rssi_dbm, rxr.marcstate);

            if (misses >= MAX_MISSES)
            {
                ESP_LOGE(TAG,
                         "[iter=%lu] CRASH HYPOTHESIS: %lu consecutive misses "
                         "(ok=%lu corrupt=%lu crc_fail=%lu)",
                         (unsigned long)iter, (unsigned long)misses,
                         (unsigned long)total_ok, (unsigned long)total_corrupt,
                         (unsigned long)total_crcfail);
                misses = 0;
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(ITER_INTERVAL_MS));
    }
}