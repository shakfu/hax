/* SPDX-License-Identifier: MIT */
#include "acp_proto.h"

#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "agent_core.h"
#include "agent_loop.h"
#include "buf.h"
#include "provider.h"
#include "tool.h"
#include "xalloc.h"
#include "text/utf8_sanitize.h"
#include "transport/api_error.h"

/* A title or location longer than this says nothing more to a person reading it. */
#define TITLE_MAX_BYTES 200

json_t *acp_text(const char *text)
{
    if (!text)
        return json_string("");
    json_t *value = json_string(text);
    if (value)
        return value;
    char *clean = utf8_sanitize(text, strlen(text));
    value = json_string(clean);
    free(clean);
    return value;
}

const char *acp_tool_kind(const char *tool_name)
{
    static const struct {
        const char *tool;
        const char *kind;
    } KINDS[] = {
        {"read", "read"},
        {"edit", "edit"},
        {"write", "edit"},
        {"bash", "execute"},
    };
    for (size_t i = 0; tool_name && i < sizeof(KINDS) / sizeof(*KINDS); i++)
        if (strcmp(tool_name, KINDS[i].tool) == 0)
            return KINDS[i].kind;
    return "other";
}

int acp_tool_asks_permission(const char *tool_name, const char *arguments_json)
{
    if (tool_name && strcmp(tool_name, "read") == 0)
        return 0;
    if (!tool_name || strcmp(tool_name, "task_wait") != 0)
        return 1;
    /* A wait that may kill its task stops a process, so it asks like bash does. */
    json_t *arguments = arguments_json ? json_loads(arguments_json, 0, NULL) : NULL;
    int kill = !json_is_object(arguments) || json_is_true(json_object_get(arguments, "kill"));
    json_decref(arguments);
    return kill;
}

/* The call's string argument `name`, or NULL. */
static const char *string_argument(const json_t *arguments, const char *name)
{
    return name ? json_string_value(json_object_get(arguments, name)) : NULL;
}

static char *tool_title(const char *tool_name, const json_t *arguments)
{
    const struct tool *tool = tool_name ? agent_find_tool(tool_name) : NULL;
    const char *argument = string_argument(arguments, tool ? tool->display.arg_name : NULL);
    if (!argument || !*argument)
        return xstrdup(tool_name ? tool_name : "tool");
    size_t length = strcspn(argument, "\n");
    if (length > TITLE_MAX_BYTES)
        length = TITLE_MAX_BYTES;
    return xasprintf("%s %.*s", tool_name, (int)length, argument);
}

json_t *acp_tool_call(const struct item *call, const char *status, const char *cwd)
{
    json_t *arguments =
        call->tool_arguments_json ? json_loads(call->tool_arguments_json, 0, NULL) : NULL;
    json_t *update = json_object();
    char *title = tool_title(call->tool_name, arguments);

    json_object_set_new(update, "sessionUpdate", json_string("tool_call"));
    json_object_set_new(update, "toolCallId", acp_text(call->call_id));
    json_object_set_new(update, "title", acp_text(title));
    json_object_set_new(update, "kind", json_string(acp_tool_kind(call->tool_name)));
    json_object_set_new(update, "status", json_string(status));
    if (json_is_object(arguments))
        json_object_set(update, "rawInput", arguments);

    const char *path = string_argument(arguments, "path");
    if (path && *path && strcmp(acp_tool_kind(call->tool_name), "execute") != 0) {
        char *absolute = path[0] == '/' || !cwd ? xstrdup(path) : xasprintf("%s/%s", cwd, path);
        json_t *location = json_object();
        json_object_set_new(location, "path", acp_text(absolute));
        json_object_set_new(update, "locations", json_pack("[o]", location));
        free(absolute);
    }
    free(title);
    json_decref(arguments);
    return update;
}

int acp_result_failed(const struct item *result)
{
    return result->origin == ITEM_ORIGIN_SKIPPED || result->origin == ITEM_ORIGIN_REFUSED ||
           result->origin == ITEM_ORIGIN_INTERRUPTED;
}

json_t *acp_tool_call_update(const char *call_id, const char *status, const struct item *result)
{
    json_t *update = json_object();
    json_object_set_new(update, "sessionUpdate", json_string("tool_call_update"));
    json_object_set_new(update, "toolCallId", acp_text(call_id));
    json_object_set_new(update, "status", json_string(status));
    if (result && result->output) {
        size_t length = strlen(result->output);
        if (result->output_hidden_tail <= length)
            length -= result->output_hidden_tail;
        char *visible = xmalloc(length + 1);
        memcpy(visible, result->output, length);
        visible[length] = '\0';
        json_t *text = json_object();
        json_object_set_new(text, "type", json_string("text"));
        json_object_set_new(text, "text", acp_text(visible));
        json_t *content = json_object();
        json_object_set_new(content, "type", json_string("content"));
        json_object_set_new(content, "content", text);
        json_object_set_new(update, "content", json_pack("[o]", content));
        free(visible);
    }
    return update;
}

json_t *acp_text_chunk(const char *kind, const char *text)
{
    json_t *content = json_object();
    json_object_set_new(content, "type", json_string("text"));
    json_object_set_new(content, "text", acp_text(text));
    json_t *update = json_object();
    json_object_set_new(update, "sessionUpdate", json_string(kind));
    json_object_set_new(update, "content", content);
    return update;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* The decoded path a local file:// URI names, or a copy of any other URI. Owned. */
static char *link_target(const char *uri)
{
    static const char scheme[] = "file://";
    if (strncmp(uri, scheme, sizeof(scheme) - 1) != 0)
        return xstrdup(uri);
    const char *path = uri + sizeof(scheme) - 1;
    if (strncmp(path, "localhost/", 10) == 0)
        path += 9;
    /* file://host/path names another machine's file. */
    if (path[0] != '/')
        return xstrdup(uri);

    struct buf decoded;
    buf_init(&decoded);
    for (const char *cursor = path; *cursor; cursor++) {
        int high = cursor[0] == '%' ? hex_value(cursor[1]) : -1;
        int low = high >= 0 ? hex_value(cursor[2]) : -1;
        /* %00 would end the string early, so it stays encoded. */
        if (low >= 0 && (high | low) != 0) {
            char byte = (char)(high * 16 + low);
            buf_append(&decoded, &byte, 1);
            cursor += 2;
        } else {
            buf_append(&decoded, cursor, 1);
        }
    }
    return buf_steal(&decoded);
}

char *acp_prompt_text(const json_t *blocks)
{
    if (!json_is_array(blocks))
        return NULL;

    struct buf text;
    buf_init(&text);
    size_t i;
    const json_t *block;
    json_array_foreach(blocks, i, block)
    {
        const char *type = json_string_value(json_object_get(block, "type"));
        const char *part = NULL;
        char *mention = NULL;
        if (type && strcmp(type, "text") == 0) {
            part = json_string_value(json_object_get(block, "text"));
        } else if (type && strcmp(type, "resource_link") == 0) {
            const char *uri = json_string_value(json_object_get(block, "uri"));
            if (uri && *uri) {
                char *target = link_target(uri);
                part = mention = xasprintf("@%s", target);
                free(target);
            }
        }
        if (part && *part) {
            if (text.len > 0)
                buf_append_str(&text, "\n\n");
            buf_append_str(&text, part);
        }
        free(mention);
    }
    if (text.len == 0) {
        buf_free(&text);
        return NULL;
    }
    return buf_steal(&text);
}

static const struct item *paired_result(const struct item *items, size_t n_items, size_t call)
{
    for (size_t i = call + 1; i < n_items; i++)
        if (items[i].kind == ITEM_TOOL_RESULT && items[i].call_id && items[call].call_id &&
            strcmp(items[i].call_id, items[call].call_id) == 0)
            return &items[i];
    return NULL;
}

/* An assistant message as the client saw it stream: without the marker hax appended to a reply
 * cut short. Returns an owned string. */
static char *streamed_text(const struct item *item)
{
    const char *text = item->text ? item->text : "";
    size_t length = strlen(text);
    size_t marker = strlen(INTERRUPT_MARKER);
    if (item->origin == ITEM_ORIGIN_INTERRUPTED && length >= marker &&
        strcmp(text + length - marker, INTERRUPT_MARKER) == 0) {
        size_t cut = length - marker;
        if (cut == 0)
            return xstrdup("");
        if (text[cut - 1] == '\n')
            return xasprintf("%.*s", (int)(cut - 1), text);
    }
    return xstrdup(text);
}

json_t *acp_history_updates(const struct item *items, size_t n_items, const char *cwd)
{
    json_t *updates = json_array();
    int separate = 0; /* the last update was a reply cut short; acp_echo_end set the same break */
    for (size_t i = 0; i < n_items; i++) {
        const struct item *item = &items[i];
        size_t before = json_array_size(updates);
        int cut_short = 0;
        switch (item->kind) {
        case ITEM_USER_MESSAGE:
            if (item_is_typed_prompt(item) && item->text && *item->text)
                json_array_append_new(updates, acp_text_chunk("user_message_chunk", item->text));
            break;
        case ITEM_ASSISTANT_MESSAGE: {
            char *text = streamed_text(item);
            if (*text) {
                char *sent = separate ? xasprintf("\n\n%s", text) : xstrdup(text);
                json_array_append_new(updates, acp_text_chunk("agent_message_chunk", sent));
                free(sent);
                cut_short = item->origin == ITEM_ORIGIN_INTERRUPTED;
            }
            free(text);
            break;
        }
        case ITEM_REASONING:
            if (item->reasoning_text && *item->reasoning_text)
                json_array_append_new(updates,
                                      acp_text_chunk("agent_thought_chunk", item->reasoning_text));
            break;
        case ITEM_TOOL_CALL: {
            const struct item *result = paired_result(items, n_items, i);
            const char *status = !result || acp_result_failed(result) ? "failed" : "completed";
            json_array_append_new(updates, acp_tool_call(item, status, cwd));
            if (result)
                json_array_append_new(updates, acp_tool_call_update(item->call_id, status, result));
            break;
        }
        case ITEM_TOOL_RESULT:
        case ITEM_TURN_BOUNDARY:
        case ITEM_TURN_USAGE:
            break;
        }
        if (json_array_size(updates) > before)
            separate = cut_short;
    }
    return updates;
}

const char *acp_event_stop_reason(const struct stream_event *event)
{
    if (event->kind == EV_DONE) {
        const char *reason = event->u.done.stop_reason;
        return reason && strcmp(reason, "refusal") == 0 ? "refusal" : NULL;
    }
    return event->kind == EV_ERROR && event->u.error.kind == API_ERROR_LENGTH_LIMIT ? "max_tokens"
                                                                                    : NULL;
}

const char *acp_stop_reason(enum agent_loop_outcome outcome, int cancelled, const char *cause)
{
    if (cancelled)
        return "cancelled";
    if (cause && (outcome == AGENT_LOOP_COMPLETE || outcome == AGENT_LOOP_PROVIDER_ERROR))
        return cause;
    switch (outcome) {
    case AGENT_LOOP_COMPLETE:
        return "end_turn";
    case AGENT_LOOP_MAX_TURNS:
    case AGENT_LOOP_BUDGET:
        return "max_turn_requests";
    case AGENT_LOOP_INTERRUPTED:
    case AGENT_LOOP_PAUSED:
        return "cancelled";
    case AGENT_LOOP_PROVIDER_ERROR:
        break;
    }
    return NULL;
}

void acp_echo_init(struct acp_echo *echo)
{
    buf_init(&echo->shown);
    echo->matched = 0;
    echo->separate = 0;
}

void acp_echo_free(struct acp_echo *echo)
{
    buf_free(&echo->shown);
    echo->matched = 0;
}

void acp_echo_retry(struct acp_echo *echo)
{
    echo->matched = 0;
}

void acp_echo_reset(struct acp_echo *echo)
{
    buf_reset(&echo->shown);
    echo->matched = 0;
    echo->separate = 0;
}

char *acp_echo_end(struct acp_echo *echo, int failed)
{
    char *resend = echo->matched > 0 && echo->matched < echo->shown.len
                       ? xasprintf("\n\n%.*s", (int)echo->matched, echo->shown.data)
                       : NULL;
    int separate = failed && (echo->separate || echo->shown.len > 0);
    acp_echo_reset(echo);
    echo->separate = separate;
    return resend;
}

char *acp_echo_take(struct acp_echo *echo, const char *delta)
{
    size_t length = strlen(delta);
    size_t available = echo->shown.len - echo->matched;
    const char *expected = echo->shown.data ? echo->shown.data + echo->matched : "";
    size_t common = 0;
    while (common < length && common < available && delta[common] == expected[common])
        common++;
    if (common == length) {
        echo->matched += length;
        return NULL;
    }
    if (common == available) {
        int separate = echo->separate && echo->shown.len == 0;
        echo->separate = 0;
        buf_append(&echo->shown, delta + common, length - common);
        echo->matched = echo->shown.len;
        return separate ? xasprintf("\n\n%s", delta + common) : xstrdup(delta + common);
    }

    /* The retry diverged: the client gets the new attempt in full, after a paragraph break. */
    echo->shown.len = echo->matched;
    echo->shown.data[echo->shown.len] = '\0';
    buf_append(&echo->shown, delta, length);
    echo->matched = echo->shown.len;
    return xasprintf("\n\n%s", echo->shown.data);
}

static const struct {
    const char *id; /* also the PermissionOptionKind */
    const char *name;
    enum acp_permission decision;
} OPTIONS[] = {
    {"allow_once", "Allow", ACP_PERMISSION_ALLOW_ONCE},
    {"allow_always", "Always allow", ACP_PERMISSION_ALLOW_ALWAYS},
    {"reject_once", "Reject", ACP_PERMISSION_REJECT_ONCE},
    {"reject_always", "Always reject", ACP_PERMISSION_REJECT_ALWAYS},
};

json_t *acp_permission_options(void)
{
    json_t *options = json_array();
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(*OPTIONS); i++)
        json_array_append_new(options, json_pack("{s:s, s:s, s:s}", "optionId", OPTIONS[i].id,
                                                 "name", OPTIONS[i].name, "kind", OPTIONS[i].id));
    return options;
}

enum acp_permission acp_permission_decision(const json_t *result)
{
    const json_t *outcome = json_object_get(result, "outcome");
    const char *kind = json_string_value(json_object_get(outcome, "outcome"));
    const char *chosen = json_string_value(json_object_get(outcome, "optionId"));
    if (!kind || strcmp(kind, "selected") != 0 || !chosen)
        return ACP_PERMISSION_REJECT_ONCE;
    for (size_t i = 0; i < sizeof(OPTIONS) / sizeof(*OPTIONS); i++)
        if (strcmp(chosen, OPTIONS[i].id) == 0)
            return OPTIONS[i].decision;
    return ACP_PERMISSION_REJECT_ONCE;
}
