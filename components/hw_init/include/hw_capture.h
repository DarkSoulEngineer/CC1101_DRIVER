#ifndef HW_CAPTURE_H
#define HW_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/rmt_rx.h"

#define RFUZZ_RAW_BLOCK_SYMBOLS 1024

typedef struct {
    uint32_t epoch;       /* New value for each receive transaction. */
    uint32_t sequence;    /* Every accepted callback, including dropped callbacks. */
    int64_t callback_us;  /* Notification time, NOT first-edge time. */
    size_t count;
    bool is_last;
    rmt_symbol_word_t symbols[RFUZZ_RAW_BLOCK_SYMBOLS]; /* 1 us ticks, digital levels. */
} rfuzz_raw_block_t;

typedef struct {
    uint32_t epoch;
    uint32_t sequence;
    bool raw_mode;
    bool active;          /* Raw transaction accepting callbacks, not packet IRQ state. */
    bool paused;          /* Raw selected but no transaction active. */
    bool valid;           /* prepare succeeded; arm may still be needed. */
    esp_err_t backend_error; /* Last failure; cleared by successful prepare. */
    uint64_t copied_blocks;
    uint64_t copied_symbols;
    uint64_t event_drops; /* Callbacks not published, plus READY blocks flushed on stop. */
    uint64_t known_dropped_symbols; /* Reported symbols discarded, not inferred RF loss. */
    uint64_t rearm_count; /* Successful service-triggered receives only. */
    uint64_t gap_count;   /* Final callbacks and explicit stops of active transactions. */
    bool hardware_loss_unknown; /* Always true: no public hardware overflow/loss counter. */
} rfuzz_raw_stats_t;

/* Single task owner, with scheduler running (not suspended). Stop before radio
 * register changes; errors forbid mode reuse. Stop invalidates preparation:
 * call prepare again before arm, including when remaining in the same mode.
 * Stop deletes the RMT channel (ISR barrier), preserving any borrowed block.
 * prepare selects GPIO IRQ vs RMT exclusively but does not start capture.
 * service rearms final receives before the caller formats/drains up to two blocks.
 * No forced abort: continuous activity may produce partial callbacks indefinitely.
 * 1 us filter and 5 ms idle termination do not guarantee timing accuracy/losslessness.
 */
esp_err_t rfuzz_b_capture_stop(void);
esp_err_t rfuzz_b_capture_prepare(bool raw);
esp_err_t rfuzz_b_capture_arm(void);
void rfuzz_b_capture_service(void);
/* At most one borrowed slot; valid until release, including across stop/prepare.
 * Old edge/drain API intentionally removed: no synthesized edge timestamps. */
bool rfuzz_b_raw_acquire(const rfuzz_raw_block_t **out);
void rfuzz_b_raw_release(void);
void rfuzz_b_raw_stats(rfuzz_raw_stats_t *out);

#endif
