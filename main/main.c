#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

#include "hw_init.h"
#include "cc1101.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    init_hardware();
    hw_init_gdo0_input();

    cc1101_handle_t radio;
    cc1101_init(&radio, cc1101_handle, PIN_NUM_CS, PIN_NUM_MISO, PIN_NUM_GDO0);

    uint8_t partnum = cc1101_read_reg(&radio, CC1101_PARTNUM);
    uint8_t version = cc1101_read_reg(&radio, CC1101_VERSION);
    ESP_LOGI(TAG, "CC1101  PARTNUM=0x%02X  VERSION=0x%02X", partnum, version);

    /* Build config from Kconfig defaults, override what you want */
    cc1101_config_t cfg = CC1101_DEFAULT_CONFIG();
    // cfg.freq_hz = 868300000;          // override at runtime
    // cfg.pa_value = CC1101_PA_POS12dBm; // override at runtime
    cc1101_configure(&radio, &cfg);

    ESP_LOGI(TAG, "CC1101 configured (freq=%lu Hz, %d bps)",
             (unsigned long)cfg.freq_hz, cfg.modem.datarate_bps);
}
