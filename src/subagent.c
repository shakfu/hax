/* SPDX-License-Identifier: MIT */
#include "subagent.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "diag.h"
#include "xalloc.h"
#include "system/tempfiles.h"
#include "text/fmt.h"

#define LEDGER_MAGIC "hax-subagent-ledger 1"

/* Costs are stored in whole micro-dollars: exact sums, and no locale-dependent decimal point. */
#define MICROS_PER_USD 1e6

struct ledger {
    long max_agents;
    long max_tokens;
    long long max_cost_micros;
    long agents;
    long tokens;
    long long cost_micros;
    int cost_unknown; /* some response had no known cost */
};

/* fcntl locks belong to the process, so threads of one process also need a mutex. Closing any
 * descriptor of the file drops the process's lock, so at most one is open at a time. */
static pthread_mutex_t ledger_mutex = PTHREAD_MUTEX_INITIALIZER;

static long long usd_to_micros(double usd)
{
    return usd > 0 ? llround(usd * MICROS_PER_USD) : 0;
}

static void ledger_format(const struct ledger *ledger, char *out, size_t size)
{
    snprintf(out, size, LEDGER_MAGIC "\nbudget %ld %ld %lld\nused %ld %ld %lld %d\n",
             ledger->max_agents, ledger->max_tokens, ledger->max_cost_micros, ledger->agents,
             ledger->tokens, ledger->cost_micros, ledger->cost_unknown);
}

static int ledger_parse(const char *text, struct ledger *ledger)
{
    int consumed = -1;
    int fields =
        sscanf(text, LEDGER_MAGIC "\nbudget %ld %ld %lld\nused %ld %ld %lld %d\n%n",
               &ledger->max_agents, &ledger->max_tokens, &ledger->max_cost_micros, &ledger->agents,
               &ledger->tokens, &ledger->cost_micros, &ledger->cost_unknown, &consumed);
    return fields == 7 && consumed >= 0 && text[consumed] == '\0' ? 0 : -1;
}

static int lock_whole_file(int fd)
{
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    while (fcntl(fd, F_SETLKW, &lock) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

/* Run `op` on the locked ledger; it returns 1 to write the ledger back, 0 to leave it. */
static int ledger_transact(const char *path, int (*op)(struct ledger *, void *), void *arg)
{
    int result = -1;
    pthread_mutex_lock(&ledger_mutex);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        goto out_unlock;
    if (lock_whole_file(fd) < 0)
        goto out_close;

    char text[256];
    ssize_t n = pread(fd, text, sizeof(text) - 1, 0);
    if (n < 0)
        goto out_close;
    text[n] = '\0';
    struct ledger ledger;
    if (ledger_parse(text, &ledger) < 0) {
        errno = EINVAL;
        goto out_close;
    }

    if (op(&ledger, arg)) {
        ledger_format(&ledger, text, sizeof(text));
        size_t len = strlen(text);
        if (pwrite(fd, text, len, 0) != (ssize_t)len || ftruncate(fd, (off_t)len) < 0)
            goto out_close;
    }
    result = 0;

out_close:;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
out_unlock:
    pthread_mutex_unlock(&ledger_mutex);
    return result;
}

static char *spent_reason(const struct ledger *ledger)
{
    if (ledger->max_tokens > 0 && ledger->tokens >= ledger->max_tokens)
        return xasprintf("subagent token budget spent (%ld of %ld)", ledger->tokens,
                         ledger->max_tokens);
    if (ledger->max_cost_micros > 0 && ledger->cost_unknown)
        return xstrdup("subagent cost budget cannot be enforced: a response had unknown cost");
    if (ledger->max_cost_micros > 0 && ledger->cost_micros >= ledger->max_cost_micros)
        return xasprintf("subagent cost budget spent ($%.2f of $%.2f)",
                         (double)ledger->cost_micros / MICROS_PER_USD,
                         (double)ledger->max_cost_micros / MICROS_PER_USD);
    return NULL;
}

int subagent_ledger_create(const struct subagent_budget *budget, char **path_out)
{
    int fd = tempfile_create("subagent-", ".ledger", path_out);
    if (fd < 0)
        return -1;
    struct ledger ledger = {
        .max_agents = budget->max_agents > 0 ? budget->max_agents : 0,
        .max_tokens = budget->max_tokens > 0 ? budget->max_tokens : 0,
        .max_cost_micros = usd_to_micros(budget->max_cost),
    };
    char text[256];
    ledger_format(&ledger, text, sizeof(text));
    size_t len = strlen(text);
    int result = write(fd, text, len) == (ssize_t)len ? 0 : -1;
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}

static int admit_op(struct ledger *ledger, void *arg)
{
    char **refusal = arg;
    *refusal = spent_reason(ledger);
    if (!*refusal && ledger->max_agents > 0 && ledger->agents >= ledger->max_agents)
        *refusal = xasprintf("subagent limit reached (%ld started)", ledger->agents);
    if (*refusal)
        return 0;
    ledger->agents++;
    return 1;
}

int subagent_ledger_admit(const char *path, char **refusal_out)
{
    *refusal_out = NULL;
    if (ledger_transact(path, admit_op, refusal_out) == 0)
        return 0;
    *refusal_out = xasprintf("subagent budget ledger %s is unreadable: %s", path, strerror(errno));
    return -1;
}

struct charge {
    long tokens;
    double cost;
};

static int charge_op(struct ledger *ledger, void *arg)
{
    const struct charge *charge = arg;
    ledger->tokens += charge->tokens;
    if (charge->cost < 0)
        ledger->cost_unknown = 1;
    else
        ledger->cost_micros += usd_to_micros(charge->cost);
    return 1;
}

int subagent_ledger_charge(const char *path, long tokens, double cost)
{
    struct charge charge = {.tokens = tokens > 0 ? tokens : 0, .cost = cost};
    return ledger_transact(path, charge_op, &charge);
}

static int spent_op(struct ledger *ledger, void *arg)
{
    *(char **)arg = spent_reason(ledger);
    return 0;
}

int subagent_ledger_spent(const char *path, char **spent_out)
{
    *spent_out = NULL;
    return ledger_transact(path, spent_op, spent_out);
}

/* Process state: written by subagent_admit, read by the other entry points. */
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static char depth_assignment[48];
static char *ledger_assignment; /* "HAX_SUBAGENT_LEDGER=<path>" for children, or NULL */
static char *charged_ledger;    /* the inherited ledger this run is charged to, or NULL */
static char *created_ledger;    /* the ledger this run created, unlinked at exit */
static int unlink_registered;

static void unlink_created_ledger(void)
{
    if (created_ledger)
        unlink(created_ledger);
}

static const char *const MAX_DEPTH_ASSIGNMENTS[SUBAGENT_DEPTH_LIMIT + 1] = {
    "HAX_SUBAGENT_MAX_DEPTH=0", "HAX_SUBAGENT_MAX_DEPTH=1", "HAX_SUBAGENT_MAX_DEPTH=2",
    "HAX_SUBAGENT_MAX_DEPTH=3", "HAX_SUBAGENT_MAX_DEPTH=4", "HAX_SUBAGENT_MAX_DEPTH=5",
    "HAX_SUBAGENT_MAX_DEPTH=6", "HAX_SUBAGENT_MAX_DEPTH=7", "HAX_SUBAGENT_MAX_DEPTH=8",
};

/* This process's depth, or -1 for a malformed inherited value. */
static int current_depth(void)
{
    const char *value = getenv("HAX_SUBAGENT_DEPTH");
    int depth = 0;
    if (value && *value && (!parse_int(value, &depth) || depth < 0))
        return -1;
    return depth;
}

/* Caller holds state_mutex. */
static void fill_depth_assignment(void)
{
    int depth = current_depth();
    /* A malformed depth fails closed: the child lands past every allowed max_depth. */
    int child_depth =
        depth < 0 || depth >= SUBAGENT_DEPTH_LIMIT ? SUBAGENT_DEPTH_LIMIT + 1 : depth + 1;
    snprintf(depth_assignment, sizeof(depth_assignment), "HAX_SUBAGENT_DEPTH=%d", child_depth);
}

static void configured_budget(struct subagent_budget *budget)
{
    budget->max_agents = config_int("subagent.max_agents");
    budget->max_tokens = config_tokens("subagent.max_tokens");
    budget->max_cost = config_usd("subagent.max_cost");
}

static int budget_configured(const struct subagent_budget *budget)
{
    return budget->max_agents > 0 || budget->max_tokens > 0 || budget->max_cost > 0;
}

static int admit_locked(void)
{
    free(ledger_assignment);
    ledger_assignment = NULL;
    free(charged_ledger);
    charged_ledger = NULL;
    fill_depth_assignment();

    int depth = current_depth();
    int max_depth = config_int("subagent.max_depth");
    if (depth < 0 || depth > max_depth) {
        hax_err("subagent depth limit (%d) reached; run the task directly instead of spawning "
                "another hax",
                max_depth);
        return -1;
    }

    const char *inherited = getenv("HAX_SUBAGENT_LEDGER");
    if (inherited && *inherited) {
        char *refusal = NULL;
        subagent_ledger_admit(inherited, &refusal);
        if (refusal) {
            hax_err("%s; run the task directly instead of spawning another hax", refusal);
            free(refusal);
            return -1;
        }
        charged_ledger = xstrdup(inherited);
        ledger_assignment = xasprintf("HAX_SUBAGENT_LEDGER=%s", inherited);
        return 0;
    }

    struct subagent_budget budget;
    configured_budget(&budget);
    if (!budget_configured(&budget))
        return 0;
    char *path = NULL;
    if (subagent_ledger_create(&budget, &path) < 0) {
        hax_err("cannot create the subagent budget ledger: %s", strerror(errno));
        free(path);
        return -1;
    }
    /* The budget spans the whole run, so the ledger must outlive the tempfile sweep that
     * starting a new conversation performs. */
    tempfile_untrack(path);
    if (created_ledger)
        unlink(created_ledger);
    free(created_ledger);
    created_ledger = path;
    if (!unlink_registered) {
        atexit(unlink_created_ledger);
        unlink_registered = 1;
    }
    ledger_assignment = xasprintf("HAX_SUBAGENT_LEDGER=%s", path);
    return 0;
}

int subagent_admit(void)
{
    pthread_mutex_lock(&state_mutex);
    int result = admit_locked();
    pthread_mutex_unlock(&state_mutex);
    return result;
}

size_t subagent_child_env(const char *out[3])
{
    size_t count = 0;
    pthread_mutex_lock(&state_mutex);
    /* Embedders and tests never admit; depth still travels to their children. */
    if (!depth_assignment[0])
        fill_depth_assignment();
    out[count++] = depth_assignment;
    int max_depth = config_int("subagent.max_depth");
    if (max_depth > SUBAGENT_DEPTH_LIMIT)
        max_depth = SUBAGENT_DEPTH_LIMIT;
    out[count++] = MAX_DEPTH_ASSIGNMENTS[max_depth];
    if (ledger_assignment)
        out[count++] = ledger_assignment;
    pthread_mutex_unlock(&state_mutex);
    return count;
}

void subagent_charge(long input_tokens, long output_tokens, double cost)
{
    pthread_mutex_lock(&state_mutex);
    char *path = charged_ledger ? xstrdup(charged_ledger) : NULL;
    pthread_mutex_unlock(&state_mutex);
    if (!path)
        return;
    long tokens = (input_tokens > 0 ? input_tokens : 0) + (output_tokens > 0 ? output_tokens : 0);
    /* Losing a charge only loosens a guard rail; the next check of a broken ledger stops the
     * run. */
    if (subagent_ledger_charge(path, tokens, cost) < 0)
        hax_warn("cannot charge the subagent budget ledger: %s", strerror(errno));
    free(path);
}

char *subagent_budget_spent(void)
{
    pthread_mutex_lock(&state_mutex);
    char *path = charged_ledger ? xstrdup(charged_ledger) : NULL;
    pthread_mutex_unlock(&state_mutex);
    if (!path)
        return NULL;
    char *spent = NULL;
    if (subagent_ledger_spent(path, &spent) < 0)
        spent = xasprintf("subagent budget ledger %s is unreadable: %s", path, strerror(errno));
    free(path);
    return spent;
}
