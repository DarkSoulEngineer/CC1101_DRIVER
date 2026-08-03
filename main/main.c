#include "esp_log.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"

#include "hw_init.h"
#include "cc1101.h"
#include "sump_capture.h"

static const char *TAG = "MAIN";

static cc1101_handle_t s_radio;

static void rx_loop_task(void *arg)
{
    cc1101_handle_t *radio = (cc1101_handle_t *)arg;
    uint8_t data[CC1101_MAX_PACKET_LEN] = {0};

    while (1) {
        size_t len = CC1101_MAX_PACKET_LEN;

        if (cc1101_receive_packet(radio, data, &len)) {
            ESP_LOGI(TAG, "RX OK (%zu bytes)", len);

            if (usb_serial_jtag_is_driver_installed() && usb_serial_jtag_is_connected()
                && !capture_is_active()) {
                /* Skip packet logging while a capture/stream owns the USB
                 * transport, so the raw capture byte stream stays clean. */
                uint8_t header[2] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
                usb_serial_jtag_write_bytes(header, 2, pdMS_TO_TICKS(100));
                usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
                usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
            }

            cc1101_set_rx_mode(radio);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== RFuzz Capture (RX only) ===");

    if (init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (cc1101_init(&s_radio, hw_cc1101_spi,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "PARTNUM=0x%02X  VERSION=0x%02X",
             cc1101_read_status_reg(&s_radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&s_radio, CC1101_VERSION));

    /* Packet RX with sync word 0xDEAF: demodulated bits after sync on GDO2. */
    cc1101_config_t rx_cfg = CC1101_DEFAULT_CONFIG();
    rx_cfg.freq_hz = 433920000;
    rx_cfg.modem.modulation    = CC1101_MOD_2FSK_E;
    rx_cfg.modem.sync_mode     = CC1101_SYNC_16_16_E;
    rx_cfg.modem.preamble_bytes = 4;
    rx_cfg.modem.datarate_bps  = 2400;
    rx_cfg.modem.deviation     = 0x27;
    rx_cfg.modem.chanbw        = 0x0C;
    rx_cfg.packet.mode         = CC1101_PKT_FIXED_E;
    rx_cfg.packet.crc_enable   = false;
    rx_cfg.packet.whitening    = false;
    rx_cfg.packet.append_status = false;
    rx_cfg.packet.max_length   = 4;
    rx_cfg.packet.sync1        = 0xDE;
    rx_cfg.packet.sync0        = 0xAF;
    rx_cfg.radio.gdo0_mode     = CC1101_GDO_SYNC_WORD;
    rx_cfg.radio.gdo2_mode     = CC1101_GDO_ASYNC_DATA;
    rx_cfg.radio.autocal       = CC1101_AUTOCAL_ALWAYS;
    rx_cfg.radio.pin_mode      = 0x3F;
    rx_cfg.radio.pin_output    = true;

    if (cc1101_configure(&s_radio, &rx_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    
    /* Restore working register values from known-good packet mode */
    cc1101_write_reg(&s_radio, CC1101_PKTCTRL1, 0x04);
    cc1101_write_reg(&s_radio, CC1101_MCSM1, 0x3F);
    
    cc1101_set_rx_mode(&s_radio);

    cc1101_log_registers(&s_radio);
    cc1101_start_status_monitor(&s_radio, 500);

    gpio_num_t gdo2_pin = (CONFIG_SUMP_GDO2_MODE != 0x2E) ? PIN_NUM_GDO2 : -1;

    esp_err_t ret = capture_init(PIN_NUM_GDO0, gdo2_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Capture init failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "Ready. Commands: [0x01][rate:4LE][count:4LE] capture, [0x03][rate:4LE] stream, [0x04] stop stream");
    ESP_LOGI(TAG, "Also streaming received packets over USB Serial JTAG (COM port)");

    /* Packet RX loop runs on CPU1, while the high-rate capture gptimer ISR
     * (allocated here on CPU0 via capture_init) owns CPU0. This keeps the
     * CC1101 FIFO drained even while a capture is running. */
    xTaskCreatePinnedToCore(rx_loop_task, "rx_loop", 4096, &s_radio, 4,
                            NULL, 1);

    vTaskDelete(NULL);
}