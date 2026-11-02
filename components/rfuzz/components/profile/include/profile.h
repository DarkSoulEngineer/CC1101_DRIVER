#ifndef PROFILE_CONTROL_H
#define PROFILE_CONTROL_H

#include "cc1101.h"

typedef struct {
    cc1101_modulation_t requested_mod, applied_mod;
    cc1101_rx_format_t requested_mode, applied_mode;
    bool valid, pending, apply_both;
    bool cleanup_pending;
} rfuzz_profile_t;

void rfuzz_profile_init(rfuzz_profile_t *state);
void rfuzz_profile_mod(rfuzz_profile_t *state, cc1101_modulation_t modulation);
bool rfuzz_profile_mode(rfuzz_profile_t *state, cc1101_rx_format_t mode);
esp_err_t rfuzz_profile_process(rfuzz_profile_t *state);
void rfuzz_profile_check(rfuzz_profile_t *state);
void rfuzz_profile_status(const rfuzz_profile_t *state);

#endif