#ifndef HW_INIT_H
#define HW_INIT_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/spi_master.h" // SPI
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

// Shared SPI bus (both radios on SPI3_HOST)
#define PIN_NUM_MISO   13
#define PIN_NUM_MOSI   11
#define PIN_NUM_CLK    12

// Radio A - transmitter / fuzzer
#define PIN_NUM_CS_A   9
#define PIN_NUM_GDO0_A 4

// Radio B - receiver / target
#define PIN_NUM_CS_B   10
#define PIN_NUM_GDO0_B 5

extern spi_device_handle_t cc1101_handle;

extern SemaphoreHandle_t tx_done_sem_a;
extern SemaphoreHandle_t tx_done_sem_b;

extern SemaphoreHandle_t cc1101_bus_mutex;

esp_err_t init_hardware(void);

typedef struct {
    int64_t time_us; /* esp_timer time since boot, sampled at ISR service */
    uint32_t epoch;  /* Changes after capture pauses; never infer a gap duration. */
    uint8_t level;   /* Digital demodulator level AFTER this edge. */
} rfuzz_raw_edge_t;

typedef struct {
    uint64_t overflow_events;
    uint64_t dropped_edges; /* Observed full-ring edges only; paused losses unknown. */
    bool paused;
} rfuzz_raw_stats_t;

/* Task APIs; only radio B is affected. Stop before changing radio registers. */
void rfuzz_b_capture_stop(void);
esp_err_t rfuzz_b_capture_prepare(bool raw);
esp_err_t rfuzz_b_capture_arm(void);
size_t rfuzz_b_raw_drain(rfuzz_raw_edge_t *edges, size_t capacity);
void rfuzz_b_raw_stats(rfuzz_raw_stats_t *stats);

#endif // HW_INIT_H
