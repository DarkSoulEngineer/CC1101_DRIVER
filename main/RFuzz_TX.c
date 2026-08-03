#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ";

static const cc1101_config_t beacon_cfg = {
    .freq_hz   = 433920000,
    .channel   = 0,
    .pa_value  = CC1101_PA_0dBm,
    .isr_enabled = true,

    .modem = {
        .modulation      = CC1101_MOD_GFSK_E,
        .sync_mode       = CC1101_SYNC_30_32_E,
        .dc_filter_off   = false,
        .manchester      = false,
        .fec_enable      = false,
        .preamble_bytes  = 2,
        .datarate_bps    = 38400,
        .deviation       = 0x03,
        .chanbw          = 0x03,
        .channel_spacing = 248,
    },

    .packet = {
        .mode          = CC1101_PKT_VARIABLE_E,
        .crc_enable    = true,
        .whitening     = false,
        .append_status = true,
        .max_length    = 255,
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

void app_main(void)
{
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    cc1101_handle_t radio;

    ESP_LOGI(TAG, "Starting RFuzz Etalon Beacon...");

    if (init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    hw_init_gdo0_input();

    if (cc1101_init(&radio, hw_cc1101_spi,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "CC1101 PARTNUM = 0x%02X  VERSION = 0x%02X",
             cc1101_read_status_reg(&radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&radio, CC1101_VERSION));

    if (cc1101_configure(&radio, &beacon_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    cc1101_dump_registers(&radio);

    while (1) {
        ESP_LOGI(TAG, "Transmitting Beacon...");

        cc1101_transmit(&radio, data, sizeof(data));

        if (cc1101_wait_tx_done(&radio, 500)) {
            ESP_LOGI(TAG, "TX Complete (Hardware Confirmed)");
        } else {
            ESP_LOGW(TAG, "TX Timeout! Interrupt failed.");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
