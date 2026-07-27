#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_rom_gpio.h"
#include "esp_private/periph_ctrl.h"
#include "esp_private/gpio.h"
#include "driver/ledc.h"
#include "esp_private/gdma.h"
#include "hal/dma_types.h"
#include "soc/lcd_cam_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "sump_capture.h"

#if CONFIG_SUMP_TRANSPORT_UART
#include "driver/uart.h"
#else
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "SUMP";

/* ── state ─────────────────────────────────────────────────────────── */
static sump_state_t g_state;

/* Ping-pong DMA buffers (raw, 1 byte per sample) */
static uint8_t *s_buf_a;
static uint8_t *s_buf_b;
static uint32_t s_chunk_size;

/* DMA descriptors */
static dma_descriptor_align4_t *s_desc;

/* GDMA RX channel */
static gdma_channel_handle_t s_gdma_rx;

/* Repack staging area (8 samples packed per byte, per channel) */
static uint8_t *s_repack_ch0;
static uint8_t *s_repack_ch1;

/* Task handles */
static TaskHandle_t s_sump_task;
static volatile bool s_capture_done;
static volatile uint32_t s_bytes_received;

/* ── transport abstraction ─────────────────────────────────────────── */
#if CONFIG_SUMP_TRANSPORT_UART
#define SUMP_UART_NUM UART_NUM_0

static void sump_transport_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = CONFIG_SUMP_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(SUMP_UART_NUM, 8192, 0, 0, NULL, 0);
    assert(ret == ESP_OK);
    ret = uart_param_config(SUMP_UART_NUM, &cfg);
    assert(ret == ESP_OK);
    ret = uart_set_pin(SUMP_UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    assert(ret == ESP_OK);

    ESP_LOGI(TAG, "UART0 transport: %d baud", CONFIG_SUMP_UART_BAUD);
}

static void sump_write(const uint8_t *data, size_t len)
{
    uart_write_bytes(SUMP_UART_NUM, data, len);
}

static int sump_read(uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = uart_read_bytes(SUMP_UART_NUM, buf + total, len - total,
                                pdMS_TO_TICKS(2000));
        if (n <= 0) break;
        total += n;
    }
    return total;
}

static void sump_drain(void)
{
    uart_flush_input(SUMP_UART_NUM);
}

#else /* USB Serial JTAG transport */

static void sump_transport_init(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg =
            USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
        assert(ret == ESP_OK);
    }
    ESP_LOGI(TAG, "USB Serial JTAG transport");
}

static void sump_write(const uint8_t *data, size_t len)
{
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(2000));
}

static int sump_read(uint8_t *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = usb_serial_jtag_read_bytes(buf + total, len - total,
                                            pdMS_TO_TICKS(2000));
        if (n <= 0) break;
        total += n;
    }
    return total;
}

static void sump_drain(void)
{
#if CONFIG_SUMP_DTR_RESET_GUARD
    uint32_t guard_ms = CONFIG_SUMP_DTR_GUARD_MS;
    uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(guard_ms);
    uint8_t buf[64];
    while (xTaskGetTickCount() < deadline) {
        usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(50));
    }
#else
    vTaskDelay(pdMS_TO_TICKS(500));
    uint8_t buf[64];
    while (usb_serial_jtag_read_bytes(buf, sizeof(buf),
                                      pdMS_TO_TICKS(10)) > 0) {}
#endif
}

#endif /* CONFIG_SUMP_TRANSPORT_UART */

/* ── SUMP protocol ─────────────────────────────────────────────────── */
static void sump_send_id(void)
{
    const uint8_t id[4] = { '1', 'A', 'L', 'S' };
    sump_write(id, 4);
}

static void sump_send_metadata(void)
{
    uint8_t buf[64];
    int n = 0;

    /* 0x01 = device name */
    buf[n++] = 0x01; buf[n++] = 20;
    memcpy(&buf[n], "RFuzz Logic Analyzer", 20); n += 20;

    /* 0x02 = firmware version */
    buf[n++] = 0x02; buf[n++] = 3;
    memcpy(&buf[n], "3.0", 3); n += 3;

    /* 0x21 = sample memory size (4 bytes, big-endian) */
    buf[n++] = 0x21; buf[n++] = 4;
    uint32_t mem = CONFIG_SUMP_DMA_CHUNK_SIZE * 2;
    buf[n++] = (mem >> 24) & 0xFF; buf[n++] = (mem >> 16) & 0xFF;
    buf[n++] = (mem >> 8)  & 0xFF; buf[n++] = (mem >> 0)  & 0xFF;

    /* 0x22 = max sample rate (4 bytes, big-endian) */
    buf[n++] = 0x22; buf[n++] = 4;
    uint32_t max_rate = g_state.clock_freq;
    buf[n++] = (max_rate >> 24) & 0xFF; buf[n++] = (max_rate >> 16) & 0xFF;
    buf[n++] = (max_rate >> 8)  & 0xFF; buf[n++] = (max_rate >> 0)  & 0xFF;

    /* 0x40 = number of channels */
    buf[n++] = 0x40; buf[n++] = 1;
    buf[n++] = g_state.num_channels;

    /* 0x23 = protocol version (1 byte, 0x01 = OLS) */
    buf[n++] = 0x23; buf[n++] = 1;
    buf[n++] = 0x01;

    /* 0x00 = end of metadata */
    buf[n++] = 0x00;
    sump_write(buf, n);
}

/* ── LEDC sample clock generation ──────────────────────────────────── */
static void sample_clk_init(gpio_num_t pin, uint32_t freq_hz)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_2_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = freq_hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .gpio_num   = pin,
        .duty       = 2,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
}

static void sample_clk_set_freq(uint32_t freq_hz)
{
    ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq_hz);
}

/* ── GPIO matrix routing ───────────────────────────────────────────── */
static void gpio_route_init(void)
{
    gpio_num_t pclk   = (gpio_num_t)CONFIG_SUMP_PCLK_PIN;
    gpio_num_t de     = (gpio_num_t)CONFIG_SUMP_SIG_DE_PIN;
    gpio_num_t hsync  = (gpio_num_t)CONFIG_SUMP_SIG_HSYNC_PIN;
    gpio_num_t gdo0   = g_state.gdo0_pin;
    gpio_num_t gdo2   = g_state.gdo2_pin;

    /* Sample clock: LEDC output on pin, self-loopback to LCD_CAM PCLK input */
    gpio_func_sel(pclk, PIN_FUNC_GPIO);
    gpio_set_direction(pclk, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(pclk, LEDC_LS_SIG_OUT0_IDX, false, false);
    esp_rom_gpio_connect_in_signal(pclk, CAM_PCLK_IDX, false);

    /* Data-enable: output high → keeps the sampler capturing continuously */
    gpio_func_sel(de, PIN_FUNC_GPIO);
    gpio_set_direction(de, GPIO_MODE_OUTPUT);
    gpio_set_level(de, 1);
    esp_rom_gpio_connect_in_signal(de, CAM_H_ENABLE_IDX, false);

    /* Horizontal sync: output high → keeps the sampler in active region */
    gpio_func_sel(hsync, PIN_FUNC_GPIO);
    gpio_set_direction(hsync, GPIO_MODE_OUTPUT);
    gpio_set_level(hsync, 1);
    esp_rom_gpio_connect_in_signal(hsync, CAM_H_SYNC_IDX, false);

    /* CC1101 GDO0 → hardware data line 0 */
    gpio_func_sel(gdo0, PIN_FUNC_GPIO);
    gpio_set_direction(gdo0, GPIO_MODE_INPUT);
    gpio_set_pull_mode(gdo0, GPIO_FLOATING);
    esp_rom_gpio_connect_in_signal(gdo0, CAM_DATA_IN0_IDX, false);

    /* CC1101 GDO2 → hardware data line 1 */
    if (gdo2 >= 0) {
        gpio_func_sel(gdo2, PIN_FUNC_GPIO);
        gpio_set_direction(gdo2, GPIO_MODE_INPUT);
        gpio_set_pull_mode(gdo2, GPIO_FLOATING);
        esp_rom_gpio_connect_in_signal(gdo2, CAM_DATA_IN1_IDX, false);
    }

    ESP_LOGI(TAG, "GPIO route: CLK=%d DE=%d SYNC=%d D0=%d D1=%d",
             pclk, de, hsync, gdo0, gdo2);
}

/* ── LCD_CAM peripheral (hijacked as DMA-driven pin sampler) ──────── */
static void hw_sampler_init(void)
{
    periph_module_enable(PERIPH_LCD_CAM_MODULE);

    /* Clock: PLL_F160M (160 MHz), divider = 1 */
    LCD_CAM.cam_ctrl.cam_clk_sel       = 3;
    LCD_CAM.cam_ctrl.cam_clkm_div_num  = 1;
    LCD_CAM.cam_ctrl.cam_clkm_div_a    = 0;
    LCD_CAM.cam_ctrl.cam_clkm_div_b    = 0;

    /* Sampling config: free-running, byte-length EOF */
    LCD_CAM.cam_ctrl.cam_stop_en       = 0;
    LCD_CAM.cam_ctrl.cam_bit_order     = 0;
    LCD_CAM.cam_ctrl.cam_byte_order    = 0;
    LCD_CAM.cam_ctrl.cam_line_int_en   = 0;
    LCD_CAM.cam_ctrl.cam_vs_eof_en     = 0;

    /* 8-bit capture, VSYNC forced high, DE+HSYNC gate sampling */
    LCD_CAM.cam_ctrl1.cam_2byte_en         = 0;
    LCD_CAM.cam_ctrl1.cam_vh_de_mode_en    = 1;
    LCD_CAM.cam_ctrl1.cam_clk_inv          = 0;
    LCD_CAM.cam_ctrl1.cam_vsync_filter_en  = 0;
    LCD_CAM.cam_ctrl1.cam_de_inv           = 0;
    LCD_CAM.cam_ctrl1.cam_hsync_inv        = 0;
    LCD_CAM.cam_ctrl1.cam_vsync_inv        = 0;

    /* Bypass colour conversion (raw bits pass straight through) */
    LCD_CAM.cam_rgb_yuv.cam_conv_bypass = 1;

    /* We use GDMA EOF, not the LCD_CAM internal interrupts */
    LCD_CAM.lc_dma_int_clr.val = 0xFFFFFFFF;
    LCD_CAM.lc_dma_int_ena.val = 0;

    /* Commit clock config */
    LCD_CAM.cam_ctrl.cam_update = 1;

    ESP_LOGI(TAG, "LCD_CAM sampler: 8-bit, PCLK from LEDC, PLL_F160M/1");
}

static void hw_sampler_start(uint32_t num_bytes)
{
    LCD_CAM.cam_ctrl1.cam_rec_data_bytelen = num_bytes - 1;
    LCD_CAM.cam_ctrl1.cam_afifo_reset     = 1;
    LCD_CAM.cam_ctrl1.cam_start           = 1;
}

static void hw_sampler_stop(void)
{
    LCD_CAM.cam_ctrl1.cam_start = 0;
}

/* ── GDMA ──────────────────────────────────────────────────────────── */
static IRAM_ATTR bool gdma_rx_done_isr(gdma_channel_handle_t chan,
                                        gdma_event_data_t *edata,
                                        void *user_data)
{
    dma_descriptor_align4_t *finished_desc =
        (dma_descriptor_align4_t *)edata->rx_eof_desc_addr;

    s_bytes_received = finished_desc->dw0.length;
    s_capture_done   = true;

    hw_sampler_stop();

    BaseType_t wake = pdFALSE;
    vTaskNotifyGiveFromISR(s_sump_task, &wake);
    return wake;
}

static void gdma_init(void)
{
    gdma_channel_alloc_config_t alloc_cfg = { 0 };
    esp_err_t ret = gdma_new_ahb_channel(&alloc_cfg, NULL, &s_gdma_rx);
    assert(ret == ESP_OK);

    ret = gdma_connect(s_gdma_rx,
                       GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_CAM, 0));
    assert(ret == ESP_OK);

    gdma_transfer_config_t trans_cfg = {
        .max_data_burst_size = 32,
        .access_ext_mem      = false,
    };
    gdma_config_transfer(s_gdma_rx, &trans_cfg);

    gdma_rx_event_callbacks_t cbs = {
        .on_recv_eof = gdma_rx_done_isr,
    };
    gdma_register_rx_event_callbacks(s_gdma_rx, &cbs, NULL);

    ESP_LOGI(TAG, "GDMA RX connected to LCD_CAM");
}

/* ── DMA descriptor helpers ────────────────────────────────────────── */
static void desc_configure(dma_descriptor_align4_t *d, void *buf,
                            uint32_t size, bool eof)
{
    d->dw0.owner   = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
    d->dw0.length  = 0;
    d->dw0.size    = size;
    d->dw0.suc_eof = eof ? 1 : 0;
    d->dw0.err_eof = 0;
    d->buffer      = buf;
    d->next        = NULL;
}

/* ── repack + OLS upload ───────────────────────────────────────────── */
/*
 * GDMA buffer: 1 byte per sample, bit 0 = ch0, bit 1 = ch1.
 * OLS format:  8 samples packed per byte, ch0 and ch1 separate,
 *              sent newest-first.
 */
static void repack_and_upload(const uint8_t *raw, uint32_t num_samples)
{
    uint32_t groups = (num_samples + 7) / 8;
    uint32_t repack_bytes = groups * 2;

    if (repack_bytes > CONFIG_SUMP_DMA_CHUNK_SIZE * 2) {
        repack_bytes = CONFIG_SUMP_DMA_CHUNK_SIZE * 2;
        groups = repack_bytes / 2;
    }

    memset(s_repack_ch0, 0, repack_bytes);
    memset(s_repack_ch1, 0, repack_bytes);

    uint32_t limit = groups * 8;
    if (limit > num_samples) limit = num_samples;

    for (uint32_t i = 0; i < limit; i++) {
        uint32_t grp = i >> 3;
        uint32_t bit = i & 7;
        if (raw[i] & 0x01) s_repack_ch0[grp] |= (1U << bit);
        if (raw[i] & 0x02) s_repack_ch1[grp] |= (1U << bit);
    }

    uint8_t block[4];
    for (int32_t g = (int32_t)groups - 1; g >= 0; g--) {
        block[0] = s_repack_ch0[g];
        block[1] = (g_state.num_channels >= 2) ? s_repack_ch1[g] : 0;
        block[2] = 0;
        block[3] = 0;
        sump_write(block, 4);
    }
}

/* ── capture task ──────────────────────────────────────────────────── */
static void capture_task(void *arg)
{
    uint32_t total = g_state.read_count;
    uint32_t remaining = total;
    uint32_t chunk = s_chunk_size;
    int buf_toggle = 0;

    ESP_LOGI(TAG, "GDMA capture: %lu samples, chunk=%lu",
             (unsigned long)total, (unsigned long)chunk);

    s_capture_done   = false;
    g_state.armed    = true;
    g_state.sampling = true;

    while (remaining > 0) {
        uint32_t this_chunk = (remaining > chunk) ? chunk : remaining;
        uint8_t *active_buf = (buf_toggle == 0) ? s_buf_a : s_buf_b;

        memset(active_buf, 0, this_chunk);
        desc_configure(&s_desc[0], active_buf, this_chunk, true);

        s_capture_done   = false;
        s_bytes_received = 0;

        gdma_reset(s_gdma_rx);
        gdma_start(s_gdma_rx, (intptr_t)&s_desc[0]);

        hw_sampler_start(this_chunk);

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint32_t got = s_bytes_received;
        if (got > this_chunk) got = this_chunk;

        repack_and_upload(active_buf, got);

        remaining -= got;
        buf_toggle ^= 1;

        if (got < this_chunk) {
            ESP_LOGW(TAG, "Short chunk: expected %lu got %lu",
                     (unsigned long)this_chunk, (unsigned long)got);
            break;
        }
    }

    g_state.sampling = false;
    g_state.armed    = false;

    ESP_LOGI(TAG, "Capture complete: %lu samples",
             (unsigned long)(total - remaining));

    vTaskDelete(NULL);
}

/* ── SUMP command handler ──────────────────────────────────────────── */
static void sump_start_capture(void)
{
    if (g_state.sample_rate == 0 || g_state.read_count == 0) {
        ESP_LOGW(TAG, "Invalid config: rate=%lu count=%lu",
                 (unsigned long)g_state.sample_rate,
                 (unsigned long)g_state.read_count);
        return;
    }

    sample_clk_set_freq(g_state.sample_rate);

    ESP_LOGI(TAG, "Start: rate=%lu Hz, count=%lu samples",
             (unsigned long)g_state.sample_rate,
             (unsigned long)g_state.read_count);

    xTaskCreatePinnedToCore(capture_task, "gdma_cap", 8192, NULL, 6,
                            NULL, 0);
}

static void sump_handle_cmd(uint8_t cmd)
{
    uint8_t buf[8];

    switch (cmd) {

    case SUMP_CMD_ID:
        sump_send_id();
        break;

    case SUMP_CMD_RESET:
        hw_sampler_stop();
        gdma_stop(s_gdma_rx);
        g_state.armed      = false;
        g_state.sampling   = false;
        g_state.capture_done = false;
        break;

    case SUMP_CMD_RUN:
        sump_start_capture();
        break;

    case SUMP_CMD_GET_METADATA:
        sump_send_metadata();
        break;

    case SUMP_CMD_SET_DIVIDER:
        if (sump_read(buf, 3) == 3) {
            g_state.divider = (uint32_t)buf[0]
                            | ((uint32_t)buf[1] << 8)
                            | ((uint32_t)buf[2] << 16);
            g_state.sample_rate = g_state.clock_freq / (g_state.divider + 1);
        }
        break;

    case SUMP_CMD_SET_COUNT:
        if (sump_read(buf, 4) == 4) {
            uint16_t raw_read  = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            uint16_t raw_delay = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
            g_state.read_count  = (uint32_t)raw_read * 4;
            g_state.delay_count = (uint32_t)raw_delay * 4;
            if (g_state.read_count > CONFIG_SUMP_MAX_SAMPLES) {
                g_state.read_count = CONFIG_SUMP_MAX_SAMPLES;
            }
        }
        break;

    case SUMP_CMD_SET_FLAGS:
        sump_read(buf, 1);
        break;

    default:
        if (cmd >= SUMP_CMD_TRIGGER_MASK0 && cmd <= SUMP_CMD_TRIGGER_CONFIG3) {
            sump_read(buf, 1);
            int idx  = cmd - SUMP_CMD_TRIGGER_MASK0;
            int type = idx % 3;
            if (type == 0) g_state.trigger_mask  = buf[0];
            if (type == 1) g_state.trigger_value = buf[0];
        }
        break;
    }
}

/* ── SUMP command loop ─────────────────────────────────────────────── */
static void sump_task(void *arg)
{
    sump_drain();

    while (1) {
        uint8_t cmd;
        int len = sump_read(&cmd, 1);
        if (len != 1) continue;
        sump_handle_cmd(cmd);
    }
}

/* ── public API ────────────────────────────────────────────────────── */
esp_err_t sump_capture_init(gpio_num_t gdo0_pin, gpio_num_t gdo2_pin)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.gdo0_pin     = gdo0_pin;
    g_state.gdo2_pin     = gdo2_pin;
    g_state.num_channels = (gdo2_pin >= 0) ? 2 : 1;
    g_state.clock_freq   = CONFIG_SUMP_CLOCK_FREQ;

    s_chunk_size = CONFIG_SUMP_DMA_CHUNK_SIZE;
    if (s_chunk_size > 4092) s_chunk_size = 4092;

    /* ── DMA ping-pong buffers ──────────────────────────────────── */
    s_buf_a = heap_caps_malloc(s_chunk_size,
                               MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf_b = heap_caps_malloc(s_chunk_size,
                               MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf_a || !s_buf_b) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffers (%lu bytes each)",
                 (unsigned long)s_chunk_size);
        return ESP_ERR_NO_MEM;
    }

    /* ── DMA descriptors ────────────────────────────────────────── */
    s_desc = heap_caps_malloc(2 * sizeof(dma_descriptor_align4_t),
                              MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_desc) {
        ESP_LOGE(TAG, "Failed to allocate DMA descriptors");
        return ESP_ERR_NO_MEM;
    }
    memset(s_desc, 0, 2 * sizeof(dma_descriptor_align4_t));

    /* ── repack staging buffers ─────────────────────────────────── */
    uint32_t repack_size = s_chunk_size;
    s_repack_ch0 = heap_caps_malloc(repack_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_repack_ch1 = heap_caps_malloc(repack_size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!s_repack_ch0 || !s_repack_ch1) {
        ESP_LOGE(TAG, "Failed to allocate repack buffers");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "DMA buffers: buf_a=%p buf_b=%p desc=%p chunk=%lu",
             s_buf_a, s_buf_b, s_desc, (unsigned long)s_chunk_size);

    /* ── Sample clock (LEDC) ────────────────────────────────────── */
    sample_clk_init((gpio_num_t)CONFIG_SUMP_PCLK_PIN, 24000);

    /* ── GPIO matrix routing ────────────────────────────────────── */
    gpio_route_init();

    /* ── LCD_CAM peripheral (pin sampler) ───────────────────────── */
    hw_sampler_init();

    /* ── GDMA RX channel ────────────────────────────────────────── */
    gdma_init();

    /* ── Serial transport ───────────────────────────────────────── */
    sump_transport_init();

    /* ── DTR/RTS reset guard (USB mode only, UART has no DTR) ──── */
#if !CONFIG_SUMP_TRANSPORT_UART
    sump_drain();
#endif

    /* ── SUMP command loop ──────────────────────────────────────── */
    xTaskCreatePinnedToCore(sump_task, "sump_task", 4096, NULL, 5,
                            &s_sump_task, 1);

    ESP_LOGI(TAG, "SUMP init complete: %d ch, clock=%lu Hz, transport=%s",
             g_state.num_channels, (unsigned long)g_state.clock_freq,
#if CONFIG_SUMP_TRANSPORT_UART
             "UART0");
#else
             "USB-JTAG");
#endif

    return ESP_OK;
}

esp_err_t sump_capture_start(void)
{
    return ESP_OK;
}

esp_err_t sump_capture_stop(void)
{
    hw_sampler_stop();
    gdma_stop(s_gdma_rx);
    g_state.armed      = false;
    g_state.sampling   = false;
    g_state.capture_done = false;
    return ESP_OK;
}
