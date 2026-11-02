#include "hw_init.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "sdkconfig.h"
#include <string.h>

enum slot_state { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_BORROWED };
typedef struct {
    rfuzz_raw_block_t block;
    enum slot_state state;
    uint64_t order;
} capture_slot_t;

/* Firmware-lifetime storage. Neither stop nor a mode switch invalidates a loan. */
static DRAM_ATTR capture_slot_t s_slots[2];
static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR rfuzz_raw_stats_t s_stats = {.hardware_loss_unknown = true};
static rmt_symbol_word_t *s_dma;
static rmt_channel_handle_t s_channel;
static uint32_t s_generation;
typedef struct { uint32_t generation; } callback_context_t;
/* Rotate firmware-lifetime contexts; reuse only after the deletion barrier. */
static DRAM_ATTR callback_context_t s_contexts[2];
static uint64_t s_order;
static int s_borrowed = -1;
static bool s_enabled, s_handler, s_finished, s_accepting, s_quarantined;

static esp_err_t capture_error(esp_err_t err)
{
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_stats.backend_error = err;
        s_stats.valid = false;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

static void IRAM_ATTR packet_isr(void *arg)
{
    (void)arg;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(tx_done_sem_b, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static bool IRAM_ATTR capture_done(rmt_channel_handle_t channel,
                                  const rmt_rx_done_event_data_t *edata, void *arg)
{
    int slot = -1;
    uint32_t epoch, sequence;
    uint32_t generation = ((callback_context_t *)arg)->generation;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_lock);
    if (!s_accepting || channel != s_channel || generation != s_generation) {
        portEXIT_CRITICAL_ISR(&s_lock);
        return false;
    }
    epoch = s_stats.epoch;
    sequence = ++s_stats.sequence;
    for (int i = 0; i < 2; ++i) {
        if (s_slots[i].state == SLOT_FREE) {
            slot = i;
            s_slots[i].state = SLOT_FILLING;
            break;
        }
    }
    portEXIT_CRITICAL_ISR(&s_lock);

    /* IDF reports a transient subrange of the DMA buffer. Bound both the source
     * span and destination, including malformed/zero-length notifications. */
    size_t count = edata->num_symbols;
    uintptr_t begin = (uintptr_t)s_dma;
    uintptr_t ptr = (uintptr_t)edata->received_symbols;
    size_t available = 0;
    if (ptr >= begin && ptr - begin < RFUZZ_RAW_BLOCK_SYMBOLS * sizeof(*s_dma) &&
        (ptr - begin) % sizeof(*s_dma) == 0) {
        available = RFUZZ_RAW_BLOCK_SYMBOLS - (ptr - begin) / sizeof(*s_dma);
    }
    if (count > available) count = available;
    if (count > RFUZZ_RAW_BLOCK_SYMBOLS) count = RFUZZ_RAW_BLOCK_SYMBOLS;
    if (slot >= 0) {
        rfuzz_raw_block_t *block = &s_slots[slot].block;
        block->epoch = epoch;
        block->sequence = sequence;
        block->callback_us = now;
        block->count = count;
        block->is_last = edata->flags.is_last != 0;
        if (count) memcpy(block->symbols, edata->received_symbols, count * sizeof(*s_dma));
    }

    portENTER_CRITICAL_ISR(&s_lock);
    /* Stop may close publication during the bounded copy. Slot reuse waits for
     * channel deletion, NOT merely an active-callback counter reaching zero. */
    if (s_accepting && generation == s_generation && epoch == s_stats.epoch) {
        if (slot >= 0) {
            s_slots[slot].order = ++s_order;
            s_slots[slot].state = SLOT_READY;
            s_stats.copied_blocks++;
            s_stats.copied_symbols += count;
            if (count != edata->num_symbols) s_stats.event_drops++;
            s_stats.known_dropped_symbols += edata->num_symbols - count;
        } else {
            s_stats.event_drops++;
            s_stats.known_dropped_symbols += edata->num_symbols;
        }
        if (edata->flags.is_last) {
            s_accepting = false;
            s_stats.active = false;
            s_stats.paused = true;
            s_stats.gap_count++;
            s_finished = true;
        }
    } else {
        s_stats.event_drops++;
        s_stats.known_dropped_symbols += edata->num_symbols;
        if (slot >= 0) s_slots[slot].state = SLOT_FREE;
    }
    portEXIT_CRITICAL_ISR(&s_lock);
    return false;
}

esp_err_t rfuzz_b_capture_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_accepting = false;
    if (s_stats.active) s_stats.gap_count++;
    s_stats.active = false;
    s_stats.paused = s_stats.raw_mode;
    s_stats.valid = false;
    s_finished = false;
    portEXIT_CRITICAL(&s_lock);

    esp_err_t gpio_err = gpio_intr_disable(PIN_NUM_GDO0_B);
    if (s_quarantined) return capture_error(ESP_ERR_INVALID_STATE);
    if (s_channel) {
        bool disabled_now = false;
        if (s_enabled) {
            esp_err_t err = rmt_disable(s_channel);
            if (err != ESP_OK) return capture_error(err);
            s_enabled = false;
            disabled_now = true;
        }
        /* IDF 6.1: rmt_rx_destroy -> gdma_del_rx_channel -> esp_intr_free.
         * esp_intr_free executes on the allocating CPU via blocking IPC when
         * necessary, disables/removes the handler, then GDMA clears pending
         * events. Only after this barrier may channel generation/slots change.
         * rmt_disable alone only stops/disconnects DMA and is NOT the barrier. */
        esp_err_t err = rmt_del_channel(s_channel);
#if ESP_IDF_VERSION == ESP_IDF_VERSION_VAL(6, 1, 0) && CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_PM_ENABLE
        if (err == ESP_ERR_INVALID_STATE && disabled_now) {
            /* Pinned IDF 6.1/S3, PM OFF only:
             * rmt_del_rx_channel:315-316 is the ONLY INVALID_STATE return in
             * this deletion path, BEFORE any destruction. gdma_del_rx_channel
             * returns OK, gdma_del_link_list returns OK/INVALID_ARG, S3 clock
             * release returns OK, esp_intr_free returns OK/INVALID_ARG/FAIL.
             * Thus this exact error leaves the handle intact. Do NOT extend
             * this recovery to PM-on: pm_lock_delete can return INVALID_STATE
             * after destruction has begun, and a second disable releases the
             * PM lock twice.
             *
             * A terminal DMA EOF can store ENABLE after disable stores INIT
             * (rmt_rx.c:821-838). GDMA clears its latched event before invoking
             * that handler (gdma.c:988-997). Observing this precondition error
             * synchronizes with the terminal ENABLE store: this single RX job
             * has no second terminal EOF, and its handler has no later FSM
             * writes. Restore INIT once, then deletion's interrupt-free barrier
             * drains any still-running application callback before reuse.
             * No receive/enable occurs here; DMA remains stopped. The repeated
             * gdma_disconnect may report already-disconnected inside IDF, but
             * rmt_disable ignores that result and still restores INIT.
             */
            err = rmt_disable(s_channel);
            if (err == ESP_OK) err = rmt_del_channel(s_channel);
        }
#else
        (void)disabled_now; /* Unreviewed SDK/configuration: fail closed, no retry. */
#endif
        if (err != ESP_OK) {
            /* Deletion can fail after partial destruction: never retry a
             * potentially freed handle or silently restore GPIO ownership. */
            s_quarantined = true;
            return capture_error(err);
        }
        s_channel = NULL;
    }
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < 2; ++i) {
        if (s_slots[i].state == SLOT_READY) {
            s_stats.event_drops++;
            s_stats.known_dropped_symbols += s_slots[i].block.count;
            s_slots[i].state = SLOT_FREE;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return capture_error(gpio_err);
}

esp_err_t rfuzz_b_capture_prepare(bool raw)
{
    esp_err_t err = rfuzz_b_capture_stop();
    if (err != ESP_OK) return err;
    if (s_handler) {
        err = gpio_isr_handler_remove(PIN_NUM_GDO0_B);
        if (err != ESP_OK) return capture_error(err);
        s_handler = false;
    }
    portENTER_CRITICAL(&s_lock);
    s_stats.raw_mode = raw;
    s_stats.paused = raw;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreTake(tx_done_sem_b, 0);
    err = gpio_set_intr_type(PIN_NUM_GDO0_B, raw ? GPIO_INTR_DISABLE : GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) return capture_error(err);
    if (raw) {
        if (!s_dma) {
            s_dma = heap_caps_aligned_alloc(64, RFUZZ_RAW_BLOCK_SYMBOLS * sizeof(*s_dma),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            if (!s_dma) return capture_error(ESP_ERR_NO_MEM);
        }
        rmt_rx_channel_config_t config = {
            .gpio_num = PIN_NUM_GDO0_B,
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 1000000,
            .mem_block_symbols = RFUZZ_RAW_BLOCK_SYMBOLS,
            .flags.with_dma = true,
            .flags.allow_pd = false,
        };
        err = rmt_new_rx_channel(&config, &s_channel);
        if (err != ESP_OK) return capture_error(err);
        ++s_generation;
        callback_context_t *context = &s_contexts[s_generation % 2];
        context->generation = s_generation;
        rmt_rx_event_callbacks_t callbacks = {.on_recv_done = capture_done};
        err = rmt_rx_register_event_callbacks(s_channel, &callbacks,
                                             context);
        if (err != ESP_OK) return capture_error(err);
    }
    portENTER_CRITICAL(&s_lock);
    s_stats.valid = true;
    s_stats.backend_error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

static esp_err_t start_receive(bool rearm)
{
    const rmt_receive_config_t config = {
        .signal_range_min_ns = 1000,
        .signal_range_max_ns = 5000000,
        .flags.en_partial_rx = true,
    };
    portENTER_CRITICAL(&s_lock);
    ++s_stats.epoch;
    s_stats.active = true;
    s_stats.paused = false;
    s_accepting = true;
    s_finished = false;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = rmt_receive(s_channel, s_dma,
                               RFUZZ_RAW_BLOCK_SYMBOLS * sizeof(*s_dma), &config);
    portENTER_CRITICAL(&s_lock);
    if (err != ESP_OK) {
        s_accepting = false;
        s_stats.active = false;
        s_stats.paused = true;
    } else if (rearm) {
        s_stats.rearm_count++;
    }
    portEXIT_CRITICAL(&s_lock);
    return capture_error(err);
}

esp_err_t rfuzz_b_capture_arm(void)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = s_stats.valid;
    bool raw = s_stats.raw_mode;
    bool active = s_stats.active;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) return capture_error(ESP_ERR_INVALID_STATE);
    if (!raw) {
        esp_err_t err;
        if (!s_handler) {
            err = gpio_isr_handler_add(PIN_NUM_GDO0_B, packet_isr, NULL);
            if (err != ESP_OK) return capture_error(err);
            s_handler = true;
        }
        return capture_error(gpio_intr_enable(PIN_NUM_GDO0_B));
    }
    if (active) return ESP_OK;
    if (!s_enabled) {
        esp_err_t err = rmt_enable(s_channel);
        if (err != ESP_OK) return capture_error(err);
        s_enabled = true;
    }
    return start_receive(false);
}

void rfuzz_b_capture_service(void)
{
    portENTER_CRITICAL(&s_lock);
    bool rearm = s_stats.valid && s_stats.raw_mode && s_finished;
    portEXIT_CRITICAL(&s_lock);
    if (rearm) (void)start_receive(true);
}

bool rfuzz_b_raw_acquire(const rfuzz_raw_block_t **out)
{
    if (!out) return false;
    *out = NULL;
    portENTER_CRITICAL(&s_lock);
    int slot = -1;
    if (s_borrowed < 0) {
        for (int i = 0; i < 2; ++i) {
            if (s_slots[i].state == SLOT_READY &&
                (slot < 0 || s_slots[i].order < s_slots[slot].order)) slot = i;
        }
    }
    if (slot >= 0) {
        s_slots[slot].state = SLOT_BORROWED;
        s_borrowed = slot;
        *out = &s_slots[slot].block;
    }
    portEXIT_CRITICAL(&s_lock);
    return slot >= 0;
}

void rfuzz_b_raw_release(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_borrowed >= 0) {
        s_slots[s_borrowed].state = SLOT_FREE;
        s_borrowed = -1;
    }
    portEXIT_CRITICAL(&s_lock);
}

void rfuzz_b_raw_stats(rfuzz_raw_stats_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}
