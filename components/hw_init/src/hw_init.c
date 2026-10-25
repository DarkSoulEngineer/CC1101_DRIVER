#include "hw_init.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "HW_INIT";

spi_device_handle_t cc1101_handle = NULL;
SemaphoreHandle_t tx_done_sem_a = NULL;
SemaphoreHandle_t tx_done_sem_b = NULL;
SemaphoreHandle_t cc1101_bus_mutex = NULL;

#define RAW_RING_SIZE 256
static rfuzz_raw_edge_t s_edges[RAW_RING_SIZE];
static size_t s_head, s_count;
static bool s_raw, s_capture, s_paused, s_handler_installed = true;
static uint32_t s_epoch;
static uint64_t s_overflows, s_dropped;
static portMUX_TYPE s_raw_lock = portMUX_INITIALIZER_UNLOCKED;
static void gdo0_isr_b(void *arg);

static void IRAM_ATTR gdo0_isr_raw(void *arg)
{
    int64_t now = esp_timer_get_time();
    uint8_t level = gpio_get_level(PIN_NUM_GDO0_B);
    portENTER_CRITICAL_ISR(&s_raw_lock);
    if (s_capture) {
        if (s_count < RAW_RING_SIZE) {
            size_t pos = (s_head + s_count) % RAW_RING_SIZE;
            s_edges[pos] = (rfuzz_raw_edge_t){now, s_epoch, level};
            s_count++;
        } else {
            s_overflows++;
            s_dropped++;
            s_capture = false;
            s_paused = true;
            /* Bound ISR load under noise. Edges while paused cannot be counted. */
            gpio_intr_disable(PIN_NUM_GDO0_B);
        }
    }
    portEXIT_CRITICAL_ISR(&s_raw_lock);
}

void rfuzz_b_capture_stop(void)
{
    gpio_intr_disable(PIN_NUM_GDO0_B);
    portENTER_CRITICAL(&s_raw_lock);
    s_capture = false;
    portEXIT_CRITICAL(&s_raw_lock);
}

esp_err_t rfuzz_b_capture_prepare(bool raw)
{
    rfuzz_b_capture_stop();
    esp_err_t err = ESP_OK;
    if (s_handler_installed) {
        err = gpio_isr_handler_remove(PIN_NUM_GDO0_B);
        if (err != ESP_OK) return err;
        s_handler_installed = false;
    }
    portENTER_CRITICAL(&s_raw_lock);
    s_raw = raw;
    s_head = s_count = 0;
    s_overflows = s_dropped = 0;
    s_paused = false;
    s_epoch++;
    portEXIT_CRITICAL(&s_raw_lock);
    xSemaphoreTake(tx_done_sem_b, 0);
    err = gpio_set_intr_type(PIN_NUM_GDO0_B, raw ? GPIO_INTR_ANYEDGE : GPIO_INTR_NEGEDGE);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

esp_err_t rfuzz_b_capture_arm(void)
{
    portENTER_CRITICAL(&s_raw_lock);
    s_capture = s_raw;
    s_paused = false;
    s_epoch++;
    portEXIT_CRITICAL(&s_raw_lock);
    esp_err_t err;
    if (!s_handler_installed) {
        err = gpio_isr_handler_add(PIN_NUM_GDO0_B, s_raw ? gdo0_isr_raw : gdo0_isr_b, NULL);
        if (err == ESP_OK) s_handler_installed = true;
    } else {
        err = gpio_intr_enable(PIN_NUM_GDO0_B);
    }
    if (err != ESP_OK) rfuzz_b_capture_stop();
    return err;
}

size_t rfuzz_b_raw_drain(rfuzz_raw_edge_t *edges, size_t capacity)
{
    portENTER_CRITICAL(&s_raw_lock);
    size_t count = s_count < capacity ? s_count : capacity;
    for (size_t i = 0; i < count; i++)
        edges[i] = s_edges[(s_head + i) % RAW_RING_SIZE];
    s_head = (s_head + count) % RAW_RING_SIZE;
    s_count -= count;
    portEXIT_CRITICAL(&s_raw_lock);
    return count;
}

void rfuzz_b_raw_stats(rfuzz_raw_stats_t *stats)
{
    portENTER_CRITICAL(&s_raw_lock);
    *stats = (rfuzz_raw_stats_t){s_overflows, s_dropped, s_paused};
    portEXIT_CRITICAL(&s_raw_lock);
}

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
