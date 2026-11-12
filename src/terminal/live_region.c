/* SPDX-License-Identifier: MIT */
#include "terminal/live_region.h"

#include <stdio.h>
#include <stdlib.h>

#include "buf.h"
#include "xalloc.h"
#include "terminal/ansi.h"
#include "terminal/width.h"

static void append_csi(struct buf *out, int count, char final)
{
    if (count <= 0)
        return;
    char sequence[24];
    snprintf(sequence, sizeof(sequence), ANSI_CSI "%d%c", count, final);
    buf_append_str(out, sequence);
}

static int screen_rows(const int *row_widths, int count, int terminal_cols)
{
    return terminal_cols > 0 ? reflow_physical_rows(row_widths, count, terminal_cols) : count;
}

/* Screen rows from the start of the cursor's row to the cursor. A cursor at or past the end of the
 * row's content stays with that content: when a rewrap exactly fills the content's last screen
 * row, the cursor stays in its final cell instead of moving to a row of its own. */
static int cursor_offset(const struct live_region *region, int terminal_cols)
{
    if (terminal_cols <= 0)
        return 0;
    int width = region->row_widths[region->cursor_row];
    if (region->cursor_col >= width && width > 0)
        return (width - 1) / terminal_cols;
    return region->cursor_col / terminal_cols;
}

/* Physical rows between the region's top and the cursor's row, under the current width. */
static int cursor_climb(const struct live_region *region, int terminal_cols)
{
    if (region->row_count == 0)
        return 0;
    return screen_rows(region->row_widths, region->cursor_row, terminal_cols) +
           cursor_offset(region, terminal_cols);
}

void live_region_free(struct live_region *region)
{
    free(region->row_widths);
    region->row_widths = NULL;
    region->row_capacity = 0;
    live_region_forget(region);
}

void live_region_forget(struct live_region *region)
{
    region->row_count = 0;
    region->cursor_row = 0;
    region->cursor_col = 0;
}

void live_region_rewind(struct live_region *region, struct buf *out, int terminal_cols)
{
    append_csi(out, cursor_climb(region, terminal_cols), 'A');
    buf_append_str(out, "\r");
    live_region_forget(region);
}

void live_region_add_row(struct live_region *region, struct buf *out, const char *bytes, size_t len,
                         int cells)
{
    if (region->row_count > 0)
        buf_append_str(out, ANSI_ERASE_LINE "\r\n");
    buf_append(out, bytes, len);

    if (region->row_count == region->row_capacity) {
        region->row_capacity = region->row_capacity ? region->row_capacity * 2 : 8;
        region->row_widths =
            xrealloc(region->row_widths, (size_t)region->row_capacity * sizeof(int));
    }
    region->row_widths[region->row_count++] = cells;
}

void live_region_end_frame(struct live_region *region, struct buf *out, int cursor_row,
                           int cursor_col)
{
    buf_append_str(out, ANSI_ERASE_BELOW);

    int last_row = region->row_count > 0 ? region->row_count - 1 : 0;
    if (cursor_row < 0 || cursor_row > last_row)
        cursor_row = last_row;
    if (cursor_col < 0)
        cursor_col = 0;
    append_csi(out, last_row - cursor_row, 'A');
    buf_append_str(out, "\r");
    append_csi(out, cursor_col, 'C');
    region->cursor_row = cursor_row;
    region->cursor_col = cursor_col;
}

/* Erase line by line: erase-below from the top of the screen can push a stale screen into
 * scrollback in tmux and some terminals, and its screen-wide damage makes multiplexers repaint far
 * more than the region. Cursor-down clamps at the bottom instead of scrolling, so the saved
 * position stays valid; climbing back by the row count would overshoot the top once a height shrink
 * has cut off rows below the cursor. */
void live_region_erase(struct live_region *region, struct buf *out, int terminal_cols)
{
    if (region->row_count == 0)
        return;
    int rows = screen_rows(region->row_widths, region->row_count, terminal_cols);
    live_region_rewind(region, out, terminal_cols);
    buf_append_str(out, ANSI_CURSOR_SAVE);
    for (int row = 0; row < rows; row++) {
        buf_append_str(out, ANSI_ERASE_LINE);
        if (row + 1 < rows)
            append_csi(out, 1, 'B');
    }
    buf_append_str(out, ANSI_CURSOR_RESTORE);
}

void live_region_leave(struct live_region *region, struct buf *out, int terminal_cols)
{
    if (region->row_count > 0) {
        int rows = screen_rows(region->row_widths, region->row_count, terminal_cols);
        append_csi(out, rows - 1 - cursor_climb(region, terminal_cols), 'B');
    }
    live_region_forget(region);
}
