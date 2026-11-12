/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_WIDTH_H
#define HAX_TERMINAL_WIDTH_H

#include <stddef.h>

/* Physical terminal width on stdout, or 100 (matching DISPLAY_WIDTH_CAP) when stdout is not a
 * terminal or the size query fails. */
int term_width(void);

/* Physical terminal height on stdout, or 24 when stdout is not a terminal or the query fails. */
int term_height(void);

#define DISPLAY_WIDTH_CAP       100
#define DISPLAY_WIDTH_TOLERANCE 10

/* Automatic content width for terminal_width. Widths within DISPLAY_WIDTH_TOLERANCE above the cap
 * are used in full; wider values are capped. The result has a 20-column floor. */
int auto_display_width(int terminal_width);

/* Width used for content layout. "auto" uses auto_display_width(), "terminal" removes the upper
 * bound, and an integer >= 20 selects an exact width. */
int display_width(void);

/* Physical rows that previously painted rows of the given cell widths occupy after an xterm-style
 * reflow to terminal_cols. Empty rows still occupy one physical row. terminal_cols must be
 * positive. */
int reflow_physical_rows(const int *row_widths, int count, int terminal_cols);

struct buf;

/* Append row, trusted styled text without line endings, clipped to max_cells terminal cells so it
 * paints on one physical row. Escape sequences pass through at zero width, and unsafe codepoints
 * become '?'. A wide character that would straddle the limit leaves an ellipsis ('.' without
 * use_utf8) in its place, and a clipped row ends with a reset, as the cut may leave styling open.
 * Returns the cells appended, for reflow-aware repaints. */
int clip_styled_row(struct buf *out, const char *row, size_t len, int max_cells, int use_utf8);

#endif /* HAX_TERMINAL_WIDTH_H */
