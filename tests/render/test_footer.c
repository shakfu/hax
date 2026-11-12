/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "harness.h"
#include "xalloc.h"
#include "render/disp.h"
#include "render/footer.h"
#include "system/locale.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"

static char capture_buf[4096];

static void capture_init(void)
{
    locale_init_utf8();
    char *path = xasprintf("%s/stdout", t_tempdir());
    if (!freopen(path, "w+", stdout)) {
        perror("freopen");
        exit(1);
    }
    free(path);
}

static void capture_reset(void)
{
    fflush(stdout);
    if (ftruncate(fileno(stdout), 0) != 0) {
        perror("ftruncate");
        exit(1);
    }
    rewind(stdout);
}

static const char *capture_read(void)
{
    fflush(stdout);
    rewind(stdout);
    size_t length = fread(capture_buf, 1, sizeof(capture_buf) - 1, stdout);
    capture_buf[length] = '\0';
    return capture_buf;
}

/* All footer entry points must be silent no-ops on the NULL footer non-TTY runs carry. */
static void test_footer_is_null_and_silent_without_tty(void)
{
    capture_reset();
    struct footer *footer = footer_new(NULL);
    EXPECT(footer == NULL);

    footer_show(footer);
    footer_set_label(footer, "thinking", "thinking...");
    footer_request_label(footer, "reading", "reading...");
    footer_set_timer(footer, 1);
    struct footer_row row = {.bytes = "| live output", .cells = 13};
    footer_set_rows(footer, &row, 1);
    footer_hide(footer);
    footer_free(footer);
    EXPECT_STR_EQ(capture_read(), "");
}

static void test_frame_paints_rows_verbatim(void)
{
    struct buf frame;
    buf_init(&frame);
    struct live_region region = {0};
    struct footer_row rows[] = {{.bytes = "<older>", .cells = 7},
                                {.bytes = "<newest>", .cells = 8}};

    footer_build_frame(&frame, &region, rows, 2, 80);
    EXPECT_STR_EQ(frame.data, ANSI_SYNC_BEGIN ANSI_RESET
                  "\r<older>" ANSI_ERASE_LINE "\r\n<newest>" ANSI_ERASE_BELOW "\r" ANSI_SYNC_END);
    EXPECT(region.row_count == 2);
    EXPECT(region.cursor_row == 1);
    EXPECT(region.cursor_col == 0);
    live_region_free(&region);
    buf_free(&frame);
}

static void test_frame_clips_rows_wider_than_the_screen(void)
{
    struct buf frame;
    buf_init(&frame);
    struct live_region region = {0};
    /* Prepared at 80 columns, painted at 5: U+6F22 twice would straddle the kept four cells. */
    struct footer_row row = {.bytes = "ab\xe6\xbc\xa2\xe6\xbc\xa2", .cells = 6};

    footer_build_frame(&frame, &region, &row, 1, 5);
    EXPECT_STR_EQ(frame.data, ANSI_SYNC_BEGIN ANSI_RESET "\rab\xe6\xbc\xa2\x1b[0m" ANSI_ERASE_BELOW
                                                         "\r" ANSI_SYNC_END);
    EXPECT(region.row_widths[0] == 4);
    live_region_free(&region);
    buf_free(&frame);
}

static void test_rows_fit_between_the_output_and_the_status_row(void)
{
    EXPECT(footer_rows_that_fit(5, 24) == 5);
    EXPECT(footer_rows_that_fit(5, 6) == 3);
    EXPECT(footer_rows_that_fit(5, 3) == 0);
    EXPECT(footer_rows_that_fit(5, 1) == 0);
}

static void test_status_row_starts_the_next_block(void)
{
    struct disp disp = {.column = 7};
    EXPECT(footer_status_gap(&disp) == 2);

    disp = (struct disp){.committed_newlines = 1};
    EXPECT(footer_status_gap(&disp) == 1);

    disp = (struct disp){.committed_newlines = 2};
    EXPECT(footer_status_gap(&disp) == 0);
    disp.pending_newlines = 3;
    EXPECT(footer_status_gap(&disp) == 0);
}

static void test_return_lands_where_output_ends_on_screen(void)
{
    char *bytes = NULL;
    size_t len = 0;
    struct disp disp = {.sink = open_memstream(&bytes, &len)};
    struct buf out;
    buf_init(&out);

    /* The output's row wraps at the screen width, a wide character at the edge wrapping early. */
    for (int i = 0; i < 55; i++)
        disp_putc(&disp, 'A');
    EXPECT(!footer_build_return(&out, &disp, 2, 40, 40));
    EXPECT_STR_EQ(out.data, "\x1b[2A\x1b[16G");

    /* Fitting on one screen row before and after a resize, the row keeps its place. */
    buf_reset(&out);
    EXPECT(!footer_build_return(&out, &disp, 2, 80, 100));
    EXPECT_STR_EQ(out.data, "\x1b[2A\x1b[56G");

    buf_reset(&out);
    for (int i = 0; i < 24; i++)
        disp_putc(&disp, 'A');
    disp_write(&disp, "\xe7\x95\x8c", 3);
    EXPECT(!footer_build_return(&out, &disp, 2, 80, 80));
    EXPECT_STR_EQ(out.data, "\x1b[2A\x1b[3G");

    /* Rewrapped by a resize, or ending at the edge, an open row resumes on the row below. */
    buf_reset(&out);
    EXPECT(footer_build_return(&out, &disp, 2, 80, 40));
    EXPECT_STR_EQ(out.data, "\x1b[1A");
    buf_reset(&out);
    EXPECT(footer_build_return(&out, &disp, 2, 27, 27));
    EXPECT_STR_EQ(out.data, "\x1b[1A");

    buf_reset(&out);
    disp_write(&disp, "\n", 1);
    disp_commit_newlines(&disp);
    EXPECT(!footer_build_return(&out, &disp, 1, 80, 40));
    EXPECT_STR_EQ(out.data, "\x1b[1A");

    buf_free(&out);
    disp_free(&disp);
    fclose(disp.sink);
    free(bytes);
}

int main(void)
{
    capture_init();
    test_footer_is_null_and_silent_without_tty();
    test_frame_paints_rows_verbatim();
    test_frame_clips_rows_wider_than_the_screen();
    test_rows_fit_between_the_output_and_the_status_row();
    test_status_row_starts_the_next_block();
    test_return_lands_where_output_ends_on_screen();
    T_REPORT();
}
