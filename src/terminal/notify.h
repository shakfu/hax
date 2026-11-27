/* SPDX-License-Identifier: MIT */
#ifndef HAX_TERMINAL_NOTIFY_H
#define HAX_TERMINAL_NOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

/* Emit and flush the configured terminal-attention notification on stdout. Auto mode selects OSC 9
 * only for known supporting terminals and otherwise uses BEL. No-op when output is not a terminal
 * or notifications are disabled. */
void notify_attention(void);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TERMINAL_NOTIFY_H */
