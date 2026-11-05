/* SPDX-License-Identifier: MIT */
#include "terminal/herdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xalloc.h"
#include "system/spawn.h"

#define HERDR_SOURCE "hax"
#define HERDR_AGENT  "hax"

/* Last report sent, so an unchanged state is not re-sent on every prompt. */
static int last_state = -1;
static char *last_session_id;
static unsigned long long last_seq;

static const char *pane_id(void)
{
    const char *env = getenv("HERDR_ENV");
    const char *pane = getenv("HERDR_PANE_ID");
    const char *bin = getenv("HERDR_BIN_PATH");
    const char *socket = getenv("HERDR_SOCKET_PATH");
    if (!env || strcmp(env, "1") != 0 || !pane || !*pane || !bin || !*bin || !socket || !*socket)
        return NULL;
    return pane;
}

/* herdr drops a report whose seq does not exceed the last one it accepted, across restarts, so
 * wall-clock microseconds, bumped past the previous value when the clock stalls or steps back. */
static void next_seq(char *buf, size_t size)
{
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    unsigned long long seq =
        (unsigned long long)now.tv_sec * 1000000ULL + (unsigned long long)now.tv_nsec / 1000ULL;
    if (seq <= last_seq)
        seq = last_seq + 1;
    last_seq = seq;
    snprintf(buf, size, "%llu", seq);
}

/* herdr rejects the whole report when a resume argument holds an apostrophe or control byte. */
static int resume_id_valid(const char *id)
{
    if (!id || !*id)
        return 0;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        if (*p == '\'' || *p < 0x20 || *p == 0x7f)
            return 0;
    }
    return 1;
}

static int same_session(const char *a, const char *b)
{
    return a == b || (a && b && strcmp(a, b) == 0);
}

void herdr_report(enum herdr_state state, const char *session_id)
{
    const char *pane = pane_id();
    if (!pane)
        return;
    if (!resume_id_valid(session_id))
        session_id = NULL;
    if ((int)state == last_state && same_session(session_id, last_session_id))
        return;
    last_state = (int)state;
    free(last_session_id);
    last_session_id = session_id ? xstrdup(session_id) : NULL;

    char seq[32];
    next_seq(seq, sizeof(seq));
    char *resume = session_id ? xasprintf("--resume=%s", session_id) : NULL;
    const char *argv[] = {
        getenv("HERDR_BIN_PATH"),
        "pane",
        "report-agent",
        pane,
        "--source",
        HERDR_SOURCE,
        "--agent",
        HERDR_AGENT,
        "--state",
        state == HERDR_WORKING ? "working" : "idle",
        "--seq",
        seq,
        /* Optional tail: session id, then the resume command after "--". */
        session_id ? "--agent-session-id" : NULL,
        session_id,
        "--",
        "hax",
        resume,
        NULL,
    };
    spawn_detached(argv);
    free(resume);
}

void herdr_release(void)
{
    const char *pane = pane_id();
    if (!pane || last_state < 0)
        return;
    last_state = -1;
    free(last_session_id);
    last_session_id = NULL;

    char seq[32];
    next_seq(seq, sizeof(seq));
    const char *argv[] = {
        getenv("HERDR_BIN_PATH"),
        "pane",
        /* Pane first: herdr 0.9.5's release-agent rejects options before it, despite its usage. */
        "release-agent",
        pane,
        "--source",
        HERDR_SOURCE,
        "--agent",
        HERDR_AGENT,
        "--seq",
        seq,
        NULL,
    };
    spawn_detached(argv);
}
