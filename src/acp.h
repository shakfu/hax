/* SPDX-License-Identifier: MIT */
#ifndef HAX_ACP_H
#define HAX_ACP_H

#include <stdio.h>

#include "agent_core.h"
#include "provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Agent Client Protocol v1 frontend: serve newline-delimited JSON-RPC 2.0 from `in_fd` to `out`
 * until `in_fd` reaches EOF, driving each session's prompts through agent_loop. `out` carries
 * only protocol messages; diagnostics stay on stderr. A reader thread consumes `in_fd` while a
 * prompt runs, so session/cancel reaches the running turn. Returns the process exit status. */
int acp_serve(struct provider *provider, const struct hax_opts *options, int in_fd, FILE *out);

/* acp_serve() on stdin and stdout, after pointing fd 1 at stderr so nothing but the protocol
 * reaches the client, with the one-shot frontend's signal dispositions. */
int acp_run(struct provider *provider, const struct hax_opts *options);

#ifdef __cplusplus
}
#endif

#endif /* HAX_ACP_H */
