#include "usb_interface.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "USB";

esp_err_t hw_usb_init(void)
{
    if (usb_serial_jtag_is_driver_installed()) {
        ESP_LOGI(TAG, "USB Serial/JTAG driver already installed");
        return ESP_OK;
    }

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = CONFIG_USB_INTERFACE_USJ_TX_BUFFER_SIZE,
        .rx_buffer_size = CONFIG_USB_INTERFACE_USJ_RX_BUFFER_SIZE,
    };

    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "USB Serial/JTAG ready (native USB)%s",
             usb_serial_jtag_is_connected() ? ", host connected" : "");

    return ESP_OK;
}
