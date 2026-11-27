/* SPDX-License-Identifier: MIT */
#ifndef HAX_SESSION_PICKER_H
#define HAX_SESSION_PICKER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Shows recorded sessions for cwd, newest first, excluding exclude_path when non-NULL. Returns an
 * owned selected path, or NULL on cancellation, an empty list, or a non-interactive terminal.
 * picker_opened is set when control reached the interactive picker, including cancellation and
 * terminal setup failure; /resume uses it for display bookkeeping. */
char *session_picker_run(const char *cwd, const char *exclude_path, int *picker_opened);

#ifdef __cplusplus
}
#endif

#endif /* HAX_SESSION_PICKER_H */
