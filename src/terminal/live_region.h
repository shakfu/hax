/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_LIVE_REGION_H
#define HAX_TERMINAL_LIVE_REGION_H

#include <stddef.h>

struct buf;

/* Rows repainted in place, from column 0 of the region's first row downward. The region records
 * the cell widths the last frame painted and where it left the cursor, so later output can climb
 * back to the region's top even after the terminal has rewrapped those rows to a new width. Climbs
 * assume xterm-style rewrapping; a terminal that truncates rows on resize may keep stale ones.
 *
 * Builders append to a caller-owned buffer and never touch the terminal; the caller writes the
 * buffer and owns any synchronized-output bracket around it. Rows must fit the terminal when
 * painted: a row that wraps on its own breaks the climb. terminal_cols <= 0 means the width is
 * unknown and treats every row as unwrapped. A zero-initialized region has nothing painted. */
struct live_region {
    int *row_widths; /* owned; painted cells per row */
    int row_count;
    int row_capacity;
    int cursor_row;
    int cursor_col; /* cell the cursor rests on within cursor_row */
};

void live_region_free(struct live_region *region);

/* Climb from where the last frame left the cursor to column 0 of the region's top row and discard
 * the recorded rows. Either paint a new frame with live_region_add_row() and
 * live_region_end_frame(), or overwrite the region with output that ends with ANSI_ERASE_BELOW to
 * clear the rows it leaves uncovered. */
void live_region_rewind(struct live_region *region, struct buf *out, int terminal_cols);

/* Paint the frame's next row, `cells` wide, without a line ending or erase. */
void live_region_add_row(struct live_region *region, struct buf *out, const char *bytes, size_t len,
                         int cells);

/* Erase the previous frame's leftovers after the last row, then rest the cursor on cell
 * cursor_col of row cursor_row. Output appended later must not move the cursor off that row. */
void live_region_end_frame(struct live_region *region, struct buf *out, int cursor_row,
                           int cursor_col);

/* Erase every painted row and leave the cursor at column 0 of the region's top row, with the
 * region empty. */
void live_region_erase(struct live_region *region, struct buf *out, int terminal_cols);

/* Move the cursor down to the region's last row, keeping its column, and give up the region with
 * its rows left on screen. */
void live_region_leave(struct live_region *region, struct buf *out, int terminal_cols);

/* Give up the region without output, for when something else has already cleared it. */
void live_region_forget(struct live_region *region);

#endif /* HAX_TERMINAL_LIVE_REGION_H */
