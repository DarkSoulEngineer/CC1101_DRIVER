#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "sump_capture.h"
#include "esp_task_wdt.h"

#if CONFIG_SUMP_TRANSPORT_UART
#include "driver/uart.h"
#else
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "CAP";

/* ── state ─────────────────────────────────────────────────────────── */
static capture_state_t g_state;

/* One byte per sample: bit0=GDO0, bit1=GDO2 */
static uint8_t *s_capture_buf;
static uint32_t s_buf_size;

static gptimer_handle_t s_timer;
static TaskHandle_t     s_stream_task;

/* ISR state */
static volatile uint32_t s_write_idx;
static volatile uint32_t s_total_samples;
static volatile bool    s_done;

/* ── transport ─────────────────────────────────────────────────────── */
#if CONFIG_SUMP_TRANSPORT_UART
#define TRANSPORT_UART UART_NUM_0

static void transport_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = CONFIG_SUMP_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(TRANSPORT_UART, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(TRANSPORT_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(TRANSPORT_UART, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "Transport: UART0 @ %d baud", CONFIG_SUMP_UART_BAUD);
}

static void transport_write(const uint8_t *data, size_t len)
{
    uart_write_bytes(TRANSPORT_UART, data, len);
}

static int transport_read(uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = uart_read_bytes(TRANSPORT_UART, buf + total, len - total,
                                pdMS_TO_TICKS(5000));
        if (n <= 0) break;
        total += n;
    }
    return total;
}

static void transport_drain(void)
{
    uart_flush_input(TRANSPORT_UART);
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_flush_input(TRANSPORT_UART);
}

#else /* USB Serial JTAG */

static void transport_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "Transport: USB Serial JTAG");
}

static void transport_write(const uint8_t *data, size_t len)
{
    /* A single write_bytes() call queues one ring buffer item; the item can
     * never exceed the TX ring buffer capacity, so stream in small chunks. */
    const size_t chunk = 1024;
    size_t written = 0;
    while (written < len) {
        size_t n = len - written;
        if (n > chunk) {
            n = chunk;
        }
        int w = usb_serial_jtag_write_bytes(data + written, n,
                                            pdMS_TO_TICKS(1000));
        if (w <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            written += w;
        }
    }
}

static int transport_read(uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = usb_serial_jtag_read_bytes(buf + total, len - total,
                                            pdMS_TO_TICKS(100));
        if (n <= 0) {
            break;
        }
        total += n;
    }
    return total;
}

static void transport_drain(void)
{
    uint8_t tmp[128];
    vTaskDelay(pdMS_TO_TICKS(500));
    while (usb_serial_jtag_read_bytes(tmp, sizeof(tmp), pdMS_TO_TICKS(10)) > 0)
        ;
}

#endif

/* ── Timer ISR: pack GDO0/GDO2 into buffer, one byte per sample ──── */
static IRAM_ATTR bool timer_isr_cb(gptimer_handle_t timer,
                                    const gptimer_alarm_event_data_t *edata,
                                    void *user_data)
{
    if (s_write_idx >= s_total_samples) {
        gptimer_stop(timer);
        s_done = true;
        BaseType_t wake = pdFALSE;
        if (s_stream_task) {
            vTaskNotifyGiveFromISR(s_stream_task, &wake);
        }
        return wake;
    }

    uint8_t sample = 0;
    if (gpio_get_level(g_state.gdo0_pin)) sample |= 0x01;
    if (g_state.gdo2_pin >= 0 && gpio_get_level(g_state.gdo2_pin)) sample |= 0x02;

    s_capture_buf[s_write_idx] = sample;
    s_write_idx++;

    return false;
}

static void timer_init(uint32_t sample_rate_hz)
{
    gptimer_config_t tcfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&tcfg, &s_timer));

    gptimer_alarm_config_t acfg = {
        .reload_count = 0,
        .alarm_count  = 1000000 / sample_rate_hz,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(s_timer, &acfg));

    gptimer_event_callbacks_t cbs = { .on_alarm = timer_isr_cb };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(s_timer, &cbs, NULL));
    ESP_ERROR_CHECK(gptimer_enable(s_timer));

    ESP_LOGI(TAG, "Timer: %lu Hz (alarm=%lu us)",
             (unsigned long)sample_rate_hz,
             (unsigned long)(1000000 / sample_rate_hz));
}

/* ── stream task: wait for capture, send raw bytes over transport ──── */
static void stream_task(void *arg)
{
    /* No TWDT entry exists for this task: xTaskCreatePinnedToCore() does not
     * subscribe it to the task watchdog, so there is nothing to unsubscribe
     * and calling esp_task_wdt_delete(NULL) would only log a misleading
     * "task not found" error. It blocks regularly, so it never starves the
     * idle task either. */

    while (1) {
        uint8_t hdr[9];
        int len = transport_read(hdr, sizeof(hdr));
        if (len != 9 || hdr[0] != CAPTURE_CMD_START) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t rate   = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8)
                        | ((uint32_t)hdr[3] << 16) | ((uint32_t)hdr[4] << 24);
        uint32_t count  = (uint32_t)hdr[5] | ((uint32_t)hdr[6] << 8)
                        | ((uint32_t)hdr[7] << 16) | ((uint32_t)hdr[8] << 24);

        if (rate == 0 || rate > 500000) {
            ESP_LOGW(TAG, "Bad rate %lu, clamping to 500000", (unsigned long)rate);
            rate = 500000;
        }
        if (count > s_buf_size) {
            ESP_LOGW(TAG, "Count %lu exceeds buffer %lu, clamping",
                     (unsigned long)count, (unsigned long)s_buf_size);
            count = s_buf_size;
        }

        g_state.sample_rate = rate;
        g_state.num_samples = count;

        ESP_LOGI(TAG, "Capture: rate=%lu Hz, count=%lu samples",
                 (unsigned long)rate, (unsigned long)count);

        /* Reconfigure timer */
        gptimer_disable(s_timer);
        gptimer_alarm_config_t acfg = {
            .reload_count = 0,
            .alarm_count  = 1000000 / rate,
            .flags.auto_reload_on_alarm = true,
        };
        gptimer_set_alarm_action(s_timer, &acfg);
        gptimer_enable(s_timer);

        /* Clear buffer */
        memset(s_capture_buf, 0, count);

        /* Start ISR capture */
        s_write_idx      = 0;
        s_total_samples  = count;
        s_done           = false;

        ESP_ERROR_CHECK(gptimer_start(s_timer));

        /* Wait for ISR to finish */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGI(TAG, "Capture done: %lu bytes, streaming...",
                 (unsigned long)count);

        /* Stream raw bytes directly */
        transport_write(s_capture_buf, count);

        ESP_LOGI(TAG, "Stream complete");
    }
}

/* ── public API ────────────────────────────────────────────────────── */
esp_err_t capture_init(gpio_num_t gdo0_pin, gpio_num_t gdo2_pin)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.gdo0_pin     = gdo0_pin;
    g_state.gdo2_pin     = gdo2_pin;
    g_state.num_channels = (gdo2_pin >= 0) ? 2 : 1;

    s_buf_size = CONFIG_SUMP_MAX_SAMPLES;

    /* Allocate capture buffer — try IRAM first for ISR speed, fall back to PSRAM */
    s_capture_buf = heap_caps_malloc(s_buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_capture_buf) {
        s_capture_buf = heap_caps_malloc(s_buf_size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    }
    if (!s_capture_buf) {
        s_capture_buf = heap_caps_malloc(s_buf_size, MALLOC_CAP_8BIT);
    }
    if (!s_capture_buf) {
        ESP_LOGE(TAG, "Failed to allocate capture buffer (%lu bytes)",
                 (unsigned long)s_buf_size);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Buffer: %p (%lu bytes)", s_capture_buf, (unsigned long)s_buf_size);

    /* Configure GDO pins */
    gpio_config_t iocfg = {
        .pin_bit_mask = (1ULL << gdo0_pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&iocfg);

    if (gdo2_pin >= 0) {
        iocfg.pin_bit_mask = (1ULL << gdo2_pin);
        gpio_config(&iocfg);
        ESP_LOGI(TAG, "GPIO: GDO0=%d GDO2=%d (2ch)", gdo0_pin, gdo2_pin);
    } else {
        ESP_LOGI(TAG, "GPIO: GDO0=%d (1ch)", gdo0_pin);
    }

    /* Timer at default 24 kHz — reconfigured per capture */
    timer_init(24000);

    /* Transport */
    transport_init();
    transport_drain();

    /* Stream command loop task — on CPU0, low priority so IDLE1 on CPU1 is fine */
    xTaskCreatePinnedToCore(stream_task, "stream", 8192, NULL, 2,
                            &s_stream_task, 0);

    /* Send a boot marker so the host knows we're alive */
    uint8_t marker = 0xAA;
    transport_write(&marker, 1);

    ESP_LOGI(TAG, "Ready. Waiting for start command [0x01][rate:4LE][count:4LE]");

    return ESP_OK;
}
