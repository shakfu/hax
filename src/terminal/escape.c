/* SPDX-License-Identifier: MIT */
#include "terminal/escape.h"

#include <stddef.h>

enum escape_scan_state escape_scan(enum escape_scan_state state, unsigned char byte)
{
    switch (state) {
    case ESCAPE_SCAN_IDLE:
        return byte == 0x1b ? ESCAPE_SCAN_START : ESCAPE_SCAN_IDLE;
    case ESCAPE_SCAN_START:
        if (byte == '[')
            return ESCAPE_SCAN_CSI;
        if (byte == ']')
            return ESCAPE_SCAN_OSC;
        return ESCAPE_SCAN_IDLE;
    case ESCAPE_SCAN_CSI:
        return byte >= 0x40 && byte <= 0x7e ? ESCAPE_SCAN_IDLE : ESCAPE_SCAN_CSI;
    case ESCAPE_SCAN_OSC:
    case ESCAPE_SCAN_OSC_ESC:
        if (byte == '\a' || (state == ESCAPE_SCAN_OSC_ESC && byte == '\\'))
            return ESCAPE_SCAN_IDLE;
        return byte == 0x1b ? ESCAPE_SCAN_OSC_ESC : ESCAPE_SCAN_OSC;
    }
    return ESCAPE_SCAN_IDLE;
}

size_t escape_sequence_len(const char *bytes, size_t len)
{
    enum escape_scan_state state = ESCAPE_SCAN_START;
    for (size_t i = 1; i < len; i++) {
        state = escape_scan(state, (unsigned char)bytes[i]);
        if (state == ESCAPE_SCAN_IDLE)
            return i + 1;
    }
    return len;
}
