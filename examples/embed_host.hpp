/* SPDX-License-Identifier: MIT */
/* What the C++ examples share for driving libhax: one agent's provider, session and cancel flag,
 * a send that returns the final reply, and the tool-call hook. "C++ hosts" in docs/embedding.md
 * covers including hax's headers from C++. */
#ifndef HAX_EXAMPLES_EMBED_HOST_HPP
#define HAX_EXAMPLES_EMBED_HOST_HPP

#include <cstdlib>
#include <functional>
#include <string>

#include "agent_core.h"
#include "agent_loop.h"
#include "agent_stream.h"
#include "agent_tool.h"
#include "hax_embed.h"
#include "provider.h"
#include "tool.h"
#include "system/cancel.h"
#include "tools/output_cap.h"

namespace embed_host
{

struct Agent {
    struct provider *provider = nullptr;
    struct agent_session session = {};
    struct cancel_state cancel = {};
    /* Runs each call the loop lets through; empty runs hax's own tools. Must not throw. */
    std::function<struct item(const struct item *call, int image_input)> tool;
};

struct Reply {
    enum agent_loop_outcome outcome = AGENT_LOOP_PROVIDER_ERROR;
    std::string text;  /* the final turn's assistant messages */
    std::string error; /* the provider's message, when there was one */
};

inline struct item result_with_origin(const struct item *call, const char *output,
                                      enum item_origin origin)
{
    struct item result = agent_tool_result_make(call, output, nullptr);
    result.origin = origin;
    return result;
}

/* A result from text the host produced, held to tool_output_cap as hax's own tools are. Built-in
 * results come from builtin_result instead, already capped. */
inline struct item host_result(const struct item *call, const char *text)
{
    char *capped = output_cap_clip(text, output_cap_bytes());
    struct item result = agent_tool_result_make(call, capped, nullptr);
    std::free(capped);
    return result;
}

/* Run `call` with hax's own implementation, under this agent's cancel flag and environment. */
inline struct item builtin_result(Agent &agent, const struct item *call, int image_input)
{
    struct tool_run_ctx ctx = {};
    ctx.env_selection = &agent.session.env_selection;
    ctx.cancel = &agent.cancel;
    ctx.image_input = image_input;
    struct agent_tool_call prepared;
    agent_tool_call_init(&prepared, call);
    char *output = agent_tool_call_run(&prepared, &ctx);
    struct item result = agent_tool_result_make(call, output, &ctx);
    std::free(output);
    agent_tool_call_destroy(&prepared);
    return result;
}

/* The loop's tool_call hook. A skipped or refused call gets the text and origin hax itself
 * writes; see "Tool calls from the host" in docs/embedding.md. */
inline struct item on_tool_call(const struct item *call, enum agent_loop_tool_action action,
                                int image_input, void *user)
{
    Agent &agent = *static_cast<Agent *>(user);
    if (action == AGENT_LOOP_TOOL_SKIP)
        return result_with_origin(call, INTERRUPT_MARKER, ITEM_ORIGIN_SKIPPED);
    if (action == AGENT_LOOP_TOOL_REFUSE)
        return result_with_origin(call, REFUSED_RESULT, ITEM_ORIGIN_REFUSED);
    if (agent.tool)
        return agent.tool(call, image_input);
    return builtin_result(agent, call, image_input);
}

/* agent_signal_cb fixes the parameter as void *, though this hook only reads through it. */
inline enum agent_signal on_signal(void *user) // NOLINT(misc-const-correctness)
{
    const Agent &agent = *static_cast<const Agent *>(user);
    if (cancel_state_abort_requested(&agent.cancel))
        return AGENT_SIGNAL_ABORT;
    if (cancel_state_pause_requested(&agent.cancel))
        return AGENT_SIGNAL_PAUSE;
    return AGENT_SIGNAL_NONE;
}

/* Build the agent from the configuration current now; NULL `provider` takes the configured one.
 * Configuration is foreground state, so call this on the main thread. */
inline bool open(Agent &agent, const char *provider)
{
    agent.provider = hax_provider_new(provider);
    if (!agent.provider)
        return false;
    static const struct hax_opts opts = {};
    agent_session_init(&agent.session, agent.provider, &opts);
    return true;
}

inline void close(Agent &agent)
{
    if (!agent.provider)
        return;
    agent_session_free(&agent.session);
    hax_provider_destroy(agent.provider);
    agent.provider = nullptr;
}

/* One user turn. Safe to run on a worker thread once the agent is open. */
inline Reply send(Agent &agent, const std::string &prompt, int max_turns)
{
    cancel_state_clear(&agent.cancel);
    agent_session_add_user(&agent.session, prompt.c_str());
    struct agent_loop_params params = {};
    params.session = &agent.session;
    params.provider = agent.provider;
    params.cancel = &agent.cancel;
    params.max_turns = max_turns;
    params.hooks.user = &agent;
    params.hooks.poll = on_signal;
    params.hooks.checkpoint = on_signal;
    params.hooks.tool_call = on_tool_call;
    struct agent_loop_result result = {};
    agent_loop_run(&params, &result);

    Reply reply;
    reply.outcome = result.outcome;
    if (result.error_message)
        reply.error = result.error_message;
    for (size_t i = result.final_items_from; i < result.final_items_to; i++) {
        const struct item &item = agent.session.items[i];
        if (item.kind != ITEM_ASSISTANT_MESSAGE || !item.text || !*item.text)
            continue;
        if (!reply.text.empty())
            reply.text += "\n\n";
        reply.text += item.text;
    }
    agent_loop_result_destroy(&result);
    return reply;
}

inline const char *outcome_name(enum agent_loop_outcome outcome)
{
    switch (outcome) {
    case AGENT_LOOP_COMPLETE:
        return "complete";
    case AGENT_LOOP_PROVIDER_ERROR:
        return "provider error";
    case AGENT_LOOP_INTERRUPTED:
        return "cancelled";
    case AGENT_LOOP_PAUSED:
        return "paused";
    case AGENT_LOOP_MAX_TURNS:
        return "turn limit reached";
    case AGENT_LOOP_BUDGET:
        return "budget spent";
    }
    return "unknown outcome";
}

} // namespace embed_host

#endif /* HAX_EXAMPLES_EMBED_HOST_HPP */
