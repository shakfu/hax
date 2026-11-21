/* SPDX-License-Identifier: MIT */
#include "acp.h"

#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "acp_proto.h"
#include "agent_core.h"
#include "agent_loop.h"
#include "agent_stats.h"
#include "agent_stream.h"
#include "agent_tool.h"
#include "buf.h"
#include "compact.h"
#include "config.h"
#include "diag.h"
#include "model_meta.h"
#include "provider.h"
#include "session.h"
#include "tool.h"
#include "transcript.h"
#include "version.h"
#include "xalloc.h"
#include "system/cancel.h"
#include "system/clock.h"
#include "system/rand.h"
#include "terminal/interrupt.h"
#include "tools/bash_process.h"
#include "tools/task_registry.h"

#define DENIED_RESULT "error: the user denied permission for this tool call"

/* A longer input line is discarded with a parse error. */
#define ACP_LINE_MAX_BYTES ((size_t)16 << 20)

/* A client request or notification waiting for the main thread. */
struct acp_message {
    struct acp_message *next;
    json_t *json;
    int cancelled; /* a session/prompt whose session/cancel arrived before it started */
};

/* An allow_always or reject_always answer, kept for the session's lifetime. */
struct acp_grant {
    struct acp_grant *next;
    char *tool;
    int allow;
};

struct acp_session {
    struct acp_session *next;
    char *id;
    char *cwd;
    struct agent_session agent;
    struct transcript_log *transcript;
    struct session_log *session_log;
    struct acp_grant *grants;
};

struct acp_server {
    struct provider *provider;
    const struct hax_opts *options;
    int recording;
    int initialized;
    struct acp_session *sessions;
    struct acp_session *active; /* the session whose prompt is running; main thread only */
    /* Main thread only: the running prompt's streamed text, and the StopReason its latest
     * request implied. */
    struct acp_echo message_echo;
    struct acp_echo thought_echo;
    const char *stop_cause;

    FILE *out;
    pthread_mutex_t out_lock; /* the reader thread writes parse errors too */

    int in_fd;
    pthread_mutex_t lock; /* guards the fields below, shared with the reader thread */
    pthread_cond_t changed;
    struct acp_message *head;
    struct acp_message *tail;
    struct acp_message *dispatching; /* taken off the queue, not yet finished */
    int eof;
    const char *running; /* borrowed id of the session whose prompt runs; NULL when idle */
    long next_request_id;
    long awaited_id; /* the agent-to-client request awaiting its response; 0 when none */
    json_t *response;
};

/* Output */

static void send_message(struct acp_server *server, json_t *message)
{
    json_object_set_new(message, "jsonrpc", json_string("2.0"));
    /* Compact output escapes every newline inside strings: one message, one line. */
    char *text = json_dumps(message, JSON_COMPACT);
    json_decref(message);
    if (!text)
        return;
    pthread_mutex_lock(&server->out_lock);
    fputs(text, server->out);
    fputc('\n', server->out);
    fflush(server->out);
    pthread_mutex_unlock(&server->out_lock);
    free(text);
}

/* `id` is borrowed; `result` is stolen. */
static void reply(struct acp_server *server, json_t *id, json_t *result)
{
    json_t *message = json_object();
    json_object_set(message, "id", id);
    json_object_set_new(message, "result", result);
    send_message(server, message);
}

static void reply_error(struct acp_server *server, json_t *id, int code, const char *text)
{
    json_t *error = json_object();
    json_object_set_new(error, "code", json_integer(code));
    json_object_set_new(error, "message", acp_text(text));
    json_t *message = json_object();
    json_object_set_new(message, "id", id ? json_incref(id) : json_null());
    json_object_set_new(message, "error", error);
    send_message(server, message);
}

/* `update` is stolen. */
static void send_update(struct acp_server *server, const char *session_id, json_t *update)
{
    json_t *params = json_object();
    json_object_set_new(params, "sessionId", acp_text(session_id));
    json_object_set_new(params, "update", update);
    json_t *message = json_object();
    json_object_set_new(message, "method", json_string("session/update"));
    json_object_set_new(message, "params", params);
    send_message(server, message);
}

/* Input: the reader thread */

static const char *message_session(const json_t *message)
{
    return json_string_value(json_object_get(json_object_get(message, "params"), "sessionId"));
}

static int message_is(const json_t *message, const char *method)
{
    const char *name = json_string_value(json_object_get(message, "method"));
    return name && strcmp(name, method) == 0;
}

static int is_prompt_for(const struct acp_message *message, const char *session_id)
{
    const char *prompt_session = message_session(message->json);
    return message_is(message->json, "session/prompt") && prompt_session &&
           strcmp(prompt_session, session_id) == 0;
}

/* Cancel the session's running prompt and any of its prompts not yet started. Caller holds
 * lock. */
static void cancel_session(struct acp_server *server, const char *session_id)
{
    if (!session_id)
        return;
    for (struct acp_message *queued = server->head; queued; queued = queued->next)
        if (is_prompt_for(queued, session_id))
            queued->cancelled = 1;
    if (server->dispatching && is_prompt_for(server->dispatching, session_id))
        server->dispatching->cancelled = 1;
    if (server->running && strcmp(server->running, session_id) == 0)
        cancel_request_abort();
}

static void take_line(struct acp_server *server, const char *line, size_t length)
{
    size_t blank = strspn(line, " \t\r");
    if (blank >= length)
        return;

    json_t *message = json_loadb(line, length, 0, NULL);
    if (!message) {
        reply_error(server, NULL, ACP_PARSE_ERROR, "parse error");
        return;
    }
    json_t *id = json_object_get(message, "id");
    json_t *method = json_object_get(message, "method");
    if (!json_is_object(message) || (method && !json_is_string(method)) ||
        (!method && !json_object_get(message, "result") && !json_object_get(message, "error"))) {
        reply_error(server, id, ACP_INVALID_REQUEST, "invalid request");
        json_decref(message);
        return;
    }

    int answer_cancel = 0;
    pthread_mutex_lock(&server->lock);
    if (!method) {
        /* A response nothing awaits any longer, such as one to a cancelled request, is dropped. */
        if (server->awaited_id && json_integer_value(id) == server->awaited_id &&
            !server->response) {
            server->response = message;
            message = NULL;
        }
    } else if (message_is(message, "session/cancel")) {
        /* A cancel sent as a request still cancels, and gets an answer. */
        cancel_session(server, message_session(message));
        answer_cancel = id != NULL;
    } else {
        struct acp_message *queued = xcalloc(1, sizeof(*queued));
        queued->json = message;
        message = NULL;
        if (server->tail)
            server->tail->next = queued;
        else
            server->head = queued;
        server->tail = queued;
    }
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    if (answer_cancel)
        reply(server, id, json_object());
    json_decref(message);
}

static void *reader_main(void *user)
{
    struct acp_server *server = user;
    struct buf line;
    buf_init(&line);
    char chunk[65536];
    int skipping = 0; /* the current line is over the limit; drop it up to its newline */

    for (;;) {
        ssize_t bytes_read = read(server->in_fd, chunk, sizeof(chunk));
        if (bytes_read < 0 && errno == EINTR)
            continue;
        /* Another process may have set O_NONBLOCK on a shared stdin: wait, then read again. */
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd ready = {.fd = server->in_fd, .events = POLLIN};
            if (poll(&ready, 1, -1) >= 0 || errno == EINTR)
                continue;
        }
        if (bytes_read <= 0)
            break;
        const char *cursor = chunk;
        const char *end = chunk + bytes_read;
        while (cursor < end) {
            const char *newline = memchr(cursor, '\n', (size_t)(end - cursor));
            size_t piece = (size_t)((newline ? newline : end) - cursor);
            if (!skipping && line.len + piece > ACP_LINE_MAX_BYTES) {
                reply_error(server, NULL, ACP_PARSE_ERROR, "message exceeds 16 MiB");
                buf_free(&line);
                skipping = 1;
            }
            if (!skipping)
                buf_append(&line, cursor, piece);
            if (!newline)
                break;
            if (!skipping) {
                buf_append(&line, "", 1);
                take_line(server, line.data, line.len - 1);
            }
            buf_reset(&line);
            skipping = 0;
            cursor = newline + 1;
        }
    }
    if (line.len > 0 && !skipping) {
        buf_append(&line, "", 1);
        take_line(server, line.data, line.len - 1);
    }
    buf_free(&line);

    /* The client is gone: nobody can see the running turn or answer its questions, and queued
     * prompts end cancelled without reaching the provider. */
    pthread_mutex_lock(&server->lock);
    server->eof = 1;
    for (struct acp_message *queued = server->head; queued; queued = queued->next)
        if (message_is(queued->json, "session/prompt"))
            queued->cancelled = 1;
    if (server->dispatching && message_is(server->dispatching->json, "session/prompt"))
        server->dispatching->cancelled = 1;
    if (server->running)
        cancel_request_abort();
    pthread_cond_broadcast(&server->changed);
    pthread_mutex_unlock(&server->lock);
    return NULL;
}

/* The next client request or notification; NULL once input has ended and the queue is empty. */
static struct acp_message *next_message(struct acp_server *server)
{
    pthread_mutex_lock(&server->lock);
    while (!server->head && !server->eof)
        pthread_cond_wait(&server->changed, &server->lock);
    struct acp_message *message = server->head;
    if (message) {
        server->head = message->next;
        if (!server->head)
            server->tail = NULL;
    }
    server->dispatching = message;
    pthread_mutex_unlock(&server->lock);
    return message;
}

/* Send a request to the client and wait for its result. NULL when it answers with an error, when
 * input ends, or when the running prompt is cancelled first. `params` is stolen. */
static json_t *client_call(struct acp_server *server, const char *method, json_t *params)
{
    pthread_mutex_lock(&server->lock);
    /* A cancel that already landed abandons the request before the client sees it. */
    if (cancel_abort_requested() || server->eof) {
        pthread_mutex_unlock(&server->lock);
        json_decref(params);
        return NULL;
    }
    long id = ++server->next_request_id;
    server->awaited_id = id;
    pthread_mutex_unlock(&server->lock);

    json_t *message = json_object();
    json_object_set_new(message, "id", json_integer(id));
    json_object_set_new(message, "method", json_string(method));
    json_object_set_new(message, "params", params);
    send_message(server, message);

    pthread_mutex_lock(&server->lock);
    while (!server->response && !server->eof && !cancel_abort_requested())
        pthread_cond_wait(&server->changed, &server->lock);
    json_t *response = server->response;
    server->response = NULL;
    server->awaited_id = 0;
    pthread_mutex_unlock(&server->lock);

    json_t *result = json_incref(json_object_get(response, "result"));
    json_decref(response);
    return result;
}

/* Sessions */

static struct acp_session *find_session(struct acp_server *server, const char *id)
{
    for (struct acp_session *session = server->sessions; id && session; session = session->next)
        if (strcmp(session->id, id) == 0)
            return session;
    return NULL;
}

static void session_destroy(struct acp_session *session)
{
    transcript_log_close(session->transcript);
    session_log_close(session->session_log);
    agent_session_free(&session->agent);
    while (session->grants) {
        struct acp_grant *grant = session->grants;
        session->grants = grant->next;
        free(grant->tool);
        free(grant);
    }
    free(session->id);
    free(session->cwd);
    free(session);
}

/* Build a session in `cwd`, resuming the file at `resume_path` when non-NULL. `id` names a
 * resumed session; a fresh one takes its session file's id. Returns NULL with an owned message in
 * `error`. */
static struct acp_session *session_open(struct acp_server *server, const char *cwd,
                                        const char *resume_path, const char *id, char **error)
{
    struct provider *provider = server->provider;
    if (chdir(cwd) != 0) {
        *error = xasprintf("cannot enter %s: %s", cwd, strerror(errno));
        return NULL;
    }

    struct acp_session *session = xcalloc(1, sizeof(*session));
    session->cwd = xstrdup(cwd);
    model_meta_wait(provider);
    agent_session_init(&session->agent, provider, server->options);
    if (!session->agent.model || !*session->agent.model) {
        *error =
            xasprintf("no model selected for provider '%s'", provider->name ? provider->name : "?");
        goto fail;
    }

    struct session_loaded loaded = {0};
    if (resume_path) {
        if (session_load_all(resume_path, &loaded) != 0) {
            *error = xasprintf("cannot read session %s", id);
            goto fail;
        }
        agent_session_adopt(&session->agent, &loaded);
    }
    session->transcript = transcript_log_open(session->agent.system_prompt, session->agent.tools,
                                              session->agent.n_tools);
    if (resume_path) {
        struct agent_resumed resumed;
        agent_session_prepare_resumed(&session->agent, provider, resume_path, &loaded.meta,
                                      session->transcript, &resumed);
        session_loaded_free(&loaded);
        session->session_log = resumed.session_log;
    } else if (server->recording) {
        session->session_log = session_log_open(agent_provider_log_name(provider),
                                                session->agent.model, session->agent.model_label,
                                                session->agent.effort, config_str("preset"));
    }

    if (id) {
        session->id = xstrdup(id);
    } else if (session_log_id(session->session_log)) {
        session->id = xstrdup(session_log_id(session->session_log));
    } else {
        char uuid[37];
        gen_uuid_v4(uuid);
        session->id = xstrdup(uuid);
    }
    session->next = server->sessions;
    server->sessions = session;
    return session;

fail:
    session_destroy(session);
    return NULL;
}

/* Loop hooks */

static enum agent_signal acp_poll(void *user)
{
    (void)user;
    return agent_interrupt_signal();
}

static void send_owned(struct acp_server *server, const char *kind, char *text)
{
    if (text)
        send_update(server, server->active->id, acp_text_chunk(kind, text));
    free(text);
}

static void send_chunk(struct acp_server *server, const char *kind, struct acp_echo *echo,
                       const char *text)
{
    send_owned(server, kind, text && *text ? acp_echo_take(echo, text) : NULL);
}

static int acp_observe(const struct stream_event *event, void *user)
{
    struct acp_server *server = user;
    switch (event->kind) {
    case EV_TEXT_DELTA:
        send_chunk(server, "agent_message_chunk", &server->message_echo, event->u.text_delta.text);
        break;
    case EV_REASONING_DELTA:
        send_chunk(server, "agent_thought_chunk", &server->thought_echo,
                   event->u.reasoning_delta.text);
        break;
    case EV_RETRY:
        /* The next attempt re-streams the response from its start. */
        acp_echo_retry(&server->message_echo);
        acp_echo_retry(&server->thought_echo);
        break;
    case EV_DONE:
    case EV_ERROR: {
        int failed = event->kind == EV_ERROR;
        send_owned(server, "agent_message_chunk", acp_echo_end(&server->message_echo, failed));
        send_owned(server, "agent_thought_chunk", acp_echo_end(&server->thought_echo, failed));
        server->stop_cause = acp_event_stop_reason(event);
        break;
    }
    default:
        break;
    }
    return 0;
}

static void acp_tool_seen(const struct item *call, void *user)
{
    struct acp_server *server = user;
    send_update(server, server->active->id, acp_tool_call(call, "pending", server->active->cwd));
}

static struct acp_grant *find_grant(const struct acp_session *session, const char *tool)
{
    for (struct acp_grant *grant = session->grants; grant; grant = grant->next)
        if (strcmp(grant->tool, tool) == 0)
            return grant;
    return NULL;
}

/* Ask the client whether `call` may run, unless the tool runs unasked or an earlier answer
 * covers it. A request the client cancels or fails denies. */
static int permitted(struct acp_server *server, const struct item *call)
{
    struct acp_session *session = server->active;
    const char *tool = call->tool_name ? call->tool_name : "";
    if (!acp_tool_asks_permission(call->tool_name, call->tool_arguments_json))
        return 1;
    const struct acp_grant *grant = find_grant(session, tool);
    if (grant)
        return grant->allow;

    json_t *tool_call = acp_tool_call(call, "pending", session->cwd);
    json_object_del(tool_call, "sessionUpdate");
    json_t *params = json_object();
    json_object_set_new(params, "sessionId", acp_text(session->id));
    json_object_set_new(params, "toolCall", tool_call);
    json_object_set_new(params, "options", acp_permission_options());
    json_t *result = client_call(server, "session/request_permission", params);
    enum acp_permission decision = acp_permission_decision(result);
    json_decref(result);

    int allow = decision == ACP_PERMISSION_ALLOW_ONCE || decision == ACP_PERMISSION_ALLOW_ALWAYS;
    if (decision == ACP_PERMISSION_ALLOW_ALWAYS || decision == ACP_PERMISSION_REJECT_ALWAYS) {
        struct acp_grant *remembered = xcalloc(1, sizeof(*remembered));
        remembered->tool = xstrdup(tool);
        remembered->allow = allow;
        remembered->next = session->grants;
        session->grants = remembered;
    }
    return allow;
}

static struct item synthetic_result(const struct item *call, const char *output,
                                    enum item_origin origin)
{
    struct item result = agent_tool_result_make(call, output, NULL);
    result.origin = origin;
    return result;
}

static struct item acp_tool_run(const struct item *call, enum agent_loop_tool_action action,
                                int image_input, void *user)
{
    struct acp_server *server = user;
    struct acp_session *session = server->active;
    struct item result;
    int denied = 0;

    if (action == AGENT_LOOP_TOOL_REFUSE) {
        result = synthetic_result(call, REFUSED_RESULT, ITEM_ORIGIN_REFUSED);
    } else if (action == AGENT_LOOP_TOOL_SKIP) {
        result = synthetic_result(call, INTERRUPT_MARKER, ITEM_ORIGIN_SKIPPED);
    } else if (!permitted(server, call) || cancel_abort_requested()) {
        /* A cancel during the question leaves the call unrun, not refused, whatever the answer. */
        if (cancel_abort_requested()) {
            result = synthetic_result(call, INTERRUPT_MARKER, ITEM_ORIGIN_SKIPPED);
        } else {
            result = synthetic_result(call, DENIED_RESULT, ITEM_ORIGIN_NONE);
            denied = 1;
        }
    } else {
        send_update(server, session->id, acp_tool_call_update(call->call_id, "in_progress", NULL));
        struct agent_tool_call prepared;
        agent_tool_call_init(&prepared, call);
        struct tool_run_ctx run_ctx = {
            .image_input = image_input,
            .env_selection = &session->agent.env_selection,
        };
        char *output = agent_tool_call_run(&prepared, &run_ctx);
        result = agent_tool_result_make(call, output, &run_ctx);
        free(output);
        agent_tool_call_destroy(&prepared);
    }

    const char *status = denied || acp_result_failed(&result) ? "failed" : "completed";
    send_update(server, session->id, acp_tool_call_update(call->call_id, status, &result));
    return result;
}

static int acp_compact(enum compact_reason reason, void *user)
{
    struct acp_server *server = user;
    struct acp_session *session = server->active;
    struct compact_params params = {
        .session = &session->agent,
        .provider = server->provider,
        .session_log = session->session_log,
        .transcript_log = session->transcript,
        .reason = reason,
        .hooks = {.user = server, .poll = acp_poll, .checkpoint = acp_poll},
    };
    struct compact_result result;

    compact_run(&params, &result);
    int completed = result.outcome == COMPACT_COMPLETE;
    if (result.outcome == COMPACT_PROVIDER_ERROR)
        hax_warn("compaction failed: %s",
                 result.error_message ? result.error_message : "stream failed");
    else if (result.outcome == COMPACT_NO_SUMMARY)
        hax_warn("compaction produced no summary");
    compact_result_destroy(&result);
    return completed;
}

/* Requests */

static void handle_initialize(struct acp_server *server, json_t *id)
{
    json_t *capabilities =
        json_pack("{s:b, s:{s:b, s:b, s:b}, s:{s:b, s:b}}", "loadSession", server->recording,
                  "promptCapabilities", "image", 0, "audio", 0, "embeddedContext", 0,
                  "mcpCapabilities", "http", 0, "sse", 0);
    json_t *result = json_pack("{s:i, s:o, s:{s:s, s:s}, s:[]}", "protocolVersion",
                               ACP_PROTOCOL_VERSION, "agentCapabilities", capabilities, "agentInfo",
                               "name", "hax", "version", HAX_VERSION, "authMethods");
    server->initialized = 1;
    reply(server, id, result);
}

static const char *absolute_cwd(const json_t *params)
{
    const char *cwd = json_string_value(json_object_get(params, "cwd"));
    return cwd && cwd[0] == '/' ? cwd : NULL;
}

static void note_mcp_servers(const json_t *params)
{
    size_t count = json_array_size(json_object_get(params, "mcpServers"));
    if (count > 0)
        hax_warn("no MCP client; ignoring %zu MCP server%s", count, count == 1 ? "" : "s");
}

static void handle_new(struct acp_server *server, json_t *id, const json_t *params)
{
    const char *cwd = absolute_cwd(params);
    if (!cwd) {
        reply_error(server, id, ACP_INVALID_PARAMS, "session/new needs an absolute cwd");
        return;
    }
    note_mcp_servers(params);

    char *error = NULL;
    struct acp_session *session = session_open(server, cwd, NULL, NULL, &error);
    if (!session) {
        reply_error(server, id, ACP_INTERNAL_ERROR, error);
        free(error);
        return;
    }
    reply(server, id, json_pack("{s:o}", "sessionId", acp_text(session->id)));
}

static void handle_load(struct acp_server *server, json_t *id, const json_t *params)
{
    const char *session_id = json_string_value(json_object_get(params, "sessionId"));
    const char *cwd = absolute_cwd(params);
    if (!session_id || !*session_id || !cwd) {
        reply_error(server, id, ACP_INVALID_PARAMS,
                    "session/load needs a sessionId and an "
                    "absolute cwd");
        return;
    }
    note_mcp_servers(params);

    struct acp_session *session = find_session(server, session_id);
    if (!session) {
        /* Session files are bucketed by the physical directory, which a symlinked cwd hides. */
        char *physical = chdir(cwd) == 0 ? getcwd(NULL, 0) : NULL;
        /* An exact id only: a prefix would open a second copy of a session already open. */
        char *path = physical ? session_find(physical, session_id, 0) : NULL;
        free(physical);
        if (!path) {
            char *message = xasprintf("no session %s in %s", session_id, cwd);
            reply_error(server, id, ACP_NOT_FOUND, message);
            free(message);
            return;
        }
        char *error = NULL;
        session = session_open(server, cwd, path, session_id, &error);
        free(path);
        if (!session) {
            reply_error(server, id, ACP_INTERNAL_ERROR, error);
            free(error);
            return;
        }
    }

    json_t *updates =
        acp_history_updates(session->agent.items, session->agent.n_items, session->cwd);
    size_t i;
    json_t *update;
    json_array_foreach(updates, i, update) send_update(server, session->id, json_incref(update));
    json_decref(updates);
    reply(server, id, json_object());
}

/* Report window use and the session's cumulative spend, when the window is known. */
static void send_usage(struct acp_server *server, struct acp_session *session, long used)
{
    long size = model_meta_context(server->provider, session->agent.model);
    if (used < 0)
        used = agent_session_last_context_tokens(&session->agent);
    if (used < 0 || size <= 0)
        return;

    json_t *update = json_pack("{s:s, s:I, s:I}", "sessionUpdate", "usage_update", "used",
                               (json_int_t)used, "size", (json_int_t)size);
    struct agent_stats stats;
    agent_stats_collect(&session->agent, 0, 0, server->provider, &stats);
    if (stats.total.spend > 0)
        json_object_set_new(
            update, "cost",
            json_pack("{s:f, s:s}", "amount", stats.total.spend, "currency", "USD"));
    send_update(server, session->id, update);
}

/* Returns whether the prompt reached the session: a cancel before that leaves no turn. */
static int run_prompt(struct acp_server *server, struct acp_session *session, const char *text,
                      struct agent_loop_result *result)
{
    struct agent_session *agent = &session->agent;
    struct provider *provider = server->provider;

    memset(result, 0, sizeof(*result));
    result->last_context_tokens = -1;
    /* Compact before appending the prompt, as the REPL does; a cancel there stops the send. */
    if (!cancel_abort_requested() && compact_due(agent, provider, text))
        acp_compact(COMPACT_THRESHOLD, server);
    if (cancel_abort_requested()) {
        result->outcome = AGENT_LOOP_INTERRUPTED;
        return 0;
    }

    agent_session_add_user(agent, text);
    agent_session_resync_effort(agent, provider, NULL);
    session_log_set_meta(session->session_log, agent_provider_log_name(provider), agent->model,
                         agent->model_label, agent->effort, config_str("preset"));
    agent_flush_logs(session->transcript, session->session_log, agent->items, agent->n_items);
    model_meta_prefetch(provider);

    struct agent_loop_params params = {
        .session = agent,
        .provider = provider,
        .tlog = session->transcript,
        .slog = session->session_log,
        .max_turns = config_int("max_turns"),
        .hooks =
            {
                .user = server,
                .observe = acp_observe,
                .poll = acp_poll,
                .checkpoint = acp_poll,
                .tool_seen = acp_tool_seen,
                .tool_call = acp_tool_run,
                .compact = acp_compact,
            },
    };
    agent_loop_run(&params, result);
    return 1;
}

static void handle_prompt(struct acp_server *server, json_t *id, const json_t *params,
                          const struct acp_message *message)
{
    struct acp_session *session =
        find_session(server, json_string_value(json_object_get(params, "sessionId")));
    if (!session) {
        reply_error(server, id, ACP_NOT_FOUND, "unknown session");
        return;
    }
    char *text = acp_prompt_text(json_object_get(params, "prompt"));
    if (!text) {
        reply_error(server, id, ACP_INVALID_PARAMS, "the prompt has no text");
        return;
    }
    if (chdir(session->cwd) != 0) {
        char *error = xasprintf("cannot enter %s: %s", session->cwd, strerror(errno));
        reply_error(server, id, ACP_INTERNAL_ERROR, error);
        free(error);
        free(text);
        return;
    }

    /* The latch is cleared and the prompt marked running in one step, so a cancel lands either
     * on the queued message or on the running turn. */
    pthread_mutex_lock(&server->lock);
    cancel_clear_requests();
    if (message->cancelled)
        cancel_request_abort();
    server->running = session->id;
    pthread_mutex_unlock(&server->lock);
    server->active = session;
    server->stop_cause = NULL;
    acp_echo_reset(&server->message_echo);
    acp_echo_reset(&server->thought_echo);

    long started_ms = monotonic_ms();
    struct agent_loop_result result;
    if (run_prompt(server, session, text, &result)) {
        long elapsed_ms = monotonic_ms() - started_ms;
        agent_session_add_worked(&session->agent, elapsed_ms);
        session_log_user_turn(session->session_log, elapsed_ms);
    }

    pthread_mutex_lock(&server->lock);
    int cancelled = cancel_abort_requested();
    server->running = NULL;
    pthread_mutex_unlock(&server->lock);
    server->active = NULL;

    const char *stop = acp_stop_reason(result.outcome, cancelled, server->stop_cause);
    if (stop) {
        send_usage(server, session, result.last_context_tokens);
        reply(server, id, json_pack("{s:s}", "stopReason", stop));
    } else {
        char *error = xasprintf("provider error: %s",
                                result.error_message ? result.error_message : "(no message)");
        reply_error(server, id, ACP_INTERNAL_ERROR, error);
        free(error);
    }
    agent_loop_result_destroy(&result);
    free(text);
}

static void dispatch(struct acp_server *server, const struct acp_message *message)
{
    json_t *id = json_object_get(message->json, "id");
    const json_t *params = json_object_get(message->json, "params");
    const char *method = json_string_value(json_object_get(message->json, "method"));

    /* No notification but session/cancel, which the reader acts on, needs handling. */
    if (!id || !method)
        return;
    if (strcmp(method, "initialize") == 0) {
        handle_initialize(server, id);
    } else if (!server->initialized) {
        reply_error(server, id, ACP_INVALID_REQUEST, "initialize first");
    } else if (strcmp(method, "session/new") == 0) {
        handle_new(server, id, params);
    } else if (strcmp(method, "session/load") == 0) {
        handle_load(server, id, params);
    } else if (strcmp(method, "session/prompt") == 0) {
        handle_prompt(server, id, params, message);
    } else {
        char *error = xasprintf("method not found: %s", method);
        reply_error(server, id, ACP_METHOD_NOT_FOUND, error);
        free(error);
    }
}

int acp_serve(struct provider *provider, const struct hax_opts *options, int in_fd, FILE *out)
{
    struct acp_server server = {
        .provider = provider,
        .options = options,
        .recording = agent_recording_enabled(provider),
        .out = out,
        .in_fd = in_fd,
    };
    acp_echo_init(&server.message_echo);
    acp_echo_init(&server.thought_echo);
    pthread_mutex_init(&server.out_lock, NULL);
    pthread_mutex_init(&server.lock, NULL);
    pthread_cond_init(&server.changed, NULL);

    pthread_t reader;
    int result = 0;
    if (pthread_create(&reader, NULL, reader_main, &server) != 0) {
        hax_err("cannot start the ACP reader thread");
        result = 1;
        goto cleanup;
    }

    struct acp_message *message;
    while ((message = next_message(&server))) {
        dispatch(&server, message);
        pthread_mutex_lock(&server.lock);
        server.dispatching = NULL;
        pthread_mutex_unlock(&server.lock);
        json_decref(message->json);
        free(message);
    }
    pthread_join(reader, NULL);

    /* Each session records the final state of the tasks it started, then one shutdown kills the
     * rest. */
    for (struct acp_session *session = server.sessions; session; session = session->next) {
        task_registry_set_owner(&session->agent);
        agent_record_task_exit(&session->agent, session->transcript, session->session_log);
    }
    task_registry_set_owner(NULL);
    task_registry_shutdown();
    while (server.sessions) {
        struct acp_session *session = server.sessions;
        server.sessions = session->next;
        session_destroy(session);
    }
    json_decref(server.response);

cleanup:
    acp_echo_free(&server.message_echo);
    acp_echo_free(&server.thought_echo);
    pthread_cond_destroy(&server.changed);
    pthread_mutex_destroy(&server.lock);
    pthread_mutex_destroy(&server.out_lock);
    return result;
}

int acp_run(struct provider *provider, const struct hax_opts *options)
{
    /* Anything that writes to stdout by habit lands on stderr; tools never see the protocol fd. */
    fflush(stdout);
    int protocol_fd = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    FILE *out = protocol_fd >= 0 ? fdopen(protocol_fd, "w") : NULL;
    if (!out || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        hax_err("cannot set up the ACP output: %s", strerror(errno));
        if (out)
            fclose(out);
        else if (protocol_fd >= 0)
            close(protocol_fd);
        return 1;
    }

    interrupt_install_fatal_signal_handlers();
    interrupt_set_fatal_signal_hook(bash_shell_pgids_kill);
    signal(SIGUSR1, SIG_IGN);
    /* A vanished client must not kill hax before it can stop its tools' process groups. */
    signal(SIGPIPE, SIG_IGN);

    int result = acp_serve(provider, options, STDIN_FILENO, out);
    fclose(out);
    return result;
}
