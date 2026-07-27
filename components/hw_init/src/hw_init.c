#include "hw_init.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

static const char *TAG = "HW_INIT";

spi_device_handle_t cc1101_handle = NULL;

esp_err_t init_hardware(void)
{
    gpio_set_direction(PIN_NUM_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_CS, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = CC1101_SPI_SPEED_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Bus Init Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &cc1101_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Device Add Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI initialized (1 MHz, DMA)");
    return ESP_OK;
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
