#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ";

/* ============================================================
 * CC1101 Configuration
 * ============================================================ */

static const cc1101_config_t radio_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_POS10dBm,
    .isr_enabled = true,

    .modem = {
        .modulation      = CC1101_MOD_ASK_E,
        .sync_mode       = CC1101_SYNC_16_16_E,
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 1,
        .datarate_bps    = 4800,
        .deviation       = 0x00,
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_VARIABLE_E,
        .crc_enable    = true,
        .whitening     = false,
        .append_status = true,
        .max_length    = 64,
        .addr_check    = CC1101_ADR_CHK_NONE,
        .sync1         = 0x2D,
        .sync0         = 0xD4,
    },

    .radio = {
        .autocal     = CC1101_AUTOCAL_IDLE_TO_RXTX,
        .pin_mode    = 0x3F,
        .pin_output  = true,
        .gdo0_mode   = CC1101_GDO_SYNC_WORD,
        .gdo2_mode   = CC1101_GDO_HIGH_Z,
    },
};

/* ============================================================
 * Application
 * ============================================================ */

void app_main(void)
{
    static cc1101_handle_t radio;

    ESP_LOGI(TAG, "=== RFuzz TX ===");

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

    if (cc1101_configure(&radio, &radio_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    const uint8_t payload[] = "Hello";
    ESP_LOGI(TAG, "Sending: %s (%d bytes)", payload, (int)sizeof(payload) - 1);

    cc1101_transmit(&radio, (uint8_t *)payload, sizeof(payload) - 1);

    if (cc1101_wait_tx_done(&radio, 500)) {
        ESP_LOGI(TAG, "TX done!");
    } else {
        ESP_LOGW(TAG, "TX timeout (no ISR or GDO0 not connected)");
    }

    ESP_LOGI(TAG, "complete");
    vTaskDelete(NULL);
}
