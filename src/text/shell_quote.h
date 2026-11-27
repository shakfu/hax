/* SPDX-License-Identifier: MIT */
#ifndef HAX_TEXT_SHELL_QUOTE_H
#define HAX_TEXT_SHELL_QUOTE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return a newly allocated shell-safe, single-quoted copy. NULL becomes empty. */
char *shell_single_quote(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TEXT_SHELL_QUOTE_H */
