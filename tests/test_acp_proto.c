/* SPDX-License-Identifier: MIT */
#include <jansson.h>
#include <stdlib.h>
#include <string.h>

#include "acp_proto.h"
#include "agent_core.h"
#include "agent_loop.h"
#include "harness.h"
#include "provider.h"
#include "transport/api_error.h"

static json_t *parse(const char *text)
{
    json_t *value = json_loads(text, 0, NULL);
    EXPECT(value != NULL);
    return value;
}

static const char *get_string(const json_t *object, const char *key)
{
    return json_string_value(json_object_get(object, key));
}

static void test_tool_kinds_and_permission(void)
{
    EXPECT_STR_EQ(acp_tool_kind("read"), "read");
    EXPECT_STR_EQ(acp_tool_kind("edit"), "edit");
    EXPECT_STR_EQ(acp_tool_kind("write"), "edit");
    EXPECT_STR_EQ(acp_tool_kind("bash"), "execute");
    EXPECT_STR_EQ(acp_tool_kind("task_wait"), "other");
    EXPECT_STR_EQ(acp_tool_kind(NULL), "other");

    EXPECT(!acp_tool_asks_permission("read", NULL));
    EXPECT(!acp_tool_asks_permission("task_wait", "{\"id\":\"t1\"}"));
    EXPECT(!acp_tool_asks_permission("task_wait", "{\"id\":\"t1\",\"kill\":false}"));
    /* A wait that kills stops a process; unreadable arguments are not trusted to be harmless. */
    EXPECT(acp_tool_asks_permission("task_wait", "{\"id\":\"t1\",\"kill\":true}"));
    EXPECT(acp_tool_asks_permission("task_wait", "not json"));
    EXPECT(acp_tool_asks_permission("bash", NULL));
    EXPECT(acp_tool_asks_permission("edit", NULL));
    EXPECT(acp_tool_asks_permission("write", NULL));
    EXPECT(acp_tool_asks_permission("some_host_tool", NULL));
    EXPECT(acp_tool_asks_permission(NULL, NULL));
}

static void test_prompt_text_joins_text_and_mentions_links(void)
{
    json_t *blocks =
        parse("[{\"type\":\"text\",\"text\":\"fix it\"},"
              "{\"type\":\"image\",\"data\":\"AAAA\"},"
              "{\"type\":\"resource_link\",\"uri\":\"file:///src/a.c\",\"name\":\"a\"},"
              "{\"type\":\"text\",\"text\":\"\"}]");
    char *text = acp_prompt_text(blocks);
    EXPECT_STR_EQ(text, "fix it\n\n@/src/a.c");
    free(text);
    json_decref(blocks);

    /* file:// paths are percent-decoded, localhost names this machine, and another host's or
     * scheme's URI stays as sent. */
    blocks = parse("[{\"type\":\"resource_link\",\"uri\":\"file:///a%20b/c%25d.c\"},"
                   "{\"type\":\"resource_link\",\"uri\":\"file://localhost/src/x.c\"},"
                   "{\"type\":\"resource_link\",\"uri\":\"file://box/src/y.c\"},"
                   "{\"type\":\"resource_link\",\"uri\":\"file:///z%00%4\"},"
                   "{\"type\":\"resource_link\",\"uri\":\"https://e.org/a%20b\"}]");
    text = acp_prompt_text(blocks);
    EXPECT_STR_EQ(text, "@/a b/c%d.c\n\n@/src/x.c\n\n@file://box/src/y.c\n\n@/z%00%4\n\n"
                        "@https://e.org/a%20b");
    free(text);
    json_decref(blocks);

    blocks = parse("[{\"type\":\"image\",\"data\":\"AAAA\"}]");
    EXPECT(acp_prompt_text(blocks) == NULL);
    json_decref(blocks);

    blocks = parse("{\"type\":\"text\",\"text\":\"not an array\"}");
    EXPECT(acp_prompt_text(blocks) == NULL);
    json_decref(blocks);
}

static void expect_decision(const char *result_json, enum acp_permission want)
{
    json_t *result = parse(result_json);
    EXPECT(acp_permission_decision(result) == want);
    json_decref(result);
}

static void test_permission_decisions_deny_unless_selected(void)
{
    expect_decision("{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"allow_once\"}}",
                    ACP_PERMISSION_ALLOW_ONCE);
    expect_decision("{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"allow_always\"}}",
                    ACP_PERMISSION_ALLOW_ALWAYS);
    expect_decision("{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"reject_always\"}}",
                    ACP_PERMISSION_REJECT_ALWAYS);
    expect_decision("{\"outcome\":{\"outcome\":\"cancelled\"}}", ACP_PERMISSION_REJECT_ONCE);
    expect_decision("{\"outcome\":{\"outcome\":\"selected\",\"optionId\":\"yes\"}}",
                    ACP_PERMISSION_REJECT_ONCE);
    expect_decision("{}", ACP_PERMISSION_REJECT_ONCE);
    EXPECT(acp_permission_decision(NULL) == ACP_PERMISSION_REJECT_ONCE);

    json_t *options = acp_permission_options();
    EXPECT(json_array_size(options) == 4);
    size_t i;
    json_t *option;
    json_array_foreach(options, i, option)
    {
        EXPECT_STR_EQ(get_string(option, "optionId"), get_string(option, "kind"));
        EXPECT(get_string(option, "name") != NULL);
    }
    json_decref(options);
}

static void test_stop_reasons(void)
{
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_COMPLETE, 0, NULL), "end_turn");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_MAX_TURNS, 0, NULL), "max_turn_requests");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_INTERRUPTED, 0, NULL), "cancelled");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_PAUSED, 0, NULL), "cancelled");
    EXPECT(acp_stop_reason(AGENT_LOOP_PROVIDER_ERROR, 0, NULL) == NULL);
    /* A cancel that lands as the turn finishes still reads as cancelled. */
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_COMPLETE, 1, NULL), "cancelled");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_PROVIDER_ERROR, 1, NULL), "cancelled");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_PROVIDER_ERROR, 1, "max_tokens"), "cancelled");
    /* The last request's cause names a truncation or refusal. */
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_PROVIDER_ERROR, 0, "max_tokens"), "max_tokens");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_COMPLETE, 0, "refusal"), "refusal");
    EXPECT_STR_EQ(acp_stop_reason(AGENT_LOOP_MAX_TURNS, 0, "refusal"), "max_turn_requests");
}

static void test_event_stop_reasons(void)
{
    struct stream_event event = {.kind = EV_DONE, .u.done = {.stop_reason = "end_turn"}};
    EXPECT(acp_event_stop_reason(&event) == NULL);
    event.u.done.stop_reason = "refusal";
    EXPECT_STR_EQ(acp_event_stop_reason(&event), "refusal");
    event.u.done.stop_reason = NULL;
    EXPECT(acp_event_stop_reason(&event) == NULL);

    event = (struct stream_event){.kind = EV_ERROR, .u.error = {.kind = API_ERROR_LENGTH_LIMIT}};
    EXPECT_STR_EQ(acp_event_stop_reason(&event), "max_tokens");
    event.u.error.kind = API_ERROR_OTHER;
    EXPECT(acp_event_stop_reason(&event) == NULL);

    event = (struct stream_event){.kind = EV_TEXT_DELTA, .u.text_delta = {.text = "x"}};
    EXPECT(acp_event_stop_reason(&event) == NULL);
}

/* Feed `delta` and expect `want` (NULL: nothing) to be sent. */
static void expect_echo(struct acp_echo *echo, const char *delta, const char *want)
{
    char *sent = acp_echo_take(echo, delta);
    if (want)
        EXPECT_STR_EQ(sent, want);
    else
        EXPECT(sent == NULL);
    free(sent);
}

static void test_echo_skips_what_a_retry_restreams(void)
{
    struct acp_echo echo;
    acp_echo_init(&echo);
    expect_echo(&echo, "Hello ", "Hello ");
    expect_echo(&echo, "wor", "wor");

    /* The retry re-streams the same text: only what follows the sent prefix goes out. */
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hel", NULL);
    expect_echo(&echo, "lo w", NULL);
    expect_echo(&echo, "orld", "ld");
    expect_echo(&echo, "!", "!");

    /* A second retry that dies early, then a third, still sends each byte once. */
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hello", NULL);
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hello world!", NULL);
    expect_echo(&echo, " Bye.", " Bye.");

    /* A retry that diverges sends its whole text after a paragraph break. */
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hello there", "\n\nHello there");
    expect_echo(&echo, ".", ".");
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hello there.", NULL);
    expect_echo(&echo, " Hi", " Hi");

    /* A new response starts empty. */
    acp_echo_reset(&echo);
    expect_echo(&echo, "Hello", "Hello");
    acp_echo_retry(&echo);
    expect_echo(&echo, "", NULL);
    expect_echo(&echo, "Help", "\n\nHelp");
    acp_echo_free(&echo);
}

static void expect_end(struct acp_echo *echo, int failed, const char *want)
{
    char *sent = acp_echo_end(echo, failed);
    if (want) {
        EXPECT(sent != NULL);
        if (sent)
            EXPECT_STR_EQ(sent, want);
    } else
        EXPECT(sent == NULL);
    free(sent);
}

static void test_echo_end_resends_a_shorter_retry_and_breaks_after_a_failure(void)
{
    struct acp_echo echo;
    acp_echo_init(&echo);

    /* A retry that ends on a prefix of the shown text is sent in full after a break. */
    expect_echo(&echo, "Hello world!", "Hello world!");
    acp_echo_retry(&echo);
    expect_echo(&echo, "Hello", NULL);
    expect_end(&echo, 0, "\n\nHello");
    /* A finished response owes the next one no break. */
    expect_echo(&echo, "Next", "Next");
    expect_end(&echo, 0, NULL);

    /* A failed response that showed text sets the next one apart, even across an empty one. */
    expect_echo(&echo, "The answer so", "The answer so");
    expect_end(&echo, 1, NULL);
    expect_end(&echo, 1, NULL);
    expect_echo(&echo, "", NULL);
    expect_echo(&echo, "The full", "\n\nThe full");
    expect_echo(&echo, " answer.", " answer.");
    expect_end(&echo, 0, NULL);
    expect_echo(&echo, "Then", "Then");

    /* A failure that showed nothing, or a new prompt, owes no break. */
    expect_end(&echo, 0, NULL);
    expect_end(&echo, 1, NULL);
    expect_echo(&echo, "A", "A");
    expect_end(&echo, 1, NULL);
    acp_echo_reset(&echo);
    expect_echo(&echo, "B", "B");
    acp_echo_free(&echo);
}

static void test_tool_call_carries_title_input_and_location(void)
{
    struct item call = {
        .kind = ITEM_TOOL_CALL,
        .call_id = "c1",
        .tool_name = "edit",
        .tool_arguments_json = "{\"path\":\"src/a.c\",\"old_string\":\"x\",\"new_string\":\"y\"}",
    };
    json_t *update = acp_tool_call(&call, "pending", "/work");
    EXPECT_STR_EQ(get_string(update, "sessionUpdate"), "tool_call");
    EXPECT_STR_EQ(get_string(update, "toolCallId"), "c1");
    EXPECT_STR_EQ(get_string(update, "title"), "edit src/a.c");
    EXPECT_STR_EQ(get_string(update, "kind"), "edit");
    EXPECT_STR_EQ(get_string(update, "status"), "pending");
    EXPECT_STR_EQ(get_string(json_object_get(update, "rawInput"), "new_string"), "y");
    json_t *locations = json_object_get(update, "locations");
    EXPECT(json_array_size(locations) == 1);
    EXPECT_STR_EQ(get_string(json_array_get(locations, 0), "path"), "/work/src/a.c");
    json_decref(update);

    struct item bash = {
        .kind = ITEM_TOOL_CALL,
        .call_id = "c2",
        .tool_name = "bash",
        .tool_arguments_json = "{\"command\":\"make\\nmake test\",\"path\":\"ignored\"}",
    };
    update = acp_tool_call(&bash, "pending", "/work");
    EXPECT_STR_EQ(get_string(update, "title"), "bash make");
    EXPECT(json_object_get(update, "locations") == NULL);
    json_decref(update);

    struct item malformed = {.kind = ITEM_TOOL_CALL,
                             .call_id = "c3",
                             .tool_name = "read",
                             .tool_arguments_json = "{not json"};
    update = acp_tool_call(&malformed, "pending", "/work");
    EXPECT_STR_EQ(get_string(update, "title"), "read");
    EXPECT(json_object_get(update, "rawInput") == NULL);
    json_decref(update);
}

static void test_tool_call_update_hides_the_model_only_tail(void)
{
    struct item result = {
        .kind = ITEM_TOOL_RESULT,
        .call_id = "c1",
        .output = "visible[note for the model]",
        .output_hidden_tail = strlen("[note for the model]"),
    };
    json_t *update = acp_tool_call_update("c1", "completed", &result);
    EXPECT_STR_EQ(get_string(update, "sessionUpdate"), "tool_call_update");
    EXPECT_STR_EQ(get_string(update, "status"), "completed");
    json_t *content = json_array_get(json_object_get(update, "content"), 0);
    EXPECT_STR_EQ(get_string(content, "type"), "content");
    EXPECT_STR_EQ(get_string(json_object_get(content, "content"), "text"), "visible");
    json_decref(update);

    update = acp_tool_call_update("c1", "in_progress", NULL);
    EXPECT(json_object_get(update, "content") == NULL);
    json_decref(update);
}

static void test_text_survives_malformed_utf8(void)
{
    json_t *text = acp_text("ok \xff end");
    EXPECT(json_is_string(text));
    EXPECT_STR_EQ(json_string_value(text), "ok \xef\xbf\xbd end");
    json_decref(text);
}

static void test_history_replays_visible_conversation(void)
{
    struct item items[] = {
        {.kind = ITEM_TURN_BOUNDARY},
        {.kind = ITEM_USER_MESSAGE, .text = "list files"},
        {.kind = ITEM_REASONING, .reasoning_text = "thinking"},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "Sure."},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c1",
         .tool_name = "bash",
         .tool_arguments_json = "{\"command\":\"ls\"}"},
        {.kind = ITEM_TOOL_CALL,
         .call_id = "c2",
         .tool_name = "bash",
         .tool_arguments_json = "{\"command\":\"pwd\"}"},
        {.kind = ITEM_TOOL_RESULT, .call_id = "c1", .output = "a.c"},
        {.kind = ITEM_TOOL_RESULT,
         .call_id = "c2",
         .output = INTERRUPT_MARKER,
         .origin = ITEM_ORIGIN_SKIPPED},
        {.kind = ITEM_TURN_USAGE},
        {.kind = ITEM_USER_MESSAGE, .text = "summary", .origin = ITEM_ORIGIN_COMPACT_SEED},
        {.kind = ITEM_USER_MESSAGE, .text = CONTINUE_MARKER, .origin = ITEM_ORIGIN_CONTINUATION},
    };
    json_t *updates = acp_history_updates(items, sizeof(items) / sizeof(*items), "/work");
    static const char *const want[][2] = {
        {"user_message_chunk", NULL},      {"agent_thought_chunk", NULL},
        {"agent_message_chunk", NULL},     {"tool_call", "completed"},
        {"tool_call_update", "completed"}, {"tool_call", "failed"},
        {"tool_call_update", "failed"},
    };
    EXPECT(json_array_size(updates) == sizeof(want) / sizeof(*want));
    for (size_t i = 0; i < json_array_size(updates) && i < sizeof(want) / sizeof(*want); i++) {
        json_t *update = json_array_get(updates, i);
        EXPECT_STR_EQ(get_string(update, "sessionUpdate"), want[i][0]);
        if (want[i][1])
            EXPECT_STR_EQ(get_string(update, "status"), want[i][1]);
    }
    EXPECT_STR_EQ(get_string(json_object_get(json_array_get(updates, 0), "content"), "text"),
                  "list files");
    json_decref(updates);
}

static void test_history_replays_a_cut_reply_as_it_streamed(void)
{
    struct item items[] = {
        {.kind = ITEM_USER_MESSAGE, .text = "explain"},
        {.kind = ITEM_ASSISTANT_MESSAGE,
         .text = "The answer so\n" INTERRUPT_MARKER,
         .origin = ITEM_ORIGIN_INTERRUPTED},
        {.kind = ITEM_USER_MESSAGE, .text = "summary", .origin = ITEM_ORIGIN_COMPACT_SEED},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "The full answer."},
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "More."},
        {.kind = ITEM_USER_MESSAGE, .text = "again"},
        {.kind = ITEM_ASSISTANT_MESSAGE,
         .text = INTERRUPT_MARKER,
         .origin = ITEM_ORIGIN_INTERRUPTED},
        /* A reply that legitimately ends on the marker keeps it. */
        {.kind = ITEM_ASSISTANT_MESSAGE, .text = "says\n" INTERRUPT_MARKER},
    };
    json_t *updates = acp_history_updates(items, sizeof(items) / sizeof(*items), "/work");
    static const char *const want[] = {
        "explain", "The answer so", "\n\nThe full answer.",
        "More.",   "again",         "says\n" INTERRUPT_MARKER,
    };
    EXPECT(json_array_size(updates) == sizeof(want) / sizeof(*want));
    for (size_t i = 0; i < json_array_size(updates) && i < sizeof(want) / sizeof(*want); i++)
        EXPECT_STR_EQ(get_string(json_object_get(json_array_get(updates, i), "content"), "text"),
                      want[i]);
    json_decref(updates);
}

int main(void)
{
    test_tool_kinds_and_permission();
    test_prompt_text_joins_text_and_mentions_links();
    test_permission_decisions_deny_unless_selected();
    test_stop_reasons();
    test_event_stop_reasons();
    test_echo_skips_what_a_retry_restreams();
    test_echo_end_resends_a_shorter_retry_and_breaks_after_a_failure();
    test_tool_call_carries_title_input_and_location();
    test_tool_call_update_hides_the_model_only_tail();
    test_text_survives_malformed_utf8();
    test_history_replays_visible_conversation();
    test_history_replays_a_cut_reply_as_it_streamed();
    T_REPORT();
}
