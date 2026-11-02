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

static const char *mod_name_for_command(rfuzz_command_t command)
{
    switch (command) {
    case RFUZZ_CMD_MOD_OOK: return "OOK";
    case RFUZZ_CMD_MOD_ASK: return "ASK";
    case RFUZZ_CMD_MOD_2FSK: return "2FSK";
    case RFUZZ_CMD_MOD_GFSK: return "GFSK";
    default: return "?";
    }
}

static void rfuzz_apply_console_command(void)
{
    rfuzz_command_t command = rfuzz_console_poll();
    switch (command) {
    case RFUZZ_CMD_HELP:
        ESP_LOGI(TAG, "RFUZZ dual-radio console commands:");
        ESP_LOGI(TAG, "  help                       Show this help");
        ESP_LOGI(TAG, "  status                     Show profile, radios, and capture status");
        ESP_LOGI(TAG, "  mode packet | pck          Radio B packet RX (mode packet / rx packet)");
        ESP_LOGI(TAG, "  mode raw                   Radio B raw digital demodulator capture");
        ESP_LOGI(TAG, "  rx packet | raw            Alias for 'mode ...'");
        ESP_LOGI(TAG, "  mod ook | ask | 2fsk | gfsk  Queue coordinated A/B profile (fsk=2fsk)");
        ESP_LOGI(TAG, "  tx start | send            Transmit one packet on Radio A");
        ESP_LOGI(TAG, "  tx stop                   Idle Radio A (TX off)");
        ESP_LOGI(TAG, "  rx start                  Enable RX reporting/listening");
        ESP_LOGI(TAG, "  rx stop                   Pause RX reporting (radio stays in RX)");
        ESP_LOGI(TAG, "");
        break;
    case RFUZZ_CMD_STATUS:
        rfuzz_profile_status(&s_profile);
        break;
    case RFUZZ_CMD_PACKET:
    case RFUZZ_CMD_RAW:
        if (!rfuzz_profile_mode(&s_profile, command == RFUZZ_CMD_PACKET ? CC1101_RX_PACKET : CC1101_RX_RAW))
            ESP_LOGW(TAG, "Mode change rejected: profile invalid. Recover with 'mod ...'.");
        else
            ESP_LOGI(TAG, "Mode request queued: %s", command == RFUZZ_CMD_PACKET ? "packet" : "raw");
        break;
    case RFUZZ_CMD_MOD_OOK:
    case RFUZZ_CMD_MOD_ASK:
    case RFUZZ_CMD_MOD_2FSK:
    case RFUZZ_CMD_MOD_GFSK:
        rfuzz_profile_mod(&s_profile,
                         command == RFUZZ_CMD_MOD_OOK ? CC1101_PROFILE_OOK :
                         command == RFUZZ_CMD_MOD_ASK ? CC1101_PROFILE_ASK :
                         command == RFUZZ_CMD_MOD_2FSK ? CC1101_PROFILE_2FSK : CC1101_PROFILE_GFSK);
        ESP_LOGI(TAG, "Profile request queued: %s (TX stays disabled)", mod_name_for_command(command));
        break;
    case RFUZZ_CMD_TX_START:
        if (rfuzz_tx_send_once() == ESP_OK)
            ESP_LOGI(TAG, "TX: packet sent (Radio A)");
        else
            ESP_LOGW(TAG, "TX: send failed or busy");
        break;
    case RFUZZ_CMD_TX_STOP:
        if (xSemaphoreTake(cc1101_bus_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            rfuzz_tx_idle_locked();
            xSemaphoreGive(cc1101_bus_mutex);
            ESP_LOGI(TAG, "TX: Radio A idled");
        } else ESP_LOGW(TAG, "TX: bus busy");
        break;
    case RFUZZ_CMD_RX_START:
        rfuzz_rx_set_paused(false);
        ESP_LOGI(TAG, "RX: reporting enabled");
        break;
    case RFUZZ_CMD_RX_STOP:
        rfuzz_rx_set_paused(true);
        ESP_LOGI(TAG, "RX: reporting paused");
        break;
    case RFUZZ_CMD_UNKNOWN:
        ESP_LOGW(TAG, "Unknown command. Type 'help' for the command list.");
        break;
    case RFUZZ_CMD_TOO_LONG:
        ESP_LOGW(TAG, "Line too long or invalid; discarded.");
        break;
    default: break;
    }
}

void rfuzz_run(void)
{
    ESP_LOGI(TAG, "Starting RFuzz dual-radio console...");

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
    ESP_LOGI(TAG, "UART0 @115200 baud. Type 'help' for commands.");
    ESP_LOGI(TAG, "RX reporting: on. TX: idle (use 'tx start').");

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
