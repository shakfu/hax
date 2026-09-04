/* SPDX-License-Identifier: MIT */
#include "system/cancel.h"

#include <stdatomic.h>
#include <stddef.h>

static struct cancel_state process_state;

struct cancel_state *cancel_process_state(void)
{
    return &process_state;
}

/* A NULL state is the process state: tools and loops are handed whatever their caller had, and
 * "no per-agent state" is the ordinary single-agent case rather than an error. */
static struct cancel_state *resolve(struct cancel_state *state)
{
    return state ? state : &process_state;
}

/* Reads take a const state for callers' benefit; atomic_load needs a non-const lvalue, and
 * loading a flag mutates nothing observable. */
static struct cancel_state *resolve_const(const struct cancel_state *state)
{
    return state ? (struct cancel_state *)state : &process_state;
}

int cancel_state_request_pause_once(struct cancel_state *state)
{
    return atomic_exchange(&resolve(state)->pause_requested, 1);
}

void cancel_state_request_pause(struct cancel_state *state)
{
    if (cancel_state_request_pause_once(state))
        atomic_store(&resolve(state)->abort_requested, 1);
}

void cancel_state_request_abort(struct cancel_state *state)
{
    atomic_store(&resolve(state)->pause_requested, 1);
    atomic_store(&resolve(state)->abort_requested, 1);
}

int cancel_state_pause_requested(const struct cancel_state *state)
{
    return atomic_load(&resolve_const(state)->pause_requested);
}

int cancel_state_abort_requested(const struct cancel_state *state)
{
    return atomic_load(&resolve_const(state)->abort_requested);
}

void cancel_state_clear(struct cancel_state *state)
{
    atomic_store(&resolve(state)->abort_requested, 0);
    atomic_store(&resolve(state)->pause_requested, 0);
}

int cancel_request_pause_once(void)
{
    return cancel_state_request_pause_once(&process_state);
}

void cancel_request_pause(void)
{
    cancel_state_request_pause(&process_state);
}

void cancel_request_abort(void)
{
    cancel_state_request_abort(&process_state);
}

int cancel_pause_requested(void)
{
    return cancel_state_pause_requested(&process_state);
}

int cancel_abort_requested(void)
{
    return cancel_state_abort_requested(&process_state);
}

void cancel_clear_requests(void)
{
    cancel_state_clear(&process_state);
}
