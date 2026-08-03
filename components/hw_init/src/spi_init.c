#include "spi_init.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SPI_INIT";

esp_err_t hw_spi_init(int mosi_io, int miso_io, int sclk_io, int cs_io,
                      uint32_t speed_hz, spi_device_handle_t *out_dev)
{
    if (!out_dev) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_dev = NULL;

    gpio_set_direction(cs_io, GPIO_MODE_OUTPUT);
    gpio_set_level(cs_io, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = mosi_io,
        .miso_io_num = miso_io,
        .sclk_io_num = sclk_io,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = speed_hz,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Bus Init Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_device(SPI3_HOST, &devcfg, out_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI Device Add Failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI initialized (%lu Hz, DMA)", (unsigned long)speed_hz);
    return ESP_OK;
}
