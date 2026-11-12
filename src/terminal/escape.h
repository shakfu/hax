/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_ESCAPE_H
#define HAX_TERMINAL_ESCAPE_H

#include <stddef.h>

/* Framing of terminal escape sequences: a CSI runs through its final byte, an OSC through its BEL
 * or ST terminator, and any other sequence is ESC and the one byte after it. */

/* Position within an escape sequence fed one byte at a time. Zero is idle, between sequences. */
enum escape_scan_state {
    ESCAPE_SCAN_IDLE = 0,
    ESCAPE_SCAN_START, /* after ESC */
    ESCAPE_SCAN_CSI,
    ESCAPE_SCAN_OSC,
    ESCAPE_SCAN_OSC_ESC, /* after an ESC that may begin ST */
};

/* Step over one byte. From idle, ESC starts a sequence and any other byte stays idle; within a
 * sequence, the byte that completes it returns to idle. */
enum escape_scan_state escape_scan(enum escape_scan_state state, unsigned char byte);

/* Byte length of the escape sequence at bytes[0], which must be ESC. An unterminated sequence
 * extends to len, so its payload is never mistaken for text. */
size_t escape_sequence_len(const char *bytes, size_t len);

#endif /* HAX_TERMINAL_ESCAPE_H */
