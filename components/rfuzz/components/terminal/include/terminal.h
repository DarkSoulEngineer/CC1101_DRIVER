#ifndef SERIAL_CONSOLE_H
#define SERIAL_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"

typedef enum {
    RFUZZ_CMD_NONE, RFUZZ_CMD_HELP, RFUZZ_CMD_STATUS,
    RFUZZ_CMD_PACKET, RFUZZ_CMD_RAW, RFUZZ_CMD_UNKNOWN, RFUZZ_CMD_TOO_LONG,
    RFUZZ_CMD_MOD_OOK, RFUZZ_CMD_MOD_ASK,
    RFUZZ_CMD_MOD_2FSK, RFUZZ_CMD_MOD_GFSK
} rfuzz_command_t;

typedef struct {
    char line[48];
    size_t length;
    bool discard;
    bool after_cr;
} rfuzz_console_t;

esp_err_t rfuzz_console_init(void);
rfuzz_command_t rfuzz_console_poll(void);

static inline bool rfuzz_console_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

static inline rfuzz_command_t rfuzz_console_parse_line(char *line)
{
    char *tokens[2];
    size_t count = 0;
    char *p = line;
    while (*p) {
        while (rfuzz_console_space((unsigned char)*p)) p++;
        if (!*p) break;
        if (count == 2) return RFUZZ_CMD_UNKNOWN;
        tokens[count++] = p;
        while (*p && !rfuzz_console_space((unsigned char)*p)) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + ('a' - 'A'));
            p++;
        }
        if (*p) *p++ = '\0';
    }
    if (!count) return RFUZZ_CMD_NONE;
    if (count == 1) {
        if (!strcmp(tokens[0], "help")) return RFUZZ_CMD_HELP;
        if (!strcmp(tokens[0], "status")) return RFUZZ_CMD_STATUS;
        return RFUZZ_CMD_UNKNOWN;
    }
    if (!strcmp(tokens[0], "rx") || !strcmp(tokens[0], "mode")) {
        if (!strcmp(tokens[1], "packet") ||
            (!strcmp(tokens[0], "mode") && !strcmp(tokens[1], "pck")))
            return RFUZZ_CMD_PACKET;
        if (!strcmp(tokens[1], "raw")) return RFUZZ_CMD_RAW;
    } else if (!strcmp(tokens[0], "mod")) {
        if (!strcmp(tokens[1], "ook")) return RFUZZ_CMD_MOD_OOK;
        if (!strcmp(tokens[1], "ask")) return RFUZZ_CMD_MOD_ASK;
        if (!strcmp(tokens[1], "2fsk") || !strcmp(tokens[1], "fsk"))
            return RFUZZ_CMD_MOD_2FSK;
        if (!strcmp(tokens[1], "gfsk")) return RFUZZ_CMD_MOD_GFSK;
    }
    return RFUZZ_CMD_UNKNOWN;
}

static inline rfuzz_command_t rfuzz_console_feed(rfuzz_console_t *parser, unsigned char c)
{
    if (c == '\n' && parser->after_cr) {
        parser->after_cr = false;
        return RFUZZ_CMD_NONE;
    }
    parser->after_cr = c == '\r';
    if (c == '\r' || c == '\n') {
        bool discarded = parser->discard;
        parser->line[parser->length] = '\0';
        parser->length = 0;
        parser->discard = false;
        if (discarded) return RFUZZ_CMD_TOO_LONG;
        return rfuzz_console_parse_line(parser->line);
    }
    if (!parser->discard) {
        if (((c < 32 || c > 126) && !rfuzz_console_space(c)) ||
            parser->length == sizeof(parser->line) - 1)
            parser->discard = true;
        else parser->line[parser->length++] = (char)c;
    }
    return RFUZZ_CMD_NONE;
}
#endif
