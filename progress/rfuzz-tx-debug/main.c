#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ_TX";

/* ============================================================
 * TX Configuration — ASK/OOK beacon, easy URH decode.
 *
 *   On-air layout:
 *     [AA AA AA AA]  — preamble (4 bytes)
 *     [DE AF]        — sync word (16-bit)
 *
 *   Radio: 433.92 MHz, 2FSK, 2400 bps, FIXED_LENGTH=20
 * ============================================================ */

static const cc1101_config_t tx_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_POS10dBm,
    .isr_enabled = false,

    .modem = {
        .modulation      = CC1101_MOD_2FSK_E,
        .sync_mode       = CC1101_SYNC_16_16_E,
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 4,
        .datarate_bps    = 2400,
        .deviation       = 0x47,    /* 2FSK: ~24 kHz deviation */
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_FIXED_E,
        .crc_enable    = false,
        .whitening     = false,
        .append_status = false,
        .max_length    = 1,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0xDE,
        .sync0         = 0xAF,
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_ALWAYS,
        .pin_mode    = 0x00,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_SYNC_WORD,  /* 0x06 */
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};

void app_main(void)
{
    static cc1101_handle_t radio;

    ESP_LOGI(TAG, "=== RFuzz TX (2FSK 2400bps) ===");

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

    ESP_LOGI(TAG, "PARTNUM=0x%02X  VERSION=0x%02X",
             cc1101_read_status_reg(&radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&radio, CC1101_VERSION));

    if (cc1101_configure(&radio, &tx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    cc1101_verify_config(&radio, &tx_cfg);

    ESP_LOGI(TAG, "2FSK 2400bps dev=24kHz sync=DEAF CRC=OFF");

    while (1) {
        ESP_LOGI(TAG, "Pre-TX: MARCSTATE=0x%02X TXBYTES=0x%02X GDO0=%d",
                 cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F,
                 cc1101_read_status_reg(&radio, CC1101_TXBYTES) & 0x7F,
                 gpio_get_level(PIN_NUM_GDO0));

        uint8_t pkt = 0x01;
        cc1101_transmit(&radio, &pkt, 1);

        ESP_LOGI(TAG, "Post-TX strobe: MARCSTATE=0x%02X TXBYTES=0x%02X",
                 cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F,
                 cc1101_read_status_reg(&radio, CC1101_TXBYTES) & 0x7F);

        cc1101_wait_tx_done(&radio, 500);

        ESP_LOGI(TAG, "Post-TX done: MARCSTATE=0x%02X",
                 cc1101_read_status_reg(&radio, CC1101_MARCSTATE) & 0x1F);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
