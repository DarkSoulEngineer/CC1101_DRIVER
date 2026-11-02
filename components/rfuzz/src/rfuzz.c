#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "terminal.h"
#include "profile.h"
#include "rx.h"
#include "tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "rfuzz.h"
#include "hw_init.h"

static const char *TAG = "RFUZZ";
static rfuzz_profile_t s_profile;

static void hang_on_error(const char *what)
{
    ESP_LOGE(TAG, "%s failed", what);
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}

static void rfuzz_apply_console_command(void)
{
    rfuzz_command_t command = rfuzz_console_poll();
    switch (command) {
    case RFUZZ_CMD_HELP:
        ESP_LOGI(TAG, "commands: help | status | rx packet/raw | mode pck/raw | mod ook/ask/2fsk/gfsk (fsk=2fsk); raw=digital demodulator, not IQ; no automatic TX");
        break;
    case RFUZZ_CMD_STATUS:
        rfuzz_profile_status(&s_profile);
        break;
    case RFUZZ_CMD_PACKET:
    case RFUZZ_CMD_RAW:
        if (!rfuzz_profile_mode(&s_profile, command == RFUZZ_CMD_PACKET ? CC1101_RX_PACKET : CC1101_RX_RAW))
            ESP_LOGW(TAG, "invalid profile; recover with mod before changing mode");
        break;
    case RFUZZ_CMD_MOD_OOK:
    case RFUZZ_CMD_MOD_ASK:
    case RFUZZ_CMD_MOD_2FSK:
    case RFUZZ_CMD_MOD_GFSK:
        rfuzz_profile_mod(&s_profile,
                         command == RFUZZ_CMD_MOD_OOK ? CC1101_PROFILE_OOK :
                         command == RFUZZ_CMD_MOD_ASK ? CC1101_PROFILE_ASK :
                         command == RFUZZ_CMD_MOD_2FSK ? CC1101_PROFILE_2FSK : CC1101_PROFILE_GFSK);
        ESP_LOGI(TAG, "profile request queued; TX stays disabled");
        break;
    case RFUZZ_CMD_UNKNOWN: ESP_LOGW(TAG, "unknown command; use help"); break;
    case RFUZZ_CMD_TOO_LONG: ESP_LOGW(TAG, "line too long/invalid; discarded"); break;
    default: break;
    }
}

void rfuzz_run(void)
{
    ESP_LOGI(TAG, "Starting RFuzz Dual-Radio Console...");

    if (init_hardware() != ESP_OK)
        hang_on_error("hardware init");
    if (rfuzz_tx_init() != ESP_OK)
        hang_on_error("TX module init");
    if (rfuzz_rx_init() != ESP_OK)
        hang_on_error("RX module init");

    rfuzz_profile_init(&s_profile);
    rfuzz_profile_process(&s_profile);

    if (rfuzz_console_init() != ESP_OK)
        hang_on_error("nonblocking console setup");
    ESP_LOGI(TAG, "UART0 115200: help | status | rx packet/raw | mode pck/raw | mod ook/ask/2fsk/gfsk;");

    while (1)
    {
        rfuzz_rx_raw_service();
        rfuzz_profile_check(&s_profile);
        rfuzz_apply_console_command();
        rfuzz_profile_process(&s_profile);
        rfuzz_rx_listen_poll();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
