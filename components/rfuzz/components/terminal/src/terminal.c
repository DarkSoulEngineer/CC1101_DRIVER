#include "terminal.h"
#include <fcntl.h>
#include <unistd.h>

static rfuzz_console_t s_parser;

esp_err_t rfuzz_console_init(void)
{
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
        return ESP_FAIL;
    return ESP_OK;
}

rfuzz_command_t rfuzz_console_poll(void)
{
    /* Sole stdin reader: bounded bytes and at most one command per poll. */
    for (unsigned i = 0; i < 64; i++) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) break;
        rfuzz_command_t command = rfuzz_console_feed(&s_parser, c);
        if (command != RFUZZ_CMD_NONE) return command;
    }
    return RFUZZ_CMD_NONE;
}
