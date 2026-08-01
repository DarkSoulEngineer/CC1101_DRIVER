#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/usb_serial_jtag.h"

#include "usb_interface.h"

static const char *TAG = "USB_TEST";

#define HEARTBEAT_MS 1000
#define USB_RX_BUF   64

void app_main(void)
{
    ESP_LOGI(TAG, "=== Native USB (USB Serial/JTAG) interface test ===");

    ESP_LOGI(TAG, "Initializing USB interface (hw_usb_init)");
    if (hw_usb_init() != ESP_OK) {
        ESP_LOGE(TAG, "USB interface init FAILED");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }
    ESP_LOGI(TAG, "USB interface OK (driver installed=%d)",
             (int)usb_serial_jtag_is_driver_installed());

    bool connected = false;
    uint32_t ticks = 0, tx_total = 0, rx_total = 0;
    char line[96];
    uint8_t buf[USB_RX_BUF];

    while (1) {
        bool now = usb_serial_jtag_is_connected();
        if (now != connected) {
            ESP_LOGI(TAG, "Host %s (open a terminal on the USB port)",
                     now ? "CONNECTED" : "disconnected");
            connected = now;
        }

        if (connected) {
            int n = snprintf(line, sizeof(line),
                             "USB-OK tick=%lu tx=%lu rx=%lu\r\n",
                             (unsigned long)ticks,
                             (unsigned long)tx_total,
                             (unsigned long)rx_total);
            int w = usb_serial_jtag_write_bytes(line, n, pdMS_TO_TICKS(1000));
            tx_total += (w > 0) ? (unsigned)w : 0;

            if (usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(3000)) != ESP_OK) {
                ESP_LOGW(TAG, "TX flush timeout");
            }
            ticks++;
        }

        int r = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(10));
        if (r > 0) {
            rx_total += (unsigned)r;
            int w = usb_serial_jtag_write_bytes(buf, r, pdMS_TO_TICKS(100));
            tx_total += (w > 0) ? (unsigned)w : 0;
            ESP_LOGI(TAG, "echo %d bytes back", r);
        }

        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
    }
}
