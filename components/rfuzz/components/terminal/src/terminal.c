#include "terminal.h"
#include <fcntl.h>
#include <unistd.h>
#include "esp_timer.h"

static rfuzz_console_t s_parser;
static int64_t s_last_input_us = 0;

esp_err_t rfuzz_console_init(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return ESP_FAIL;
    s_last_input_us = esp_timer_get_time();
    return ESP_OK;
}

rfuzz_command_t rfuzz_console_poll(void)
{
    for (unsigned i = 0; i < 64; i++) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        s_last_input_us = esp_timer_get_time();
        /* Echo typed characters so the user sees their input. */
        if (c == '\r' || c == '\n') {
            write(STDOUT_FILENO, "\r\n", 2);
        } else if (c >= 32 && c < 127) {
            write(STDOUT_FILENO, &c, 1);
        }
        rfuzz_command_t command = rfuzz_console_feed(&s_parser, c);
        if (command != RFUZZ_CMD_NONE) return command;
    }
    return RFUZZ_CMD_NONE;
}

bool rfuzz_console_is_typing(void)
{
    return (esp_timer_get_time() - s_last_input_us) < 500000;
}
