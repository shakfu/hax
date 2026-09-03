/* SPDX-License-Identifier: MIT */
/* Several agents in one process.
 *
 * hax_init() refuses a second initialization, but nothing refuses a second agent_session: the
 * conversation state is per-instance and the loop is fully instance-parameterized. These tests
 * pin that down so it stays true, and give the remaining process-wide state a place to fail.
 *
 * Under BUILD_DIR=build-tsan the same assertions double as a race gate: the agents here run
 * concurrently and dispatch tools, which is what reaches the globals below agent_loop_run(). */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "config.h"
#include "harness.h"
#include "hax_embed.h"
#include "provider.h"
#include "system/cancel.h"
#include "system/clock.h"

struct fixture {
    const char *label;
    const char *script_path;
    const char *marker;  /* appears in this agent's own tool output */
    const char *foreign; /* must never appear in this agent's history */
    struct provider *provider;
    struct cancel_state cancel;
    /* Set by the run thread as it returns, so the main thread can ask whether an agent is still
     * working without reading its result while it is being written. */
    atomic_int finished;
    struct agent_session session;
    struct agent_loop_result result;
};

/* A two-turn script: one tool call, then a closing message. The tool output carries `marker` so
 * a result routed to the wrong session is visible rather than merely suspected. */
static void write_script(const char *path, const char *label, const char *marker)
{
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (!file)
        return;
    fprintf(file, "text I am %s\n", label);
    fprintf(file, "tool bash {\"command\":\"echo %s\"}\n", marker);
    fprintf(file, "end-turn\n\n");
    fprintf(file, "text %s finished\n", label);
    fprintf(file, "end-turn\n");
    fclose(file);
}

/* Resolve config into instance state at construction, the pattern providers/mock.c already uses
 * for its script: each agent keeps the value that was current when it was built. */
static void fixture_construct(struct fixture *fixture)
{
    static const struct hax_opts opts;
    config_set_override("providers.mock.script", fixture->script_path);
    fixture->provider = hax_provider_new("mock");
    EXPECT(fixture->provider != NULL);
    if (!fixture->provider)
        return;
    agent_session_init(&fixture->session, fixture->provider, &opts);
    agent_session_add_user(&fixture->session, "go");
}

static void fixture_run(struct fixture *fixture)
{
    struct agent_loop_params params = {
        .session = &fixture->session,
        .provider = fixture->provider,
        .max_turns = 6,
    };
    agent_loop_run(&params, &fixture->result);
}

static void *fixture_run_thread(void *arg)
{
    fixture_run(arg);
    return NULL;
}

static int history_contains(const struct agent_session *session, const char *needle)
{
    for (size_t i = 0; i < session->n_items; i++) {
        const struct item *item = &session->items[i];
        if (item->text && strstr(item->text, needle))
            return 1;
        if (item->output && strstr(item->output, needle))
            return 1;
    }
    return 0;
}

/* Completed the run, saw its own tool output, and saw nothing belonging to its sibling. */
static void expect_independent(const struct fixture *fixture)
{
    EXPECT(fixture->result.outcome == AGENT_LOOP_COMPLETE);
    EXPECT(fixture->result.turns == 2);
    EXPECT(history_contains(&fixture->session, fixture->marker));
    EXPECT(!history_contains(&fixture->session, fixture->foreign));
}

static void fixture_destroy(struct fixture *fixture)
{
    agent_loop_result_destroy(&fixture->result);
    agent_session_free(&fixture->session);
    hax_provider_destroy(fixture->provider);
}

/* Build two fixtures with distinct scripts under a fresh temp dir. */
static void fixtures_init(struct fixture *a, struct fixture *b, char *paths[2])
{
    char *dir = t_tempdir();
    static char path_a[512], path_b[512];
    snprintf(path_a, sizeof(path_a), "%s/a.txt", dir);
    snprintf(path_b, sizeof(path_b), "%s/b.txt", dir);
    write_script(path_a, "agent-A", "AAA");
    write_script(path_b, "agent-B", "BBB");
    paths[0] = path_a;
    paths[1] = path_b;

    a->label = "agent-A";
    a->script_path = path_a;
    a->marker = "AAA";
    a->foreign = "BBB";
    b->label = "agent-B";
    b->script_path = path_b;
    b->marker = "BBB";
    b->foreign = "AAA";
}

static void test_two_agents_run_sequentially(void)
{
    EXPECT(hax_init(NULL) == 0);
    config_set_override("provider", "mock");

    struct fixture a = {0}, b = {0};
    char *paths[2];
    fixtures_init(&a, &b, paths);
    fixture_construct(&a);
    fixture_construct(&b);

    fixture_run(&a);
    fixture_run(&b);

    expect_independent(&a);
    expect_independent(&b);

    fixture_destroy(&a);
    fixture_destroy(&b);
    hax_shutdown();
}

/* The divergent scripts matter: identical config would hide any sharing. */
static void test_two_agents_run_concurrently(void)
{
    EXPECT(hax_init(NULL) == 0);
    config_set_override("provider", "mock");

    struct fixture a = {0}, b = {0};
    char *paths[2];
    fixtures_init(&a, &b, paths);
    fixture_construct(&a);
    fixture_construct(&b);

    pthread_t thread_a, thread_b;
    EXPECT(pthread_create(&thread_a, NULL, fixture_run_thread, &a) == 0);
    EXPECT(pthread_create(&thread_b, NULL, fixture_run_thread, &b) == 0);
    pthread_join(thread_a, NULL);
    pthread_join(thread_b, NULL);

    expect_independent(&a);
    expect_independent(&b);

    fixture_destroy(&a);
    fixture_destroy(&b);
    hax_shutdown();
}

/* Four agents sharing one script. Construction stays on the calling thread: AGENTS.md makes
 * config and live provider state foreground state ("resolve config and prepare owned worker
 * inputs before spawning background work"), so building a session concurrently asks for a
 * guarantee the architecture does not offer. Running the built sessions concurrently is the
 * supported shape, and is what this pins down. */
static void test_four_agents_run_concurrently(void)
{
    EXPECT(hax_init(NULL) == 0);
    config_set_override("provider", "mock");

    char *dir = t_tempdir();
    char path[512];
    snprintf(path, sizeof(path), "%s/shared.txt", dir);
    write_script(path, "agent", "SHARED");
    config_set_override("providers.mock.script", path);

    struct fixture agents[4] = {0};
    for (size_t i = 0; i < 4; i++) {
        agents[i].label = "agent";
        agents[i].script_path = path;
        agents[i].marker = "SHARED";
        agents[i].foreign = "NOTHING-ELSE";
        fixture_construct(&agents[i]);
    }

    pthread_t threads[4];
    for (size_t i = 0; i < 4; i++)
        EXPECT(pthread_create(&threads[i], NULL, fixture_run_thread, &agents[i]) == 0);
    for (size_t i = 0; i < 4; i++)
        pthread_join(threads[i], NULL);

    for (size_t i = 0; i < 4; i++)
        expect_independent(&agents[i]);
    for (size_t i = 0; i < 4; i++)
        fixture_destroy(&agents[i]);
    hax_shutdown();
}

/* A script whose single tool call outlives the test's patience, so a cancel lands mid-tool. */
static void write_stalling_script(const char *path)
{
    FILE *file = fopen(path, "w");
    EXPECT(file != NULL);
    if (!file)
        return;
    fprintf(file, "text working\n");
    fprintf(file, "tool bash {\"command\":\"sleep 30\"}\n");
    fprintf(file, "end-turn\n\n");
    fprintf(file, "text done\n");
    fprintf(file, "end-turn\n");
    fclose(file);
}

/* Poll for a run thread to return, up to `timeout_ms`. Returns whether it did. */
static int wait_for_finish(const struct fixture *fixture, long timeout_ms)
{
    long deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        if (atomic_load(&fixture->finished))
            return 1;
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
        nanosleep(&tick, NULL);
    }
    return atomic_load(&fixture->finished);
}

/* What an embedder's checkpoint hook does: answer for this agent's state, not the process's. */
static int fixture_checkpoint(void *user)
{
    struct fixture *fixture = user;
    if (cancel_state_abort_requested(&fixture->cancel))
        return AGENT_LOOP_SIG_ABORT;
    if (cancel_state_pause_requested(&fixture->cancel))
        return AGENT_LOOP_SIG_PAUSE;
    return AGENT_LOOP_SIG_NONE;
}

static void *fixture_run_cancellable(void *arg)
{
    struct fixture *fixture = arg;
    struct agent_loop_params params = {
        .session = &fixture->session,
        .provider = fixture->provider,
        .cancel = &fixture->cancel,
        .max_turns = 6,
        .hooks = {.user = fixture, .checkpoint = fixture_checkpoint},
    };
    agent_loop_run(&params, &fixture->result);
    atomic_store(&fixture->finished, 1);
    return NULL;
}

/* Cancelling one agent must stop its tool and its turn while a sibling runs on. Both agents sit
 * in `sleep 30`, so anything that finishes promptly did so because it was cancelled. */
static void test_cancel_stops_one_agent_only(void)
{
    EXPECT(hax_init(NULL) == 0);
    config_set_override("provider", "mock");

    char *dir = t_tempdir();
    char stalling[512], quick[512];
    snprintf(stalling, sizeof(stalling), "%s/stall.txt", dir);
    snprintf(quick, sizeof(quick), "%s/quick.txt", dir);
    write_stalling_script(stalling);
    write_stalling_script(quick);

    struct fixture target = {
        .label = "target", .script_path = stalling, .marker = "", .foreign = "\x01"};
    struct fixture bystander = {
        .label = "bystander", .script_path = quick, .marker = "", .foreign = "\x01"};
    fixture_construct(&target);
    fixture_construct(&bystander);

    pthread_t threads[2];
    EXPECT(pthread_create(&threads[0], NULL, fixture_run_cancellable, &target) == 0);
    EXPECT(pthread_create(&threads[1], NULL, fixture_run_cancellable, &bystander) == 0);

    /* Let both reach the tool, then cancel exactly one. */
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 400 * 1000 * 1000};
    nanosleep(&pause, NULL);
    cancel_state_request_abort(&target.cancel);

    /* Wait for the cancel to take rather than joining blind: a per-agent cancel that never
     * reaches the tool would otherwise wedge this test for the whole sleep. */
    int target_stopped = wait_for_finish(&target, 10000);
    EXPECT(target_stopped);
    /* The sibling neither latched a cancel nor stopped: it is still inside its own sleep. */
    EXPECT(!cancel_state_abort_requested(&bystander.cancel));
    EXPECT(!atomic_load(&bystander.finished));
    if (!target_stopped) {
        /* Unwedge both sleeps so the failure is reported instead of timing out the suite. */
        cancel_request_abort();
        T_SKIP("the cancelled agent never stopped; the remaining checks cannot be trusted");
    }

    pthread_join(threads[0], NULL);
    EXPECT(target.result.outcome == AGENT_LOOP_INTERRUPTED);

    /* Only now let the sibling go, so its own cancel is unambiguously ours and not a leak. */
    cancel_state_request_abort(&bystander.cancel);
    pthread_join(threads[1], NULL);
    EXPECT(bystander.result.outcome == AGENT_LOOP_INTERRUPTED);

    /* The process state was never touched by either agent's cancel. */
    EXPECT(!cancel_abort_requested());

    fixture_destroy(&target);
    fixture_destroy(&bystander);
    hax_shutdown();
}

int main(void)
{
    test_two_agents_run_sequentially();
    test_two_agents_run_concurrently();
    test_four_agents_run_concurrently();
    test_cancel_stops_one_agent_only();
    T_REPORT();
}
