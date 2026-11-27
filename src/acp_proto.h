/* SPDX-License-Identifier: MIT */
#ifndef HAX_ACP_PROTO_H
#define HAX_ACP_PROTO_H

#include <jansson.h>
#include <stddef.h>

#include "agent_loop.h"
#include "buf.h"
#include "provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Translation between hax's records and Agent Client Protocol v1 messages
 * (https://agentclientprotocol.com/protocol/). Pure: no I/O, so the server in acp.c and the tests
 * share one mapping. Every json_t returned is a new reference. */

#define ACP_PROTOCOL_VERSION 1

/* JSON-RPC 2.0 error codes, plus ACP's resource-not-found. */
#define ACP_PARSE_ERROR      (-32700)
#define ACP_INVALID_REQUEST  (-32600)
#define ACP_METHOD_NOT_FOUND (-32601)
#define ACP_INVALID_PARAMS   (-32602)
#define ACP_INTERNAL_ERROR   (-32603)
#define ACP_NOT_FOUND        (-32002)

/* A string value for `text`, replacing malformed UTF-8 rather than failing as json_string does.
 * NULL becomes "". */
json_t *acp_text(const char *text);

/* ToolKind for a hax tool name: read, edit, execute, or other. */
const char *acp_tool_kind(const char *tool_name);

/* Whether running the tool needs the client's permission. Only reads and task waits that do not
 * kill run unasked; an unknown name asks. `arguments_json` may be NULL. */
int acp_tool_asks_permission(const char *tool_name, const char *arguments_json);

/* A tool_call update for `call` with `status`: title, kind, rawInput, and a location for tools
 * that take a path, resolved against `cwd` when relative. */
json_t *acp_tool_call(const struct item *call, const char *status, const char *cwd);

/* A tool_call_update to `status`, with `result`'s visible output as text content when non-NULL. */
json_t *acp_tool_call_update(const char *call_id, const char *status, const struct item *result);

/* Whether a result reports a call that did not run to completion. */
int acp_result_failed(const struct item *result);

/* An agent_message_chunk, agent_thought_chunk, or user_message_chunk carrying `text`. */
json_t *acp_text_chunk(const char *kind, const char *text);

/* The prompt's ContentBlocks as one user message: text blocks in order, and each resource_link
 * as an @-mention of its percent-decoded file path. Returns an owned string, or NULL when
 * `blocks` is not an array or yields no text. */
char *acp_prompt_text(const json_t *blocks);

/* The session/update payloads that replay items[0, n_items) for session/load: typed prompts,
 * assistant text as it streamed, readable reasoning, and each tool call with its final status. */
json_t *acp_history_updates(const struct item *items, size_t n_items, const char *cwd);

/* The StopReason a request's terminal event implies: "max_tokens" for a length limit, "refusal"
 * for a provider's refusal stop. NULL for any other event. */
const char *acp_event_stop_reason(const struct stream_event *event);

/* StopReason for a loop outcome. A latched cancel reads as "cancelled" whatever the outcome;
 * `cause`, from acp_event_stop_reason or NULL, overrides a completion or provider error. NULL for
 * any other provider error, which the prompt answers with a JSON-RPC error. */
const char *acp_stop_reason(enum agent_loop_outcome outcome, int cancelled, const char *cause);

/* Streamed text the client already holds for one response. A retried attempt re-streams from
 * the start, so its matching prefix is not sent twice. */
struct acp_echo {
    struct buf shown;
    size_t matched; /* bytes of `shown` the current attempt has re-streamed */
    int separate;   /* a failed response showed text; the next one starts after a break */
};

void acp_echo_init(struct acp_echo *echo);
void acp_echo_free(struct acp_echo *echo);
/* A new attempt of the same response begins. */
void acp_echo_retry(struct acp_echo *echo);
/* A new prompt begins. */
void acp_echo_reset(struct acp_echo *echo);
/* The response ends, `failed` or not. Returns the owned text to send when its last attempt
 * re-streamed only a prefix of what was shown: that attempt in full, after a paragraph break. */
char *acp_echo_end(struct acp_echo *echo, int failed);
/* The owned text to send for `delta`, or NULL when the client already has it. A retry that
 * diverges sends its whole text so far after a paragraph break. */
char *acp_echo_take(struct acp_echo *echo, const char *delta);

/* The options a permission request offers. Each optionId equals its kind. */
json_t *acp_permission_options(void);

enum acp_permission {
    ACP_PERMISSION_REJECT_ONCE = 0, /* also a cancelled, malformed, or unknown answer */
    ACP_PERMISSION_REJECT_ALWAYS,
    ACP_PERMISSION_ALLOW_ONCE,
    ACP_PERMISSION_ALLOW_ALWAYS,
};

/* Read a session/request_permission result. Anything but a selected, offered option denies. */
enum acp_permission acp_permission_decision(const json_t *result);

/* The host-defined tools in a session/new or session/load request's `_meta.hax.tools`, as owned
 * defs carrying `schema_json`. Absent tools yield *n = 0. Returns 0, or -1 with an owned message
 * in `error` that names the offending tool. */
int acp_host_tools_parse(const json_t *params, struct tool_def **defs, size_t *n, char **error);
void acp_host_tools_free(struct tool_def *defs, size_t n);

/* How a client's response relates to the one agent-to-client request that can be outstanding. */
enum acp_response_match {
    ACP_RESPONSE_AWAITED, /* answers the outstanding request */
    ACP_RESPONSE_COERCED, /* answers it, but by the same number of another type, or by a null id
                           * on an error, as JSON-RPC prescribes when the request did not parse */
    ACP_RESPONSE_STALE,   /* answers an earlier request, such as one a cancel abandoned */
    ACP_RESPONSE_UNKNOWN, /* answers no request hax sent */
};

/* Classify `response` against `awaited`, the outstanding request's id or 0 when none, given that
 * ids 1 through `issued` have been sent. */
enum acp_response_match acp_response_match(const json_t *response, long awaited, long issued);

/* The model-facing output for the JSON-RPC `response` to a _hax/tool_call: its text content parts
 * joined by newlines. A JSON-RPC error, a missing result, or `isError` sets *failed and yields
 * "error: " and the message. Returns an owned string. */
char *acp_host_tool_output(const json_t *response, int *failed);

#ifdef __cplusplus
}
#endif

#endif /* HAX_ACP_PROTO_H */
