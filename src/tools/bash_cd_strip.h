/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_CD_STRIP_H
#define HAX_TOOLS_BASH_CD_STRIP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the offset after a leading `cd <target> &&` when the target can be proven equivalent to
 * `cwd`; otherwise return 0. `cwd` must be absolute and normalized. `home` may be NULL. Syntax not
 * modeled by the conservative parser is left unchanged. */
size_t bash_strip_cd_prefix(const char *command, const char *cwd, const char *home);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TOOLS_BASH_CD_STRIP_H */
