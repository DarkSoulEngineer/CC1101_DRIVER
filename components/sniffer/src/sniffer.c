#include "sniffer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "SNIFFER";

/* ============================================================
 * DOUBLE-BUFFER + TASK-NOTIFICATION RECEIVE PATTERN
 *
 *   rmt_receive() is non-blocking: it arms the hardware and
 *   returns immediately.  When the buffer is full (or the idle
 *   timeout fires), the ISR callback runs and notifies the
 *   processing task which buffer is ready.
 *
 *   While the task processes buffer A, the RMT hardware is
 *   already filling buffer B, giving zero-gap capture.
 * ============================================================ */
static rmt_symbol_word_t  s_buf_a[SNIFFER_RX_BUF_SYMBOLS];
static rmt_symbol_word_t  s_buf_b[SNIFFER_RX_BUF_SYMBOLS];
static rmt_symbol_word_t *s_rx_buf     = s_buf_a;   /* currently being filled  */
static rmt_symbol_word_t *s_proc_buf   = s_buf_a;   /* ready to be processed   */
static volatile size_t    s_proc_count  = 0;
static TaskHandle_t       s_task_handle = NULL;

/* ISR callback – called when rmt_receive() completes */
static bool rmt_rx_done_cb(rmt_channel_handle_t chan,
                            const rmt_rx_done_event_data_t *edata,
                            void *user_ctx)
{
    s_proc_buf  = s_rx_buf;
    s_proc_count = edata->num_symbols;

    /* swap to the other buffer for the next receive */
    s_rx_buf = (s_rx_buf == s_buf_a) ? s_buf_b : s_buf_a;

    BaseType_t wake = pdFALSE;
    if (s_task_handle) {
        vTaskNotifyGiveFromISR(s_task_handle, &wake);
    }
    return wake == pdTRUE;
}

/* ============================================================
 * UART INITIALISATION  (UART2 – leaves UART0 console free)
 * ============================================================ */
static esp_err_t init_uart(void)
{
    const uart_config_t cfg = {
        .baud_rate  = SNIFFER_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(SNIFFER_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(SNIFFER_UART_PORT,
                                 SNIFFER_UART_TX_PIN,
                                 SNIFFER_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SNIFFER_UART_PORT, 2048, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "UART%d ready (TX=%d RX=%d @ %d baud)",
             SNIFFER_UART_PORT,
             SNIFFER_UART_TX_PIN,
             SNIFFER_UART_RX_PIN,
             SNIFFER_UART_BAUD);
    return ESP_OK;
}

/* ============================================================
 * STREAM A BATCH OF SYMBOLS OVER UART
 *
 *   [STX] [cnt_hi] [cnt_lo] [RSSI] [symbols ...] [CKSUM]
 * ============================================================ */
static void stream_batch(sniffer_ctx_t *ctx,
                          const rmt_symbol_word_t *syms,
                          size_t count)
{
    uint32_t data_bytes = count * sizeof(rmt_symbol_word_t);
    uint32_t frame_len  = 4 + data_bytes + 1;

    uint8_t *frame = malloc(frame_len);
    if (!frame) {
        ESP_LOGE(TAG, "OOM (%lu bytes)", (unsigned long)frame_len);
        return;
    }

    frame[0] = SNIFFER_STX;
    frame[1] = (count >> 8) & 0xFF;
    frame[2] =  count       & 0xFF;
    frame[3] = ctx->rssi;
    memcpy(&frame[4], syms, data_bytes);

    uint8_t cksum = 0;
    for (uint32_t i = 0; i < frame_len - 1; i++) {
        cksum ^= frame[i];
    }
    frame[frame_len - 1] = cksum;

    int written = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        int ret = uart_write_bytes(SNIFFER_UART_PORT, frame, frame_len);
        if (ret == (int)frame_len) { written = frame_len; break; }
        if (ret > 0) written = ret;
        ets_delay_us(100);
    }

    free(frame);

    if (written < (int)frame_len) {
        ESP_LOGW(TAG, "UART short (%d / %lu)", written, (unsigned long)frame_len);
    }
}

/* ============================================================
 * CC1101 ASYNC SERIAL RX CONFIGURATION
 *
 *   PKTCTRL0 = 0x32  async serial, no whitening, continuous
 *   IOCFG0   = 0x0D  async data output on GDO0
 * ============================================================ */
static void cc1101_enter_async_rx(cc1101_handle_t *radio, uint32_t baud)
{
    uint8_t drate_e = 0, drate_m = 0;
    for (uint8_t e = 0; e < 16; e++) {
        uint64_t val = ((uint64_t)baud << 28) / (26000000ULL << e);
        if (val >= 256 && val <= 511) {
            drate_e = e;
            drate_m = (uint8_t)(val - 256);
            break;
        }
    }

    cc1101_strobe(radio, CC1101_SIDLE);
    cc1101_write_reg(radio, CC1101_PKTCTRL0, 0x32);
    cc1101_write_reg(radio, CC1101_IOCFG0,   0x0D);

    uint8_t mdmcfg4 = cc1101_read_reg(radio, CC1101_MDMCFG4);
    mdmcfg4 = (mdmcfg4 & 0xF0) | (drate_e & 0x0F);
    cc1101_write_reg(radio, CC1101_MDMCFG4, mdmcfg4);
    cc1101_write_reg(radio, CC1101_MDMCFG3, drate_m);

    cc1101_strobe(radio, CC1101_SRX);

    ESP_LOGI(TAG, "CC1101 async RX  DRATE_E=%u DRATE_M=%u (%lu bps)",
             drate_e, drate_m, (unsigned long)baud);
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t sniffer_init(sniffer_ctx_t *ctx,
                       cc1101_handle_t *radio,
                       uint32_t freq_hz,
                       uint32_t baud)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->data_rate_bps = baud;

    /* ---- UART2 ---- */
    ESP_ERROR_CHECK(init_uart());

    /* ---- RMT RX channel (new v6 API) ---- */
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num          = SNIFFER_RMT_RX_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = SNIFFER_RMT_RESOLUTION_HZ,
        .mem_block_symbols = SNIFFER_RMT_MEM_BLOCKS * 64,
        .intr_priority     = 0,
        .flags = {
            .invert_in = false,
            .with_dma  = false,
        },
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &ctx->rmt_chan));

    /* register completion callback */
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rmt_rx_done_cb,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(ctx->rmt_chan, &cbs, NULL));

    /* enable the channel */
    ESP_ERROR_CHECK(rmt_enable(ctx->rmt_chan));

    /* ---- configure CC1101 ---- */
    cc1101_enter_async_rx(radio, baud);
    cc1101_write_reg(radio, CC1101_IOCFG0, 0x0D);
    ctx->rssi = cc1101_read_status_reg(radio, CC1101_RSSI);

    ESP_LOGI(TAG, "Sniffer initialised  GPIO=%d  RES=%d Hz  RSSI=%d",
             SNIFFER_RMT_RX_GPIO, SNIFFER_RMT_RESOLUTION_HZ, ctx->rssi);
    return ESP_OK;
}

void sniffer_start(sniffer_ctx_t *ctx)
{
    s_task_handle = xTaskGetCurrentTaskHandle();
    s_rx_buf      = s_buf_a;
    s_proc_count  = 0;

    /* prime the first receive */
    rmt_receive_config_t rx_conf = {
        .signal_range_min_ns = SNIFFER_GLITCH_THRESH_NS,
        .signal_range_max_ns = SNIFFER_IDLE_THRESH_NS,
    };
    ESP_ERROR_CHECK(rmt_receive(ctx->rmt_chan,
                                s_buf_a,
                                SNIFFER_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &rx_conf));

    ctx->running = true;
    ESP_LOGI(TAG, "Capture started  (buf=%d symbols)", SNIFFER_RX_BUF_SYMBOLS);
}

void sniffer_stop(sniffer_ctx_t *ctx)
{
    ctx->running = false;
    rmt_disable(ctx->rmt_chan);
    ESP_LOGI(TAG, "Capture stopped");
}

esp_err_t sniffer_stream(sniffer_ctx_t *ctx)
{
    if (!ctx->running) return ESP_OK;

    /* wait for callback to signal data (50 ms timeout) */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    if (s_proc_count == 0) return ESP_OK;

    /* stream the completed buffer */
    stream_batch(ctx, s_proc_buf, s_proc_count);
    size_t count = s_proc_count;
    s_proc_count = 0;

    /* arm the next receive on the swapped buffer */
    rmt_receive_config_t rx_conf = {
        .signal_range_min_ns = SNIFFER_GLITCH_THRESH_NS,
        .signal_range_max_ns = SNIFFER_IDLE_THRESH_NS,
    };
    esp_err_t ret = rmt_receive(ctx->rmt_chan,
                                s_rx_buf,
                                SNIFFER_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &rx_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Flushed %lu symbols", (unsigned long)count);
    return ESP_OK;
}

void sniffer_deinit(sniffer_ctx_t *ctx)
{
    sniffer_stop(ctx);

    if (ctx->rmt_chan) {
        rmt_del_channel(ctx->rmt_chan);
        ctx->rmt_chan = NULL;
    }

    uart_driver_delete(SNIFFER_UART_PORT);
    ESP_LOGI(TAG, "Sniffer de-initialised");
}
