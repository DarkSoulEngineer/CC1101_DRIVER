#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ_RX";

/* ============================================================
 * RX Configuration — must match TX exactly for reception.
 *
 *   Same modulation, datarate, sync word, deviation.
 *   append_status = true (RSSI + CRC bytes in FIFO).
 * ============================================================ */

static const cc1101_config_t rx_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_0dBm,
    .isr_enabled = false,

    .modem = {
        .modulation      = CC1101_MOD_2FSK_E,
        .sync_mode       = CC1101_SYNC_16_16_E,
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 4,
        .datarate_bps    = 2400,
        .deviation       = 0x47,
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_VARIABLE_E,
        .crc_enable    = false,
        .whitening     = false,
        .append_status = true,
        .max_length    = 64,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0x2D,
        .sync0         = 0xD4,
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_ALWAYS,
        .pin_mode    = 0x00,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_HIGH_Z,
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};

void app_main(void)
{
    uint8_t data[CC1101_MAX_PACKET_LEN] = {0};
    static cc1101_handle_t radio;

    ESP_LOGI(TAG, "=== RFuzz RX (2FSK 2400 bps, dev 12 kHz) ===");

    if (init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    hw_init_gdo0_input();

    if (cc1101_init(&radio, cc1101_handle,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "CC1101 PARTNUM = 0x%02X  VERSION = 0x%02X",
             cc1101_read_status_reg(&radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&radio, CC1101_VERSION));

    if (cc1101_configure(&radio, &rx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "freq=433.92MHz  modulation=2FSK  rate=2400bps  dev=12kHz");
    ESP_LOGI(TAG, "preamble=4B  sync=2DD4  CRC=OFF  append_status=ON");

    cc1101_set_rx_mode(&radio);
    ESP_LOGI(TAG, "Entering RX mode, waiting for packets...");

    while (1) {
        uint8_t marcstate = cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F;
        if (marcstate != 0x0D) { /* 0x0D = RX */
            ESP_LOGW(TAG, "Not in RX (MARCSTATE=0x%02X), re-entering RX", marcstate);
            cc1101_set_rx_mode(&radio);
        }

        size_t len = CC1101_MAX_PACKET_LEN;

        if (cc1101_receive_packet(&radio, data, &len)) {
            ESP_LOGI(TAG, "RX OK (%zu bytes):", len);
            /* Print payload in hex, grouped like the TX format:
             *   [SRC DST TYPE SEQ_H SEQ_L DATA0 DATA1 DATA2 DATA3] */
            if (len >= 9) {
                ESP_LOGI(TAG, "  SRC=0x%02X DST=0x%02X TYPE=0x%02X SEQ=0x%02X%02X DATA=%02X%02X%02X%02X",
                         data[0], data[1], data[2],
                         data[3], data[4],
                         data[5], data[6], data[7], data[8]);
            } else {
                /* Print raw hex */
                char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
                for (size_t i = 0; i < len; i++)
                    sprintf(hex + i * 3, "%02X ", data[i]);
                ESP_LOGI(TAG, "  %s", hex);
            }
            cc1101_set_rx_mode(&radio);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
