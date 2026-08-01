#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "hw_init.h"
#include "cc1101.h"
#include "sump_capture.h"

static const char *TAG = "MAIN";

static void dump_radio_regs(cc1101_handle_t *dev)
{
    ESP_LOGI(TAG, "=== CC1101 Register Dump ===");
    ESP_LOGI(TAG, "  IOCFG2  = 0x%02X",  cc1101_read_reg(dev, CC1101_IOCFG2));
    ESP_LOGI(TAG, "  IOCFG0  = 0x%02X",  cc1101_read_reg(dev, CC1101_IOCFG0));
    ESP_LOGI(TAG, "  PKTCTRL1= 0x%02X",  cc1101_read_reg(dev, CC1101_PKTCTRL1));
    ESP_LOGI(TAG, "  PKTCTRL0= 0x%02X",  cc1101_read_reg(dev, CC1101_PKTCTRL0));
    ESP_LOGI(TAG, "  SYNC1   = 0x%02X",  cc1101_read_reg(dev, CC1101_SYNC1));
    ESP_LOGI(TAG, "  SYNC0   = 0x%02X",  cc1101_read_reg(dev, CC1101_SYNC0));
    ESP_LOGI(TAG, "  PKTLEN  = 0x%02X",  cc1101_read_reg(dev, CC1101_PKTLEN));
    ESP_LOGI(TAG, "  MDMCFG4 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG4));
    ESP_LOGI(TAG, "  MDMCFG3 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG3));
    ESP_LOGI(TAG, "  MDMCFG2 = 0x%02X",  cc1101_read_reg(dev, CC1101_MDMCFG2));
    ESP_LOGI(TAG, "  MCSM1   = 0x%02X",  cc1101_read_reg(dev, CC1101_MCSM1));
    ESP_LOGI(TAG, "  FREQ    = 0x%02X/0x%02X/0x%02X",
             cc1101_read_reg(dev, CC1101_FREQ2),
             cc1101_read_reg(dev, CC1101_FREQ1),
             cc1101_read_reg(dev, CC1101_FREQ0));
    ESP_LOGI(TAG, "  MARCSTATE= 0x%02X", cc1101_read_status_reg(dev, CC1101_MARCSTATE) & 0x1F);
    ESP_LOGI(TAG, "  PKTSTATUS= 0x%02X", cc1101_read_status_reg(dev, CC1101_PKTSTATUS));
    ESP_LOGI(TAG, "  RSSI    = %d dBm",  (int8_t)cc1101_read_status_reg(dev, CC1101_RSSI));
    ESP_LOGI(TAG, "============================");
}

void app_main(void)
{
    static cc1101_handle_t radio;

    ESP_LOGI(TAG, "=== RFuzz Capture ===");

    if (init_hardware() != ESP_OK) {
        ESP_LOGE(TAG, "Hardware init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    if (cc1101_init(&radio, cc1101_handle,
                    PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0) != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 init failed");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "PARTNUM=0x%02X  VERSION=0x%02X",
             cc1101_read_status_reg(&radio, CC1101_PARTNUM),
             cc1101_read_status_reg(&radio, CC1101_VERSION));

    /* CC1101 transparent async serial RX config */
    cc1101_config_t cfg = CC1101_DEFAULT_CONFIG();
    cfg.isr_enabled = false;
    cfg.freq_hz = 433920000;
    cfg.modem.datarate_bps = 2400;
    cfg.modem.modulation = CC1101_MOD_2FSK_E;
    cfg.modem.sync_mode = CC1101_SYNC_NONE_E;
    cfg.modem.chanbw = 0x00;
    cfg.modem.deviation = 0x47;
    cfg.packet.mode = CC1101_PKT_INFINITE_E;
    cfg.packet.crc_enable = false;
    cfg.packet.whitening = false;
    cfg.packet.append_status = false;
    cfg.radio.gdo0_mode = CC1101_GDO_ASYNC_DATA;
    cfg.radio.gdo2_mode = 0x2E;
    cfg.radio.autocal = CC1101_AUTOCAL_ALWAYS;
    cfg.radio.pin_mode = 0x3F;

    esp_err_t ret = cc1101_configure(&radio, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CC1101 configure failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    cc1101_write_reg(&radio, CC1101_PKTCTRL0,
                     CC1101_PKT_FORMAT_ASYNC | CC1101_PKTLEN_INFINITE);
    cc1101_write_reg(&radio, CC1101_PKTCTRL1, 0x04);
    cc1101_write_reg(&radio, CC1101_MCSM1, 0x3F);
    cc1101_write_reg(&radio, CC1101_IOCFG0, CC1101_GDO_ASYNC_DATA);
    cc1101_write_reg(&radio, CC1101_IOCFG2, 0x2E);

    cc1101_set_rx_mode(&radio);

    dump_radio_regs(&radio);

    /* Capture init */
    gpio_num_t gdo2_pin = (CONFIG_SUMP_GDO2_MODE != 0x2E) ? PIN_NUM_GDO2 : -1;

    ret = capture_init(PIN_NUM_GDO0, gdo2_pin);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Capture init failed: %s", esp_err_to_name(ret));
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    ESP_LOGI(TAG, "Ready.");
}
