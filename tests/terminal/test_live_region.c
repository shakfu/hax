/* SPDX-License-Identifier: MIT */
#include "buf.h"
#include "harness.h"
#include "terminal/ansi.h"
#include "terminal/live_region.h"

/* Record a frame of blank rows with the given widths, discarding its bytes. */
static void paint_rows(struct live_region *region, const int *widths, int count, int cursor_row,
                       int cursor_col)
{
    struct buf scratch;
    buf_init(&scratch);
    live_region_rewind(region, &scratch, 0);
    for (int row = 0; row < count; row++)
        live_region_add_row(region, &scratch, "", 0, widths[row]);
    live_region_end_frame(region, &scratch, cursor_row, cursor_col);
    buf_free(&scratch);
}

static void expect_rewind(struct live_region *region, int terminal_cols, const char *expected)
{
    struct buf out;
    buf_init(&out);
    live_region_rewind(region, &out, terminal_cols);
    EXPECT_STR_EQ(out.data, expected);
    buf_free(&out);
}

static void test_frame_paints_rows_and_places_cursor(void)
{
    struct live_region region = {0};
    struct buf out;
    buf_init(&out);

    live_region_rewind(&region, &out, 80);
    live_region_add_row(&region, &out, "ab", 2, 2);
    live_region_add_row(&region, &out, "cde", 3, 3);
    live_region_end_frame(&region, &out, 0, 1);

    EXPECT_STR_EQ(out.data, "\r"
                            "ab" ANSI_ERASE_LINE "\r\n"
                            "cde" ANSI_ERASE_BELOW "\x1b[1A\r\x1b[1C");
    EXPECT(region.row_count == 2);
    EXPECT(region.row_widths[0] == 2);
    EXPECT(region.row_widths[1] == 3);
    buf_free(&out);
    live_region_free(&region);
}

static void test_frame_records_rows_past_initial_capacity(void)
{
    struct live_region region = {0};
    int widths[20];
    for (int row = 0; row < 20; row++)
        widths[row] = row + 1;
    /* An out-of-range cursor row rests on the last row. */
    paint_rows(&region, widths, 20, 99, 0);

    EXPECT(region.row_count == 20);
    EXPECT(region.row_widths[0] == 1);
    EXPECT(region.row_widths[19] == 20);
    EXPECT(region.cursor_row == 19);
    expect_rewind(&region, 80, "\x1b[19A\r");
    live_region_free(&region);
}

static void test_rewind_climbs_from_cursor_row(void)
{
    struct live_region region = {0};
    int widths[] = {5, 5, 5};
    paint_rows(&region, widths, 3, 2, 0);
    expect_rewind(&region, 80, "\x1b[2A\r");

    paint_rows(&region, widths, 3, 1, 4);
    expect_rewind(&region, 80, "\x1b[1A\r");
    live_region_free(&region);
}

static void test_rewind_counts_rewrapped_rows_above_cursor(void)
{
    struct live_region region = {0};
    /* The 100-cell row rewraps to three rows at 40 columns. */
    int widths[] = {100, 10};
    paint_rows(&region, widths, 2, 1, 0);
    expect_rewind(&region, 40, "\x1b[3A\r");
    live_region_free(&region);
}

/* Over-climbing would overwrite settled output above the region. */
static void test_rewind_ignores_rewrap_below_cursor(void)
{
    struct live_region region = {0};
    int widths[] = {100, 90};
    paint_rows(&region, widths, 2, 1, 0);
    expect_rewind(&region, 40, "\x1b[3A\r");

    int solo[] = {100};
    paint_rows(&region, solo, 1, 0, 0);
    expect_rewind(&region, 40, "\r");
    live_region_free(&region);
}

static void test_rewind_follows_cursor_into_rewrapped_row(void)
{
    struct live_region region = {0};
    int widths[] = {50};
    paint_rows(&region, widths, 1, 0, 45);
    expect_rewind(&region, 40, "\x1b[1A\r");
    live_region_free(&region);
}

static void test_rewind_keeps_cursor_after_row_end_on_its_row(void)
{
    struct live_region region = {0};
    /* Narrowed to exactly the row's width, the cursor after it stays in the row's final cell. */
    int widths[] = {5, 12};
    paint_rows(&region, widths, 2, 1, 12);
    expect_rewind(&region, 12, "\x1b[1A\r");

    int rewrapped[] = {24};
    paint_rows(&region, rewrapped, 1, 0, 24);
    expect_rewind(&region, 12, "\x1b[1A\r");
    live_region_free(&region);
}

static void test_unknown_width_assumes_no_rewrap(void)
{
    struct live_region region = {0};
    int widths[] = {100, 10};
    paint_rows(&region, widths, 2, 1, 0);
    expect_rewind(&region, 0, "\x1b[1A\r");
    live_region_free(&region);
}

static void test_erase_clears_each_screen_row(void)
{
    struct live_region region = {0};
    struct buf out;
    buf_init(&out);
    int widths[] = {100, 10};
    paint_rows(&region, widths, 2, 1, 0);

    /* The return is to the saved top, not a climb by the row count: after a height shrink cut
     * off rows below the cursor, the cursor-downs stop short at the screen bottom. */
    live_region_erase(&region, &out, 40);
    EXPECT_STR_EQ(out.data,
                  "\x1b[3A\r" ANSI_CURSOR_SAVE ANSI_ERASE_LINE "\x1b[1B" ANSI_ERASE_LINE
                  "\x1b[1B" ANSI_ERASE_LINE "\x1b[1B" ANSI_ERASE_LINE ANSI_CURSOR_RESTORE);
    expect_rewind(&region, 40, "\r");

    buf_reset(&out);
    int single[] = {5};
    paint_rows(&region, single, 1, 0, 3);
    live_region_erase(&region, &out, 40);
    EXPECT_STR_EQ(out.data, "\r" ANSI_CURSOR_SAVE ANSI_ERASE_LINE ANSI_CURSOR_RESTORE);

    buf_reset(&out);
    live_region_erase(&region, &out, 40);
    EXPECT(out.len == 0);
    buf_free(&out);
    live_region_free(&region);
}

static void test_leave_moves_to_last_screen_row(void)
{
    struct live_region region = {0};
    struct buf out;
    buf_init(&out);
    int widths[] = {5, 5, 5};
    paint_rows(&region, widths, 3, 0, 2);
    live_region_leave(&region, &out, 80);
    EXPECT_STR_EQ(out.data, "\x1b[2B");
    expect_rewind(&region, 80, "\r");

    buf_reset(&out);
    int rewrapped[] = {10, 100};
    paint_rows(&region, rewrapped, 2, 0, 0);
    live_region_leave(&region, &out, 40);
    EXPECT_STR_EQ(out.data, "\x1b[3B");

    buf_reset(&out);
    paint_rows(&region, widths, 3, 2, 0);
    live_region_leave(&region, &out, 80);
    EXPECT(out.len == 0);
    buf_free(&out);
    live_region_free(&region);
}

int main(void)
{
    test_frame_paints_rows_and_places_cursor();
    test_frame_records_rows_past_initial_capacity();
    test_rewind_climbs_from_cursor_row();
    test_rewind_counts_rewrapped_rows_above_cursor();
    test_rewind_ignores_rewrap_below_cursor();
    test_rewind_follows_cursor_into_rewrapped_row();
    test_rewind_keeps_cursor_after_row_end_on_its_row();
    test_unknown_width_assumes_no_rewrap();
    test_erase_clears_each_screen_row();
    test_leave_moves_to_last_screen_row();
    T_REPORT();
}
