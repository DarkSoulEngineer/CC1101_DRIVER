#ifndef RFUZZ_CONSOLE_H
#define RFUZZ_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef enum {
    RFUZZ_CMD_NONE, RFUZZ_CMD_HELP, RFUZZ_CMD_STATUS,
    RFUZZ_CMD_PACKET, RFUZZ_CMD_RAW, RFUZZ_CMD_UNKNOWN, RFUZZ_CMD_TOO_LONG
} rfuzz_command_t;

typedef struct {
    char line[48];
    size_t length;
    bool discard;
    bool after_cr;
} rfuzz_console_t;

/* Pure bounded parser: CR, LF or CRLF terminate a line. No partial commands
 * execute; oversized/binary lines are discarded through their terminator. */
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
        if (!parser->line[0]) return RFUZZ_CMD_NONE;
        if (!strcmp(parser->line, "help")) return RFUZZ_CMD_HELP;
        if (!strcmp(parser->line, "status")) return RFUZZ_CMD_STATUS;
        if (!strcmp(parser->line, "rx packet")) return RFUZZ_CMD_PACKET;
        if (!strcmp(parser->line, "rx raw")) return RFUZZ_CMD_RAW;
        return RFUZZ_CMD_UNKNOWN;
    }
    if (!parser->discard) {
        if (c < 32 || c > 126 || parser->length == sizeof(parser->line) - 1)
            parser->discard = true;
        else parser->line[parser->length++] = (char)c;
    }
    return RFUZZ_CMD_NONE;
}
#endif
