#include "hw_init.h"
#include "esp_log.h"

static const char *TAG = "HW_INIT";

spi_device_handle_t cc1101_handle = NULL;
SemaphoreHandle_t tx_done_sem_a = NULL;
SemaphoreHandle_t tx_done_sem_b = NULL;
SemaphoreHandle_t cc1101_bus_mutex = NULL;

static void IRAM_ATTR gdo0_isr_a(void* arg)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(tx_done_sem_a, &high_task_wakeup);

    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void IRAM_ATTR gdo0_isr_b(void* arg)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(tx_done_sem_b, &high_task_wakeup);

    if (high_task_wakeup == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t init_hardware(void)
{
    gpio_set_direction(PIN_NUM_CS_A, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_CS_A, 1);
    gpio_set_direction(PIN_NUM_CS_B, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_NUM_CS_B, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,
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

    gpio_config_t io_a = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << PIN_NUM_GDO0_A),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 0
    };
    gpio_config(&io_a);

    gpio_config_t io_b = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << PIN_NUM_GDO0_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 0,
        .pull_down_en = 0
    };
    gpio_config(&io_b);

    tx_done_sem_a = xSemaphoreCreateBinary();
    tx_done_sem_b = xSemaphoreCreateBinary();
    cc1101_bus_mutex = xSemaphoreCreateMutex();

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_NUM_GDO0_A, gdo0_isr_a, NULL);
    gpio_isr_handler_add(PIN_NUM_GDO0_B, gdo0_isr_b, NULL);

    ESP_LOGI(TAG, "SPI3 bus + dual GDO0 IRQs initialized");
    return ESP_OK;
}
