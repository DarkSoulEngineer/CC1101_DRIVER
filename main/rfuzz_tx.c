/*
 * RFuzz TX module - Radio A (fuzzer / transmitter side).
 * Imported by main.c as part of the single translation unit.
 * Shared helpers (rfuzz_log_hex, rfuzz_radio_init, RFUZZ_LINK_*)
 * are defined in main.c BEFORE this file is #included.
 */
#ifndef RFUZZ_TX_C
#define RFUZZ_TX_C

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw_init.h"
#include "cc1101.h"

/* ============================================================
 * MODULE INTERFACE (types + API, visible to main.c via include)
 * ============================================================ */
typedef struct {
    bool confirmed;
    int64_t spi_transfer_us;
    int64_t gdo0_confirm_us;
} rfuzz_tx_result_t;

esp_err_t rfuzz_tx_init(void);
esp_err_t rfuzz_tx_send(const uint8_t *payload, size_t len, rfuzz_tx_result_t *result);

/* ============================================================
 * IMPLEMENTATION (private to module)
 * ============================================================ */
static const char *TX_TAG = "RFUZZ_TX";

/* Match both radios' nominal +7 dBm default; not a guaranteed ERP. */
#define TX_POWER          CC1101_DEFAULT_PA_433_MHZ
#define TX_BUS_LOCK_MS    200
#define TX_GDO0_TIMEOUT_MS 100

static cc1101_handle_t s_tx_radio;

esp_err_t rfuzz_tx_init(void)
{
    if (rfuzz_radio_init(&s_tx_radio, "Radio A", PIN_NUM_CS_A, PIN_NUM_GDO0_A) != ESP_OK)
        return ESP_FAIL;

    cc1101_set_tx_power(&s_tx_radio, TX_POWER);

    xSemaphoreTake(tx_done_sem_a, 0);

    return ESP_OK;
}

esp_err_t rfuzz_tx_send(const uint8_t *payload, size_t len, rfuzz_tx_result_t *result)
{
    int64_t t0, t1;

    memset(result, 0, sizeof(*result));

    if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(TX_BUS_LOCK_MS)) != pdTRUE)
    {
        ESP_LOGE(TX_TAG, "bus lock timeout");
        return ESP_ERR_TIMEOUT;
    }

    t0 = esp_timer_get_time();
    cc1101_transmit(&s_tx_radio, (uint8_t *)payload, len);
    xSemaphoreGive(cc1101_bus_mutex);
    t1 = esp_timer_get_time();

    if (xSemaphoreTake(tx_done_sem_a, pdMS_TO_TICKS(TX_GDO0_TIMEOUT_MS)) == pdTRUE)
    {
        int64_t t2 = esp_timer_get_time();

        result->confirmed = true;
        result->spi_transfer_us = t1 - t0;
        result->gdo0_confirm_us = t2 - t1;

        ESP_LOGI(TX_TAG, "TX OK: payload=%uB spi=%lldus gdo0=%lldus",
                 (unsigned)len,
                 (long long)result->spi_transfer_us,
                 (long long)result->gdo0_confirm_us);
    }
    else
    {
        ESP_LOGW(TX_TAG, "TX NOT CONFIRMED by GDO0 (no interrupt)");
    }

    rfuzz_log_hex(TX_TAG, "TX payload:", payload, len);

    return ESP_OK;
}

#endif /* RFUZZ_TX_C */
