/* SPDX-License-Identifier: MIT */
#ifndef HAX_SUBAGENT_H
#define HAX_SUBAGENT_H

#include <stddef.h>

/* Limits on the tree of hax processes a run starts through the bash tool. These are guard rails
 * against runaway delegation, not a security boundary: the model controls the child's shell.
 *
 * Depth travels down as HAX_SUBAGENT_DEPTH and HAX_SUBAGENT_MAX_DEPTH. The budgets (agent
 * count, tokens, dollars) need state siblings share, so the first run with a budget configured
 * creates a ledger file and exports its path as HAX_SUBAGENT_LEDGER. The ledger records the
 * budget and what every descendant has used; each access holds an fcntl lock across the
 * read-modify-write. The run that creates a ledger is not charged to it. */

/* Upper bound of subagent.max_depth. */
#define SUBAGENT_DEPTH_LIMIT 8

struct subagent_budget {
    long max_agents; /* <= 0 is unlimited */
    long max_tokens; /* <= 0 is unlimited */
    double max_cost; /* USD; <= 0 is unlimited */
};

/* Startup gate, run once on the foreground thread after config loads. Refuses a run nested
 * deeper than subagent.max_depth, and a run whose inherited ledger has no budget left; an
 * admitted child is counted against the ledger and charged for its responses. A run without an
 * inherited ledger creates one when a budget is configured. Returns 0 to run, or -1 after a
 * diagnostic. Calling it again replaces the previous admission. */
int subagent_admit(void);

/* Store at most 3 borrowed environment assignments for a child process in `out`, valid for the
 * process lifetime, and return their count. */
size_t subagent_child_env(const char *out[3]);

/* Charge one response to the inherited ledger: input plus output tokens, and `cost` in USD, or
 * a negative cost when it is unknown. A no-op for a run that was not admitted as a child. */
void subagent_charge(long input_tokens, long output_tokens, double cost);

/* Allocated description of the spent token or cost budget, or NULL while budget remains (and
 * always for a run that was not admitted as a child). */
char *subagent_budget_spent(void);

/* Ledger primitives over an explicit path, for the functions above and their tests. Each
 * returns -1 with errno set when the ledger cannot be read or written. */

/* Create a private ledger holding `budget` and nothing used; *path_out receives the allocated
 * path, removed at process exit. */
int subagent_ledger_create(const struct subagent_budget *budget, char **path_out);

/* Count one agent in unless the budget is spent. *refusal_out receives NULL when admitted, or an
 * allocated reason. A ledger that cannot be read refuses. */
int subagent_ledger_admit(const char *path, char **refusal_out);

int subagent_ledger_charge(const char *path, long tokens, double cost);

/* *spent_out receives NULL while the token and cost budgets remain, or an allocated reason. */
int subagent_ledger_spent(const char *path, char **spent_out);

#endif /* HAX_SUBAGENT_H */
