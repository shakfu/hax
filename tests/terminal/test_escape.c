/* SPDX-License-Identifier: MIT */
#include <string.h>

#include "harness.h"
#include "terminal/escape.h"

static size_t len_of(const char *bytes)
{
    return escape_sequence_len(bytes, strlen(bytes));
}

static void test_csi_ends_at_final_byte(void)
{
    EXPECT(len_of("\x1b[1mtext") == 4);
    EXPECT(len_of("\x1b[?2026htext") == 8);
    EXPECT(len_of("\x1b[38;5;244mtext") == 11);
}

static void test_osc_ends_at_either_terminator(void)
{
    EXPECT(len_of("\x1b]8;;https://x\x07text") == 15);
    EXPECT(len_of("\x1b]8;;https://x\x1b\\text") == 16);
}

static void test_other_escape_takes_one_byte(void)
{
    EXPECT(len_of("\x1b"
                  "7text") == 2);
}

static void test_unterminated_sequence_runs_to_end(void)
{
    EXPECT(len_of("\x1b") == 1);
    EXPECT(len_of("\x1b[") == 2);
    EXPECT(len_of("\x1b[38;5") == 6);
    EXPECT(len_of("\x1b]8;;https://x") == 14);
    /* An ESC inside an OSC is a terminator only when it starts ST. */
    EXPECT(len_of("\x1b]8;;x\x1b") == 7);
}

static void test_scan_completes_split_sequence(void)
{
    const char *csi = "\x1b[38;5;244m";
    enum escape_scan_state state = ESCAPE_SCAN_IDLE;
    for (size_t i = 0; csi[i + 1]; i++) {
        state = escape_scan(state, (unsigned char)csi[i]);
        EXPECT(state != ESCAPE_SCAN_IDLE);
    }
    EXPECT(escape_scan(state, 'm') == ESCAPE_SCAN_IDLE);
    EXPECT(escape_scan(ESCAPE_SCAN_IDLE, 'x') == ESCAPE_SCAN_IDLE);
}

int main(void)
{
    test_csi_ends_at_final_byte();
    test_osc_ends_at_either_terminator();
    test_other_escape_takes_one_byte();
    test_unterminated_sequence_runs_to_end();
    test_scan_completes_split_sequence();
    T_REPORT();
}
