#ifndef SNIFFER_H
#define SNIFFER_H

#include "cc1101.h"
#include "driver/rmt_rx.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include <stdbool.h>

/* ============================================================
 * DEFAULT PIN ASSIGNMENTS
 * ============================================================ */
#define SNIFFER_RMT_RX_GPIO   GPIO_NUM_25
#define SNIFFER_UART_PORT     UART_NUM_2
#define SNIFFER_UART_TX_PIN   GPIO_NUM_17
#define SNIFFER_UART_RX_PIN   GPIO_NUM_16
#define SNIFFER_UART_BAUD     115200

/* ============================================================
 * RMT / BUFFER TUNING
 * ============================================================ */
#define SNIFFER_RMT_RESOLUTION_HZ  1000000   /* 1 tick = 1 us */
#define SNIFFER_RMT_MEM_BLOCKS     4         /* 4 x 64 = 256 hw slots */
#define SNIFFER_RX_BUF_SYMBOLS     1024      /* symbols per double-buffer */

/* idle timeout: 10 ms of no transitions => end of burst */
#define SNIFFER_IDLE_THRESH_NS     10000000ULL
/* ignore glitches < 5 us */
#define SNIFFER_GLITCH_THRESH_NS   3000ULL

/* ============================================================
 * STREAMING PROTOCOL  (UART, big-endian per-frame)
 *
 *   [0x02 STX] [hi cnt] [lo cnt] [RSSI] [sym0..N-1 ...] [CKSUM]
 *
 * Each symbol is 4 bytes (rmt_symbol_word_t).
 * CKSUM = XOR of every byte in the frame (STX .. last symbol byte).
 * ============================================================ */
#define SNIFFER_STX  0x02

/* ============================================================
 * CONTEXT
 * ============================================================ */
typedef struct {
    rmt_channel_handle_t  rmt_chan;
    uart_port_t           uart_port;
    uint32_t              data_rate_bps;
    uint8_t               rssi;
    volatile bool         running;
} sniffer_ctx_t;

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t sniffer_init(sniffer_ctx_t *ctx,
                       cc1101_handle_t *radio,
                       uint32_t freq_hz,
                       uint32_t baud);

void sniffer_start(sniffer_ctx_t *ctx);
void sniffer_stop(sniffer_ctx_t *ctx);
esp_err_t sniffer_stream(sniffer_ctx_t *ctx);
void sniffer_deinit(sniffer_ctx_t *ctx);

#endif /* SNIFFER_H */
