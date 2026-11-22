/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "harness.h"
#include "subagent.h"
#include "xalloc.h"

static void reset_env(void)
{
    unsetenv("HAX_SUBAGENT_DEPTH");
    unsetenv("HAX_SUBAGENT_MAX_DEPTH");
    unsetenv("HAX_SUBAGENT_LEDGER");
    unsetenv("HAX_SUBAGENT_MAX_AGENTS");
    unsetenv("HAX_SUBAGENT_MAX_TOKENS");
    unsetenv("HAX_SUBAGENT_MAX_COST");
}

static char *ledger_with(long max_agents, long max_tokens, double max_cost)
{
    struct subagent_budget budget = {max_agents, max_tokens, max_cost};
    char *path = NULL;
    EXPECT(subagent_ledger_create(&budget, &path) == 0);
    return path;
}

/* Returns the borrowed assignment for `name` from subagent_child_env, or NULL. */
static const char *child_assignment(const char *name)
{
    const char *env[3];
    size_t count = subagent_child_env(env);
    size_t len = strlen(name);
    for (size_t i = 0; i < count; i++) {
        if (strncmp(env[i], name, len) == 0 && env[i][len] == '=')
            return env[i] + len + 1;
    }
    return NULL;
}

static void test_depth_gate(void)
{
    reset_env();
    EXPECT(subagent_admit() == 0);
    /* The default max_depth of 2 admits grandchildren but not their children. */
    setenv("HAX_SUBAGENT_DEPTH", "2", 1);
    EXPECT(subagent_admit() == 0);
    setenv("HAX_SUBAGENT_DEPTH", "3", 1);
    EXPECT(subagent_admit() == -1);
    setenv("HAX_SUBAGENT_DEPTH", "invalid", 1);
    EXPECT(subagent_admit() == -1);
    setenv("HAX_SUBAGENT_DEPTH", "-1", 1);
    EXPECT(subagent_admit() == -1);

    setenv("HAX_SUBAGENT_MAX_DEPTH", "0", 1);
    setenv("HAX_SUBAGENT_DEPTH", "1", 1);
    EXPECT(subagent_admit() == -1);
    setenv("HAX_SUBAGENT_MAX_DEPTH", "5", 1);
    setenv("HAX_SUBAGENT_DEPTH", "5", 1);
    EXPECT(subagent_admit() == 0);
    reset_env();
}

static void test_child_env_stamps_depth_and_limit(void)
{
    reset_env();
    EXPECT(subagent_admit() == 0);
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_DEPTH"), "1");
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_MAX_DEPTH"), "2");
    EXPECT(child_assignment("HAX_SUBAGENT_LEDGER") == NULL);

    /* The limit is read live, so a child inherits the root's current value. */
    setenv("HAX_SUBAGENT_MAX_DEPTH", "4", 1);
    setenv("HAX_SUBAGENT_DEPTH", "3", 1);
    EXPECT(subagent_admit() == 0);
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_DEPTH"), "4");
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_MAX_DEPTH"), "4");

    /* A malformed depth stamps a child past every allowed limit. */
    setenv("HAX_SUBAGENT_DEPTH", "bogus", 1);
    EXPECT(subagent_admit() == -1);
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_DEPTH"), "9");
    reset_env();
    EXPECT(subagent_admit() == 0);
}

static void test_root_creates_ledger_and_is_not_charged(void)
{
    reset_env();
    setenv("HAX_SUBAGENT_MAX_TOKENS", "1k", 1);
    EXPECT(subagent_admit() == 0);
    const char *path = child_assignment("HAX_SUBAGENT_LEDGER");
    EXPECT(path != NULL && access(path, R_OK | W_OK) == 0);
    char *first = xstrdup(path ? path : "");

    subagent_charge(5000, 5000, 1.0);
    EXPECT(subagent_budget_spent() == NULL);

    /* A second admission replaces the ledger, removing the first. */
    EXPECT(subagent_admit() == 0);
    EXPECT(access(first, F_OK) != 0);
    free(first);

    reset_env();
    EXPECT(subagent_admit() == 0);
    EXPECT(child_assignment("HAX_SUBAGENT_LEDGER") == NULL);
}

static void test_child_is_counted_and_charged(void)
{
    reset_env();
    char *path = ledger_with(1, 10, 0);
    setenv("HAX_SUBAGENT_DEPTH", "1", 1);
    setenv("HAX_SUBAGENT_LEDGER", path, 1);
    /* An inherited ledger wins over the child's own budget settings. */
    setenv("HAX_SUBAGENT_MAX_AGENTS", "50", 1);
    EXPECT(subagent_admit() == 0);
    EXPECT_STR_EQ(child_assignment("HAX_SUBAGENT_LEDGER"), path);
    EXPECT(subagent_budget_spent() == NULL);

    subagent_charge(6, 5, -1);
    char *spent = subagent_budget_spent();
    EXPECT_STR_EQ(spent, "subagent token budget spent (11 of 10)");
    free(spent);

    /* A spent budget refuses further children. */
    EXPECT(subagent_admit() == -1);

    reset_env();
    EXPECT(subagent_admit() == 0);
    subagent_charge(100, 100, 1.0);
    EXPECT(subagent_budget_spent() == NULL);
    free(path);
}

static void test_ledger_agent_limit(void)
{
    char *path = ledger_with(2, 0, 0);
    char *refusal = NULL;
    EXPECT(subagent_ledger_admit(path, &refusal) == 0 && refusal == NULL);
    EXPECT(subagent_ledger_admit(path, &refusal) == 0 && refusal == NULL);
    EXPECT(subagent_ledger_admit(path, &refusal) == 0);
    EXPECT_STR_EQ(refusal, "subagent limit reached (2 started)");
    free(refusal);
    free(path);
}

static void test_ledger_cost_limit(void)
{
    char *path = ledger_with(0, 0, 0.5);
    char *spent = NULL;
    EXPECT(subagent_ledger_charge(path, 0, 0.25) == 0);
    EXPECT(subagent_ledger_spent(path, &spent) == 0 && spent == NULL);
    EXPECT(subagent_ledger_charge(path, 0, 0.25) == 0);
    EXPECT(subagent_ledger_spent(path, &spent) == 0);
    EXPECT_STR_EQ(spent, "subagent cost budget spent ($0.50 of $0.50)");
    free(spent);
    free(path);

    /* An unknown cost cannot be counted, so a cost budget fails closed. */
    path = ledger_with(0, 0, 0.5);
    EXPECT(subagent_ledger_charge(path, 0, -1) == 0);
    EXPECT(subagent_ledger_spent(path, &spent) == 0);
    EXPECT_STR_EQ(spent, "subagent cost budget cannot be enforced: a response had unknown cost");
    char *refusal = NULL;
    EXPECT(subagent_ledger_admit(path, &refusal) == 0);
    EXPECT_STR_EQ(refusal, spent);
    free(refusal);
    free(spent);
    free(path);

    /* Without a cost budget, unknown cost is irrelevant. */
    path = ledger_with(0, 100, 0);
    EXPECT(subagent_ledger_charge(path, 1, -1) == 0);
    EXPECT(subagent_ledger_spent(path, &spent) == 0 && spent == NULL);
    free(path);
}

static void test_ledger_unreadable_refuses(void)
{
    char *refusal = NULL;
    char *missing = xasprintf("%s/none.ledger", t_tempdir());
    EXPECT(subagent_ledger_admit(missing, &refusal) == -1);
    EXPECT(refusal != NULL && strstr(refusal, "unreadable") != NULL);
    free(refusal);
    free(missing);

    char *garbled = xasprintf("%s/garbled.ledger", t_tempdir());
    FILE *file = fopen(garbled, "w");
    EXPECT(file != NULL);
    if (file) {
        fputs("hax-subagent-ledger 1\nbudget 1 2\n", file);
        fclose(file);
    }
    EXPECT(subagent_ledger_admit(garbled, &refusal) == -1);
    EXPECT(refusal != NULL);
    free(refusal);
    free(garbled);
}

/* Each process admits until refused and charges per admission; the lock must make the totals
 * exact across processes. */
#define RACERS            6
#define ATTEMPTS_PER_RACE 10
#define AGENT_LIMIT       25

static void test_ledger_is_exact_across_processes(void)
{
    /* The token budget is spent only by the last of AGENT_LIMIT charges. */
    char *path = ledger_with(AGENT_LIMIT, AGENT_LIMIT * 3, 0);
    pid_t pids[RACERS];
    for (int i = 0; i < RACERS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            int admitted = 0;
            for (int n = 0; n < ATTEMPTS_PER_RACE; n++) {
                char *refusal = NULL;
                if (subagent_ledger_admit(path, &refusal) != 0)
                    _exit(255);
                if (!refusal) {
                    admitted++;
                    subagent_ledger_charge(path, 3, 0.01);
                }
                free(refusal);
            }
            _exit(admitted);
        }
        EXPECT(pids[i] > 0);
    }
    int total = 0;
    for (int i = 0; i < RACERS; i++) {
        int status = 0;
        EXPECT(waitpid(pids[i], &status, 0) == pids[i]);
        EXPECT(WIFEXITED(status) && WEXITSTATUS(status) != 255);
        total += WEXITSTATUS(status);
    }
    EXPECT(total == AGENT_LIMIT);

    /* Every admission was charged exactly once. */
    char *spent = NULL;
    EXPECT(subagent_ledger_spent(path, &spent) == 0);
    EXPECT_STR_EQ(spent, "subagent token budget spent (75 of 75)");
    free(spent);
    free(path);
}

int main(void)
{
    test_depth_gate();
    test_child_env_stamps_depth_and_limit();
    test_root_creates_ledger_and_is_not_charged();
    test_child_is_counted_and_charged();
    test_ledger_agent_limit();
    test_ledger_cost_limit();
    test_ledger_unreadable_refuses();
    test_ledger_is_exact_across_processes();
    T_REPORT();
}
