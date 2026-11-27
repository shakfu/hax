/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_SHELL_H
#define HAX_TOOLS_BASH_SHELL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve bash.shell → PATH bash → /bin/bash → /bin/sh; return a malloc'd path. */
char *bash_resolve_shell(void);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TOOLS_BASH_SHELL_H */
