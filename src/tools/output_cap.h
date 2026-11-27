/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_OUTPUT_CAP_H
#define HAX_TOOLS_OUTPUT_CAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tool-result limits applied before output is sent to the model. */
size_t output_cap_bytes(void);
#define OUTPUT_CAP_LINES      2000
#define OUTPUT_CAP_LINE_WIDTH 500
/* A truncated result keeps 1/OUTPUT_CAP_HEAD_DIVISOR of its budget from its start, for the
 * continuation point or a first error, and the rest from its end, where results, errors, and exit
 * status land. */
#define OUTPUT_CAP_HEAD_DIVISOR 8

/* Replace each line suffix beyond max_line_bytes with an elision marker while preserving newline
 * structure. Returns a newly allocated NUL-terminated buffer and stores its length in out_len. */
char *cap_line_lengths(const char *data, size_t length, size_t max_line_bytes, size_t *out_len);

/* Line alignment for a truncated result's head and tail, over bytes already in memory. */

/* The end of the leading whole lines of data[0, length), at most `max_lines` of them: the offset
 * just past the last newline kept, or 0 when the first line does not end within `length`.
 * `lines_out` (may be NULL) receives the count kept. */
size_t whole_lines_head(const char *data, size_t length, size_t max_lines, size_t *lines_out);

/* The start of the trailing whole lines of data[0, length), at most `max_lines` of them, where
 * `mid_line` says the bytes before `data` do not end in a newline. A partial first line is skipped
 * unless that would leave nothing, so a single long line is kept as raw bytes and returns 0.
 * `lines_out` (may be NULL) receives the count kept, a final unterminated line included. */
size_t whole_lines_tail(const char *data, size_t length, int mid_line, size_t max_lines,
                        size_t *lines_out);

/* Where a head of at most `budget` bytes of `text` ends: after whole lines, or at a character
 * boundary when its first line is longer. */
size_t output_cap_head_end(const char *text, size_t budget);

/* Where a tail of at most `budget` of the `length` > `budget` bytes of `text` starts: at whole
 * lines, or at a character boundary when its last line is longer. */
size_t output_cap_tail_start(const char *text, size_t length, size_t budget);

/* Text a host supplied for a tool result, held to `budget` bytes: unchanged when it fits,
 * otherwise its head and tail around a truncation marker, split as the built-in tools split
 * theirs. Returns an owned copy. Built-in results are already capped and must not pass through
 * this, since their own markers would be cut. */
char *output_cap_clip(const char *text, size_t budget);

/* Visible separator between read-tool line numbers and content. Unlike whitespace, it cannot be
 * mistaken for indentation when a model copies text into an edit request. */
#define READ_LINE_DELIM "\xE2\x86\x92"

#ifdef __cplusplus
}
#endif

#endif /* HAX_TOOLS_OUTPUT_CAP_H */
