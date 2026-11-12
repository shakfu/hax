/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "render/disp.h"
#include "system/locale.h"
#include "terminal/sgr.h"

struct capture {
    struct disp disp;
    FILE *stream;
    char *bytes;
    size_t len;
};

static int capture_init(struct capture *capture, size_t committed_newlines)
{
    memset(capture, 0, sizeof(*capture));
    capture->stream = open_memstream(&capture->bytes, &capture->len);
    EXPECT(capture->stream != NULL);
    if (!capture->stream)
        return 0;
    capture->disp.sink = capture->stream;
    capture->disp.committed_newlines = committed_newlines;
    return 1;
}

static const char *capture_read(struct capture *capture)
{
    disp_flush(&capture->disp);
    return capture->bytes ? capture->bytes : "";
}

static void capture_free(struct capture *capture)
{
    disp_free(&capture->disp);
    fclose(capture->stream);
    free(capture->bytes);
}

static void test_putc_writes_visible_bytes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_putc(&capture.disp, 'h');
    disp_putc(&capture.disp, 'i');

    EXPECT_STR_EQ(capture_read(&capture), "hi");
    EXPECT(capture.disp.committed_newlines == 0);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_putc_defers_trailing_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_putc(&capture.disp, 'a');
    disp_putc(&capture.disp, '\n');
    disp_putc(&capture.disp, '\n');

    EXPECT_STR_EQ(capture_read(&capture), "a");
    EXPECT(capture.disp.pending_newlines == 2);

    disp_commit_newlines(&capture.disp);
    EXPECT_STR_EQ(capture_read(&capture), "a\n\n");
    EXPECT(capture.disp.pending_newlines == 0);
    EXPECT(capture.disp.committed_newlines == 2);
    capture_free(&capture);
}

static void test_visible_byte_commits_pending_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "a\n", 2);
    disp_putc(&capture.disp, 'b');

    EXPECT_STR_EQ(capture_read(&capture), "a\nb");
    EXPECT(capture.disp.committed_newlines == 0);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_write_defers_trailing_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\n\n", 4);

    EXPECT_STR_EQ(capture_read(&capture), "hi");
    EXPECT(capture.disp.pending_newlines == 2);
    capture_free(&capture);
}

static void test_write_normalizes_trailing_crlf(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\r\n", 4);
    disp_commit_newlines(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "hi\n");
    capture_free(&capture);
}

static void test_write_preserves_trailing_carriage_return(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "hi\r", 3);

    EXPECT_STR_EQ(capture_read(&capture), "hi\r");
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_putc_carriage_return_preserves_newline_state(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "row\n", 4);
    disp_putc(&capture.disp, '\r');
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "row\n\r\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_write_carriage_return_preserves_newline_state(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "row\n\r", 5);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "row\n\r\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_empty_write_is_noop(void)
{
    struct capture capture;
    if (!capture_init(&capture, 1))
        return;

    disp_write(&capture.disp, "", 0);

    EXPECT_STR_EQ(capture_read(&capture), "");
    EXPECT(capture.disp.committed_newlines == 1);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_starts_with_blank_line(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_collapses_pending_newlines(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "\n\n\n\n\n", 5);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_block_separator_preserves_existing_blank_line(void)
{
    struct capture capture;
    if (!capture_init(&capture, 2))
        return;

    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "");
    EXPECT(capture.disp.committed_newlines == 2);
    capture_free(&capture);
}

static void test_printf_uses_display_newline_handling(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_printf(&capture.disp, "n=%d %s\n", 42, "rows");

    EXPECT_STR_EQ(capture_read(&capture), "n=42 rows");
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_ansi_write_does_not_commit_pending_newline(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "body\n", 5);
    disp_write_ansi(&capture.disp, "\x1b[1m");

    EXPECT_STR_EQ(capture_read(&capture), "body\x1b[1m");
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_external_line_resynchronizes_separator(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    capture.disp.pending_newlines = 3;
    fputs("external\n", capture.stream);
    disp_sync_external_line(&capture.disp);
    disp_block_separator(&capture.disp);

    EXPECT_STR_EQ(capture_read(&capture), "external\n\n");
    EXPECT(capture.disp.committed_newlines == 2);
    EXPECT(capture.disp.pending_newlines == 0);
    capture_free(&capture);
}

static void test_column_counts_cells_not_escapes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    /* A wide and an accented codepoint around an SGR run. */
    const char *styled = "a\xe7\x95\x8c\x1b[1m\xc3\xa9\x1b[0m";
    disp_write(&capture.disp, styled, strlen(styled));
    disp_write_ansi(&capture.disp, "\x1b[2m");
    disp_putc(&capture.disp, 'c');
    EXPECT(capture.disp.column == 5);

    disp_write(&capture.disp, "\t", 1);
    EXPECT(capture.disp.column == 8);
    capture_free(&capture);
}

static void test_column_follows_the_cursor_row(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "first\nrow", 9);
    EXPECT(capture.disp.column == 3);
    disp_write(&capture.disp, "ab\r", 3);
    EXPECT(capture.disp.column == 0);

    /* A pending newline is not written yet, so the cursor is still on the row. */
    disp_write(&capture.disp, "text\n", 5);
    EXPECT(capture.disp.column == 4);
    disp_commit_newlines(&capture.disp);
    EXPECT(capture.disp.column == 0);

    disp_write(&capture.disp, "x", 1);
    disp_block_separator(&capture.disp);
    EXPECT(capture.disp.column == 0);

    disp_write(&capture.disp, "y", 1);
    fputs("external\n", capture.stream);
    disp_sync_external_line(&capture.disp);
    EXPECT(capture.disp.column == 0);
    capture_free(&capture);
}

static void write_bytewise(struct disp *disp, const char *bytes)
{
    for (size_t i = 0; bytes[i]; i++)
        disp_write(disp, bytes + i, 1);
}

static void test_column_survives_split_writes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    write_bytewise(&capture.disp, "\xe7\x95\x8c");
    EXPECT(capture.disp.column == 2);
    write_bytewise(&capture.disp, "\x1b[31mX");
    EXPECT(capture.disp.column == 3);
    capture_free(&capture);
}

static void test_new_row_drops_partial_sequences(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "\xe7\x95\n", 3);
    disp_write(&capture.disp, "a", 1);
    EXPECT(capture.disp.column == 1);

    const char *unterminated = "\x1b]8;;unterminated\n";
    disp_write(&capture.disp, unterminated, strlen(unterminated));
    disp_write(&capture.disp, "b", 1);
    EXPECT(capture.disp.column == 1);
    capture_free(&capture);
}

struct live_calls {
    int suspends;
    int resumes;
    int declines; /* resumes still to decline */
};

static void count_suspend(void *user)
{
    ((struct live_calls *)user)->suspends++;
}

static int count_resume(void *user)
{
    struct live_calls *calls = user;
    calls->resumes++;
    if (calls->declines == 0)
        return 1;
    calls->declines--;
    return 0;
}

static void write_repeated(struct disp *disp, char byte, size_t count)
{
    for (size_t i = 0; i < count; i++)
        disp_write(disp, &byte, 1);
}

static void test_screen_column_wraps_like_a_terminal(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    write_repeated(&capture.disp, 'A', 55);
    EXPECT(disp_screen_column(&capture.disp, 80) == 55);
    EXPECT(disp_screen_column(&capture.disp, 40) == 15);

    /* A wide character that would straddle the edge starts the next row, one cell later. */
    write_repeated(&capture.disp, 'A', 24);
    disp_write(&capture.disp, "\xe7\x95\x8c", 3);
    EXPECT(disp_screen_column(&capture.disp, 80) == 2);
    EXPECT(disp_screen_column(&capture.disp, 40) == 2);

    disp_write(&capture.disp, "\n", 1);
    disp_commit_newlines(&capture.disp);
    write_repeated(&capture.disp, 'A', 80);
    EXPECT(disp_screen_column(&capture.disp, 80) == 80);
    EXPECT(disp_screen_column(&capture.disp, 0) == 80);

    /* A new row forgets the wide characters before it. */
    disp_write(&capture.disp, "\xe7\x95\x8c\n", 4);
    disp_commit_newlines(&capture.disp);
    write_repeated(&capture.disp, 'A', 79);
    EXPECT(disp_screen_column(&capture.disp, 80) == 79);

    /* 39 wide characters fill each 79-column row, each row leaving its last cell blank. */
    disp_write(&capture.disp, "\n", 1);
    disp_commit_newlines(&capture.disp);
    for (int i = 0; i < 200; i++)
        disp_write(&capture.disp, "\xe7\x95\x8c", 3);
    EXPECT(disp_screen_column(&capture.disp, 79) == 10);
    capture_free(&capture);
}

static void test_screen_column_holds_off_on_uncertain_rows(void)
{
    /* U+26A0 U+FE0F, U+1F44D U+1F3FD, U+1F468 U+200D U+1F4BB, a flag, then bytes of no width. */
    const char *uncertain[] = {
        "\xe2\x9a\xa0\xef\xb8\x8f",
        "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd",
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x92\xbb",
        "\xf0\x9f\x87\xba\xf0\x9f\x87\xa6",
        "\t",
        "\a",
        "\xff",
    };
    for (size_t i = 0; i < sizeof(uncertain) / sizeof(uncertain[0]); i++) {
        struct capture capture;
        if (!capture_init(&capture, 0))
            return;
        disp_write(&capture.disp, "ab", 2);
        disp_write(&capture.disp, uncertain[i], strlen(uncertain[i]));
        disp_write(&capture.disp, "cd", 2);
        EXPECT(disp_screen_column(&capture.disp, 80) == 80);

        disp_write(&capture.disp, "\nab", 3);
        EXPECT(disp_screen_column(&capture.disp, 80) == 2);
        capture_free(&capture);
    }
}

static void test_row_break_takes_the_next_line_ending(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "xyz", 3);
    disp_note_row_break(&capture.disp);
    EXPECT(capture.disp.column == 0);
    disp_write(&capture.disp, "\nA\n", 3);
    disp_note_row_break(&capture.disp);
    disp_commit_newlines(&capture.disp);
    disp_note_row_break(&capture.disp);
    disp_write(&capture.disp, "\r", 1);
    disp_write(&capture.disp, "\nB\r\nC", strlen("\nB\r\nC"));
    disp_note_row_break(&capture.disp);
    disp_write(&capture.disp, "D\nE", 3);
    EXPECT_STR_EQ(capture_read(&capture), "xyzA\rB\r\nCD\nE");
    capture_free(&capture);
}

static void test_mid_sequence_until_escape_and_codepoint_complete(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    disp_write(&capture.disp, "a\x1b[31", 5);
    EXPECT(disp_mid_sequence(&capture.disp));
    disp_write(&capture.disp, "m\xe7\x95", 3);
    EXPECT(disp_mid_sequence(&capture.disp));
    disp_write(&capture.disp, "\x8c", 1);
    EXPECT(!disp_mid_sequence(&capture.disp));
    capture_free(&capture);
}

static void test_declined_resume_retries_at_later_flushes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;
    struct live_calls calls = {.declines = 1};
    struct disp_live_area live = {.suspend = count_suspend, .resume = count_resume, .user = &calls};
    capture.disp.live = &live;

    /* Line endings alone stay pending, so only the flush brings the live area back. */
    const char *intro = "Intro \xe2\x9a\xa0\xef\xb8\x8f";
    disp_write(&capture.disp, intro, strlen(intro));
    disp_flush(&capture.disp);
    disp_write(&capture.disp, "\n\n", 2);
    disp_flush(&capture.disp);
    EXPECT(calls.suspends == 1);
    EXPECT(calls.resumes == 2);
    EXPECT(!capture.disp.live_suspended);
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_flush_ends_an_uncertain_row_for_the_live_area(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;
    struct live_calls calls = {0};
    struct disp_live_area live = {.suspend = count_suspend, .resume = count_resume, .user = &calls};
    capture.disp.live = &live;

    /* U+26A0 U+FE0F leaves no column to return to; the row ends early instead. */
    const char *intro = "Table \xe2\x9a\xa0\xef\xb8\x8f:\n\n";
    disp_write(&capture.disp, intro, strlen(intro));
    disp_flush(&capture.disp);
    EXPECT(disp_screen_column(&capture.disp, 80) == 0);
    disp_block_separator(&capture.disp);
    EXPECT_STR_EQ(capture_read(&capture), "Table \xe2\x9a\xa0\xef\xb8\x8f:\n\n");

    /* A certain row keeps its line endings pending. */
    disp_write(&capture.disp, "plain\n", 6);
    disp_flush(&capture.disp);
    EXPECT(capture.disp.pending_newlines == 1);
    capture_free(&capture);
}

static void test_style_tracks_sgr_across_writes(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;

    write_bytewise(&capture.disp, "\x1b[1mbold");
    disp_write_raw(&capture.disp, "\x1b[3m", 4);
    disp_write(&capture.disp, "row\nnext", 8);
    EXPECT(capture.disp.style.bold);
    EXPECT(capture.disp.style.italic);

    disp_write_ansi(&capture.disp, "\x1b[0m");
    EXPECT(sgr_is_default(&capture.disp.style));
    capture_free(&capture);
}

static void test_live_area_steps_aside_once_per_batch(void)
{
    struct capture capture;
    if (!capture_init(&capture, 0))
        return;
    struct live_calls calls = {0};
    struct disp_live_area live = {.suspend = count_suspend, .resume = count_resume, .user = &calls};
    capture.disp.live = &live;

    disp_flush(&capture.disp);
    disp_write(&capture.disp, "\n", 1);
    EXPECT(calls.suspends == 0);

    disp_write(&capture.disp, "text", 4);
    disp_write_ansi(&capture.disp, "\x1b[0m");
    disp_putc(&capture.disp, '!');
    EXPECT(calls.suspends == 1);
    EXPECT(calls.resumes == 0);
    disp_flush(&capture.disp);
    EXPECT(calls.resumes == 1);

    disp_write_raw(&capture.disp, "\x1b[2m", 4);
    EXPECT(calls.suspends == 2);
    disp_flush(&capture.disp);
    disp_flush(&capture.disp);
    EXPECT(calls.resumes == 2);
    capture_free(&capture);
}

static void test_null_sink_selects_stdout(void)
{
    struct disp disp = {0};
    EXPECT(disp_sink(&disp) == stdout);
}

int main(void)
{
    locale_init_utf8();
    test_putc_writes_visible_bytes();
    test_putc_defers_trailing_newlines();
    test_visible_byte_commits_pending_newlines();
    test_write_defers_trailing_newlines();
    test_write_normalizes_trailing_crlf();
    test_write_preserves_trailing_carriage_return();
    test_putc_carriage_return_preserves_newline_state();
    test_write_carriage_return_preserves_newline_state();
    test_empty_write_is_noop();
    test_block_separator_starts_with_blank_line();
    test_block_separator_collapses_pending_newlines();
    test_block_separator_preserves_existing_blank_line();
    test_printf_uses_display_newline_handling();
    test_ansi_write_does_not_commit_pending_newline();
    test_external_line_resynchronizes_separator();
    test_column_counts_cells_not_escapes();
    test_column_follows_the_cursor_row();
    test_column_survives_split_writes();
    test_new_row_drops_partial_sequences();
    test_screen_column_wraps_like_a_terminal();
    test_screen_column_holds_off_on_uncertain_rows();
    test_row_break_takes_the_next_line_ending();
    test_mid_sequence_until_escape_and_codepoint_complete();
    test_style_tracks_sgr_across_writes();
    test_live_area_steps_aside_once_per_batch();
    test_flush_ends_an_uncertain_row_for_the_live_area();
    test_declined_resume_retries_at_later_flushes();
    test_null_sink_selects_stdout();

    T_REPORT();
}
