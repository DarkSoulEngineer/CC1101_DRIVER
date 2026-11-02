#include "tx.h"
#include "radio.h"
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


#define TX_POWER CC1101_DEFAULT_PA_433_MHZ

static cc1101_handle_t s_tx_radio;

esp_err_t rfuzz_tx_init(void)
{
    if (rfuzz_radio_init(&s_tx_radio, "Radio A", PIN_NUM_CS_A, PIN_NUM_GDO0_A) != ESP_OK)
        return ESP_FAIL;

    cc1101_set_tx_power(&s_tx_radio, TX_POWER);

    xSemaphoreTake(tx_done_sem_a, 0);

    return ESP_OK;
}

esp_err_t rfuzz_tx_idle_locked(void)
{
    esp_err_t err = cc1101_idle_locked(&s_tx_radio);
    xSemaphoreTake(tx_done_sem_a, 0);
    return err;
}

esp_err_t rfuzz_tx_apply_profile_locked(cc1101_modulation_t modulation)
{
    return cc1101_apply_profile_locked(&s_tx_radio, modulation, CC1101_RX_PACKET);
}

esp_err_t rfuzz_tx_profile_readback_locked(cc1101_profile_status_t *out)
{
    return cc1101_profile_readback_locked(&s_tx_radio, out);
}
