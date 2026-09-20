#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw_init.h"
#include "cc1101.h"
#include "rfuzz_tx.h"

static const char *TAG = "RFUZZ_TX";

#define TX_FREQUENCY_HZ   433920000
#define TX_DATARATE       38400
#define TX_CHANNEL        0
#define TX_POWER          0x1F
#define TX_BUS_LOCK_MS    200
#define TX_GDO0_TIMEOUT_MS 100

static cc1101_handle_t s_radio;

static void log_hex(const char *prefix, const uint8_t *buf, size_t len)
{
    char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(hex) - 3; i++)
        pos += sprintf(&hex[pos], "%02X ", buf[i]);

    ESP_LOGI(TAG, "%s [%s]", prefix, hex);
}

esp_err_t rfuzz_tx_init(void)
{
    if (cc1101_init(&s_radio, cc1101_handle, PIN_NUM_CS_A, PIN_NUM_MISO, PIN_NUM_GDO0_A) != ESP_OK)
        return ESP_FAIL;

    ESP_LOGI(TAG, "Radio A PARTNUM=0x%02X VERSION=0x%02X",
             cc1101_read_status_reg(&s_radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&s_radio, CC1101_VERSION));

    cc1101_config(&s_radio);
    cc1101_set_frequency(&s_radio, TX_FREQUENCY_HZ);
    cc1101_set_channel(&s_radio, TX_CHANNEL);
    cc1101_set_datarate(&s_radio, TX_DATARATE);
    cc1101_set_tx_power(&s_radio, TX_POWER);

    xSemaphoreTake(tx_done_sem_a, 0);

    return ESP_OK;
}

esp_err_t rfuzz_tx_send(const uint8_t *payload, size_t len, rfuzz_tx_result_t *result)
{
    int64_t t0, t1;

    memset(result, 0, sizeof(*result));

    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(TX_BUS_LOCK_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "bus lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    t0 = esp_timer_get_time();
    cc1101_transmit(&s_radio, (uint8_t *)payload, len);
    xSemaphoreGive(cc1101_bus_mutex);
    t1 = esp_timer_get_time();

    if (xSemaphoreTake(tx_done_sem_a, pdMS_TO_TICKS(TX_GDO0_TIMEOUT_MS)) == pdTRUE)
    {
        int64_t t2 = esp_timer_get_time();

        result->confirmed = true;
        result->spi_transfer_us = t1 - t0;
        result->gdo0_confirm_us = t2 - t1;

        ESP_LOGI(TAG, "TX OK: payload=%uB spi=%lldus gdo0=%lldus",
                 (unsigned)len,
                 (long long)result->spi_transfer_us,
                 (long long)result->gdo0_confirm_us);
    }
    else
    {
        ESP_LOGW(TAG, "TX NOT CONFIRMED by GDO0 (no interrupt)");
    }

    log_hex("TX payload:", payload, len);

    return ESP_OK;
}