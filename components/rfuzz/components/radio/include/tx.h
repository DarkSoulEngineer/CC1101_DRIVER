#ifndef TX_H
#define TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cc1101.h"

esp_err_t rfuzz_tx_init(void);

esp_err_t rfuzz_tx_apply_profile_locked(cc1101_modulation_t modulation);
esp_err_t rfuzz_tx_profile_readback_locked(cc1101_profile_status_t *out);
esp_err_t rfuzz_tx_idle_locked(void);
esp_err_t rfuzz_tx_send_once(void);

#endif
