#include "hw_init.h"
#include "esp_log.h"

static const char *TAG = "HW_INIT";

spi_device_handle_t hw_cc1101_spi = NULL;

esp_err_t init_hardware(void)
{
    esp_err_t ret = hw_spi_init(PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK,
                                PIN_NUM_CS, CC1101_SPI_SPEED_HZ,
                                &hw_cc1101_spi);
    if (ret != ESP_OK) {
        return ret;
    }

    return hw_usb_init();
}

esp_err_t hw_init_gdo0_input(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_GDO0),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GDO0 config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GDO0 (GPIO %d) configured as input, level=%d",
             PIN_NUM_GDO0, gpio_get_level(PIN_NUM_GDO0));
    return ESP_OK;
}

esp_err_t hw_init_gdo2_input(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_NUM_GDO2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GDO2 config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "GDO2 (GPIO %d) configured as input, level=%d",
             PIN_NUM_GDO2, gpio_get_level(PIN_NUM_GDO2));
    return ESP_OK;
}
