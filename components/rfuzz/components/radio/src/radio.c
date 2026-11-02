#include "radio.h"
#include <stdio.h>
#include "esp_log.h"
#include "hw_init.h"

static const char *TAG = "RFUZZ";

#define RFUZZ_LINK_FREQUENCY_HZ 433920000
#define RFUZZ_LINK_DATARATE     38400
#define RFUZZ_LINK_CHANNEL      0

void rfuzz_log_hex(const char *tag,
                   const char *prefix,
                   const uint8_t *buf,
                   size_t len)
{
    char hex[CC1101_MAX_PACKET_LEN * 3 + 1] = {0};
    size_t pos = 0;

    for (size_t i = 0; i < len && pos < sizeof(hex) - 3; i++)
        pos += sprintf(&hex[pos], "%02X ", buf[i]);

    ESP_LOGI(tag, "%s [%s]", prefix, hex);
}

esp_err_t rfuzz_radio_init(cc1101_handle_t *dev,
                           const char *name,
                           gpio_num_t cs_pin,
                           gpio_num_t gdo0_pin)
{
    if (cc1101_init(dev, cc1101_handle, cs_pin, PIN_NUM_MISO, gdo0_pin) != ESP_OK)
        return ESP_FAIL;

    ESP_LOGI(TAG, "%s PARTNUM=0x%02X VERSION=0x%02X",
             name,
             cc1101_read_status_reg(dev, CC1101_PARTNUM),
             cc1101_read_status_reg(dev, CC1101_VERSION));

    cc1101_config(dev);
    cc1101_set_frequency(dev, RFUZZ_LINK_FREQUENCY_HZ);
    cc1101_set_channel(dev, RFUZZ_LINK_CHANNEL);
    cc1101_set_datarate(dev, RFUZZ_LINK_DATARATE);

    return ESP_OK;
}
