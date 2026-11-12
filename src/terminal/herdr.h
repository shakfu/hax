/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_HERDR_H
#define HAX_TERMINAL_HERDR_H

/* Agent state reports to herdr (https://herdr.dev), the terminal workspace manager. Each call runs
 * `$HERDR_BIN_PATH pane ...` detached and ignores its outcome. Calls are no-ops outside a herdr
 * pane: HERDR_ENV must be 1 and HERDR_PANE_ID, HERDR_BIN_PATH, and HERDR_SOCKET_PATH set. */

enum herdr_state {
    HERDR_IDLE,
    HERDR_WORKING,
};

/* Report `state` for this pane. A non-NULL `session_id` names a resumable session, which herdr
 * reopens with `hax --resume=<id>` after a restart. Repeating the last report sends nothing. */
void herdr_report(enum herdr_state state, const char *session_id);

/* Clear hax's state and resume command from the pane, on quit. */
void herdr_release(void);

#endif /* HAX_TERMINAL_HERDR_H */
