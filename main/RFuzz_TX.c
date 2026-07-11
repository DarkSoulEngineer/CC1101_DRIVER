#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ";

void app_main(void)
{
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    cc1101_handle_t radio;

    ESP_LOGI(TAG, "Starting RFuzz Etalon Beacon...");

    if (init_hardware() != ESP_OK)
    {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (cc1101_init(&radio, cc1101_handle, PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK)
    {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    uint8_t partnum = cc1101_read_status_reg(&radio, CC1101_PARTNUM);
    uint8_t version = cc1101_read_status_reg(&radio, CC1101_VERSION);

    ESP_LOGI(TAG, "CC1101 PARTNUM = 0x%02X", partnum);
    ESP_LOGI(TAG, "CC1101 VERSION = 0x%02X", version);

    cc1101_config(&radio);

    cc1101_set_tx_power(&radio, 0x1F); 
    cc1101_set_frequency(&radio, 433920000);
    cc1101_set_channel(&radio, 0);
    cc1101_set_datarate(&radio, 38400); // 38.4k baud
    
    cc1101_dump_registers(&radio);

    xSemaphoreTake(tx_done_sem, 0); 

    while (1)
    {
        ESP_LOGI(TAG, "Transmitting Beacon...");

        cc1101_transmit(&radio, data, sizeof(data));

        if (xSemaphoreTake(tx_done_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "TX Complete (Hardware Confirmed)");
        } else {
            ESP_LOGW(TAG, "TX Timeout! Interrupt failed.");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
