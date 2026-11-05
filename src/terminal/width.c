/* SPDX-License-Identifier: MIT */
#include "terminal/width.h"

#include <stddef.h>
#include <strings.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "buf.h"
#include "config.h"
#include "terminal/ansi.h"
#include "terminal/escape.h"
#include "text/utf8.h"

int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 100;
}

int term_height(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24;
}

int auto_display_width(int terminal_width)
{
    int width = terminal_width;
    if (width > DISPLAY_WIDTH_CAP + DISPLAY_WIDTH_TOLERANCE)
        width = DISPLAY_WIDTH_CAP;
    return width < 20 ? 20 : width;
}

int display_width(void)
{
    const char *mode = config_str("display_width");
    int configured_width = config_int("display_width");
    if (configured_width >= 20)
        return configured_width;

    int width = term_width();
    if (!mode || strcasecmp(mode, "terminal") != 0)
        return auto_display_width(width);
    return width < 20 ? 20 : width;
}

int reflow_physical_rows(const int *row_widths, int count, int terminal_cols)
{
    int physical_rows = 0;
    for (int row = 0; row < count; row++) {
        int width = row_widths[row];
        physical_rows += width > 0 ? (width + terminal_cols - 1) / terminal_cols : 1;
    }
    return physical_rows;
}

int clip_styled_row(struct buf *out, const char *row, size_t len, int max_cells, int use_utf8)
{
    int cells = 0;
    size_t offset = 0;
    while (offset < len) {
        if ((unsigned char)row[offset] == 0x1b) {
            size_t escape_len = escape_sequence_len(row + offset, len - offset);
            buf_append(out, row + offset, escape_len);
            offset += escape_len;
            continue;
        }

        size_t bytes;
        int width = utf8_codepoint_cells(row, len, offset, &bytes);
        int codepoint_cells = width < 0 ? 1 : width;
        if (cells + codepoint_cells > max_cells) {
            if (cells < max_cells) {
                buf_append_str(out, use_utf8 ? "\xe2\x80\xa6" : ".");
                cells++;
            }
            buf_append_str(out, ANSI_RESET);
            return cells;
        }
        if (width < 0)
            buf_append(out, "?", 1);
        else
            buf_append(out, row + offset, bytes ? bytes : 1);
        cells += codepoint_cells;
        offset += bytes ? bytes : 1;
    }
    return cells;
}
