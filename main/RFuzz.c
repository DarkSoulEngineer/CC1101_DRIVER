#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "RFUZZ";

void app_main(void)
{
    uint8_t data[1] = {0x00};
    cc1101_handle_t radio;

    ESP_LOGI(TAG, "Starting RFuzz...");

    if (init_hardware() != ESP_OK)
    {
        ESP_LOGE(TAG, "Hardware init failed");

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (cc1101_init(
            &radio,
            cc1101_handle,
            PIN_NUM_CS,
            PIN_NUM_MISO,
            PIN_NUM_GDO0) != ESP_OK)
    {
        ESP_LOGE(TAG, "CC1101 init failed");

        while (1)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    uint8_t partnum = cc1101_read_status_reg(&radio, CC1101_PARTNUM);
    uint8_t version = cc1101_read_status_reg(&radio, CC1101_VERSION);

    ESP_LOGI(TAG, "CC1101 PARTNUM = 0x%02X", partnum);
    ESP_LOGI(TAG, "CC1101 VERSION = 0x%02X", version);

    cc1101_config(&radio);

    cc1101_set_frequency(&radio, 433920000);   // 433.92 MHz
    cc1101_set_channel(&radio, 0);
    cc1101_set_datarate(&radio, 38400);

    cc1101_dump_registers(&radio);

    while (1)
    {
        ESP_LOGI(TAG, "TX Byte: 0x%02X", data[0]);

        cc1101_transmit(&radio, data, sizeof(data));

        data[0]++;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
