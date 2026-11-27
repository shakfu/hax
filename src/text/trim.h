/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_TRIM_H
#define HAX_TEXT_TRIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trim a NUL-terminated user prompt in place to what is worth sending: leading blank lines and all
 * trailing whitespace go, while the first content line keeps its indentation and the interior
 * stays as written. A whitespace-only prompt becomes empty. Returns the new length. */
size_t trim_prompt(char *text);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TEXT_TRIM_H */
