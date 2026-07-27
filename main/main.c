#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "hw_init.h"
#include "cc1101.h"
#include "sump_capture.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    static cc1101_handle_t radio;

    ESP_LOGI(TAG, "=== RFuzz SUMP Logic Analyzer ===");

    if (init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (cc1101_init(&radio, cc1101_handle,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "PARTNUM=0x%02X  VERSION=0x%02X",
             cc1101_read_status_reg(&radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&radio, CC1101_VERSION));

    cc1101_config_t cfg = CC1101_DEFAULT_CONFIG();
    cfg.isr_enabled = false;
    cfg.radio.gdo0_mode = CC1101_GDO_ASYNC_DATA;
    cfg.radio.gdo2_mode = CONFIG_SUMP_GDO2_MODE;
    cfg.packet.mode = CC1101_PKT_INFINITE_E;
    cfg.packet.crc_enable = false;

    esp_err_t ret = cc1101_configure(&radio, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    cc1101_write_reg(&radio, CC1101_PKTCTRL0,
                     CC1101_PKT_FORMAT_ASYNC | CC1101_PKTLEN_INFINITE);

    cc1101_set_rx_mode(&radio);

    gpio_num_t gdo2_pin = (CONFIG_SUMP_GDO2_MODE != 0x2E) ? PIN_NUM_GDO2 : -1;

    ret = sump_capture_init(PIN_NUM_GDO0, gdo2_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SUMP init failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "SUMP ready — %d channel(s), GDO2 mode=0x%02X",
             (gdo2_pin >= 0) ? 2 : 1, CONFIG_SUMP_GDO2_MODE);
}
