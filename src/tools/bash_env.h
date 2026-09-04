/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_BASH_ENV_H
#define HAX_TOOLS_BASH_ENV_H

/* The child environment stamp and startup guard must use the same recursion cap. */
#define HAX_SUBAGENT_MAX_DEPTH 3

#include <stddef.h>

/* The provider selection a child process inherits. Owned by the agent whose subprocesses it
 * describes: two agents in one process resolve different models, and a shared copy would publish
 * whichever wrote last — or free the other's strings underneath it. */
struct bash_env_selection {
    char *entries[4];
    size_t count;
};

/* Publish the effective provider selection for `selection`'s child processes. A NULL or empty
 * provider clears the export; NULL model and effort values are exported as empty strings. */
void bash_env_selection_set(struct bash_env_selection *selection, const char *provider,
                            const char *model, const char *effort);

/* Release the strings held by `selection`, leaving it empty and reusable. */
void bash_env_selection_free(struct bash_env_selection *selection);

/* Return a malloc'd environment vector whose entries remain borrowed — including `selection`'s,
 * which must outlive the vector. A NULL `selection` exports no provider selection. */
char **bash_build_child_env(const struct bash_env_selection *selection);

#endif /* HAX_TOOLS_BASH_ENV_H */
