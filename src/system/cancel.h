/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_CANCEL_H
#define HAX_SYSTEM_CANCEL_H

#include <stdatomic.h>

/* Latched cancellation, independent of how it was requested. Requests stay latched until they are
 * cleared, and abort implies pause. The flags are the whole state and each accessor is one atomic
 * operation, so producers may run on any thread.
 *
 * There is one state per process and, where a host runs several agents at once, optionally one per
 * agent. The process state is what the terminal's bare-Esc watcher and a signal handler write:
 * both speak for the whole program, and the interactive frontend is the only agent that owns the
 * terminal. An embedder running concurrent agents gives each its own state instead, so cancelling
 * one leaves its siblings running. */
struct cancel_state {
    atomic_int pause_requested;
    atomic_int abort_requested;
};

/* The state the plain cancel_* functions below act on. Never NULL. */
struct cancel_state *cancel_process_state(void);

/* Every operation against a chosen state. A NULL `state` means the process state, so a caller
 * that was handed no per-agent state still does the right thing. */
void cancel_state_request_pause(struct cancel_state *state);
int cancel_state_request_pause_once(struct cancel_state *state);
void cancel_state_request_abort(struct cancel_state *state);
int cancel_state_pause_requested(const struct cancel_state *state);
int cancel_state_abort_requested(const struct cancel_state *state);
void cancel_state_clear(struct cancel_state *state);

/* The same operations against the process state. */

/* Request a pause, escalating to abort when a pause is already latched. This is the repeated-key
 * path: the first press asks a turn to stop at a seam, the second gives up on the seam. */
void cancel_request_pause(void);

/* Latch a pause without escalating. Returns whether a pause was already latched. For producers
 * whose repeat carries no extra meaning, such as the SIGUSR1 pause signal. */
int cancel_request_pause_once(void);

/* Request an immediate abort, which also latches pause. */
void cancel_request_abort(void);

int cancel_pause_requested(void);
int cancel_abort_requested(void);

void cancel_clear_requests(void);

#endif /* HAX_SYSTEM_CANCEL_H */
