/* SPDX-License-Identifier: MIT */
/* Stress test for several agents in one process, driven through the shared libhax as an outside
 * C++ host would drive it. N agents run R rounds each on their own threads, against scripted mock
 * providers. Each round's response calls a host tool and the built-in bash together. Every K-th
 * agent is cancelled inside a blocking host tool in round 2, then resends the round. Exits 0 when
 * every agent kept to its own conversation, tools, and cancellation.
 *
 *   meson setup build-embed -Dembed=true && meson compile -C build-embed
 *   build-embed/parallel_agents [--agents N] [--rounds R] [--cancel-every K] [--no-bash]
 *
 * --no-bash drops the bash calls, so no thread forks. ThreadSanitizer on macOS needs it: its fork
 * interceptor takes the runtime's locks before libc's atfork handlers take theirs, and a thread
 * inside snprintf holding a libc lock then deadlocks the process.
 *
 * Construction runs on the main thread, because configuration is foreground state; only the runs
 * are concurrent. See "Lifecycle" in docs/embedding.md. */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <jansson.h>
#include <latch>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "agent_core.h"
#include "agent_loop.h"
#include "agent_tool.h"
#include "config.h"
#include "diag.h"
#include "embed_host.hpp"
#include "hax_embed.h"
#include "provider.h"
#include "system/cancel.h"

namespace
{

using Clock = std::chrono::steady_clock;

/* How long anything waits for a cancel that should arrive within milliseconds. */
constexpr auto HOLD_LIMIT = std::chrono::seconds(10);

struct Options {
    int agents = 16;
    int rounds = 4;
    int cancel_every = 4; /* 0 cancels none */
    bool bash = true;     /* false forks nothing; see --no-bash in the header comment */
};

/* Host state every agent's tool writes: one count per agent, and any call that named the wrong
 * agent. */
struct Ledger {
    std::mutex lock;
    std::vector<int> records;
    int cross_talk = 0;
};

struct Agent {
    int index = 0;
    std::string tag; /* "<A03>": in every tool argument and output this agent produces */
    bool cancel_target = false;
    std::string script_path;
    Ledger *ledger = nullptr;

    embed_host::Agent host;
    std::atomic<bool> holding{false};
    std::atomic<bool> hold_timed_out{false};
    std::vector<enum agent_loop_outcome> outcomes;
};

std::mutex diag_lock;

void diag_sink(enum hax_diag_level level, const char *message, void * /* user */)
{
    const std::lock_guard<std::mutex> guard(diag_lock);
    std::fprintf(stderr, "hax[%d]: %s\n", static_cast<int>(level), message);
}

/* One round: text, a host tool call and a bash call in one response, then a closing message. The
 * cancel target's round 2 starts with a response that only calls `hold`. */
std::string script_for(const Agent &agent, const Options &options)
{
    const int rounds = options.rounds;
    std::string script;
    for (int round = 1; round <= rounds; round++) {
        const std::string r = std::to_string(round);
        if (agent.cancel_target && round == 2)
            script += "tool hold {}\nend-turn\n\n";
        script += "text " + agent.tag + " round " + r + "\n";
        script += "tool record {\"agent\":\"" + agent.tag + "\",\"round\":" + r + "}\n";
        if (options.bash)
            script += "tool bash {\"command\":\"echo " + agent.tag + "-r" + r + "\"}\n";
        script += "end-turn\n\n";
        script += "text " + agent.tag + " round " + r + " done\nend-turn\n\n";
    }
    return script;
}

struct item record_tool(const Agent &agent, const struct item *call)
{
    json_t *arguments = json_loads(call->tool_arguments_json, 0, nullptr);
    const char *named = json_string_value(json_object_get(arguments, "agent"));
    const bool mine = named && agent.tag == named;
    int count = 0;
    {
        const std::lock_guard<std::mutex> guard(agent.ledger->lock);
        if (mine)
            count = ++agent.ledger->records[agent.index];
        else
            agent.ledger->cross_talk++;
    }
    json_decref(arguments);
    const std::string output = mine ? "recorded " + agent.tag + " #" + std::to_string(count)
                                    : std::string("error: this tool belongs to ") + agent.tag;
    return embed_host::host_result(call, output.c_str());
}

/* Block until this agent, and only this agent, is cancelled. */
struct item hold_tool(Agent &agent, const struct item *call)
{
    agent.holding = true;
    const auto deadline = Clock::now() + HOLD_LIMIT;
    while (!cancel_state_abort_requested(&agent.host.cancel)) {
        if (Clock::now() > deadline) {
            agent.hold_timed_out = true;
            return agent_tool_result_make(call, "error: no cancel arrived", nullptr);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return embed_host::result_with_origin(call, INTERRUPT_MARKER, ITEM_ORIGIN_INTERRUPTED);
}

/* Configuration is copied at construction, so this runs on the main thread, one agent at a
 * time. */
bool construct(Agent &agent)
{
    config_set_override("providers.mock.script", agent.script_path.c_str());
    if (!embed_host::open(agent.host, "mock"))
        return false;
    /* The host's tools by name, hax's own for the rest. */
    agent.host.tool = [&agent](const struct item *call, int image_input) {
        const std::string_view name = call->tool_name ? call->tool_name : "";
        if (name == "record")
            return record_tool(agent, call);
        if (name == "hold")
            return hold_tool(agent, call);
        return embed_host::builtin_result(agent.host, call, image_input);
    };

    /* One tool through `params`, one through `schema_json`, so both advertisement paths run. */
    static const struct tool_param record_params[] = {
        {"agent", "string", nullptr, "The agent's tag.", 1, 0},
        {"round", "integer", nullptr, "The round number.", 1, 1},
    };
    struct tool_def record = {};
    record.name = "record";
    record.description = "Record that this agent finished a round.";
    record.params = record_params;
    record.n_params = 2;
    struct tool_def hold = {};
    hold.name = "hold";
    hold.description = "Wait until cancelled.";
    hold.schema_json = "{\"type\":\"object\",\"properties\":{}}";
    return agent_session_add_tool(&agent.host.session, &record) == 0 &&
           agent_session_add_tool(&agent.host.session, &hold) == 0;
}

void run(Agent &agent, int rounds, std::latch &start)
{
    start.arrive_and_wait();
    for (int round = 1; round <= rounds; round++) {
        const std::string prompt = agent.tag + " round " + std::to_string(round);
        agent.outcomes.push_back(embed_host::send(agent.host, prompt, 8).outcome);
        if (agent.cancel_target && round == 2)
            agent.outcomes.push_back(embed_host::send(agent.host, prompt + " again", 8).outcome);
    }
}

bool history_has(const Agent &agent, const std::string &needle)
{
    for (size_t i = 0; i < agent.host.session.n_items; i++) {
        const struct item &item = agent.host.session.items[i];
        for (const char *text : {item.text, item.output, item.tool_arguments_json})
            if (text && std::strstr(text, needle.c_str()))
                return true;
    }
    return false;
}

/* Every expectation this agent failed, one line each. */
std::vector<std::string> check(const Agent &agent, const std::vector<Agent> &all,
                               const Options &options)
{
    const int rounds = options.rounds;
    std::vector<std::string> failures;
    const auto fail = [&](const std::string &what) { failures.push_back(agent.tag + " " + what); };

    std::vector<enum agent_loop_outcome> expected(rounds, AGENT_LOOP_COMPLETE);
    if (agent.cancel_target && rounds >= 2)
        expected.insert(expected.begin() + 1, AGENT_LOOP_INTERRUPTED);
    if (agent.outcomes != expected)
        fail("ended its sends with unexpected outcomes");
    if (agent.hold_timed_out)
        fail("waited out its hold: the cancel never reached it");
    if (agent.ledger->records[agent.index] != rounds)
        fail("recorded " + std::to_string(agent.ledger->records[agent.index]) + " rounds, not " +
             std::to_string(rounds));

    for (int round = 1; options.bash && round <= rounds; round++)
        if (!history_has(agent, agent.tag + "-r" + std::to_string(round)))
            fail("lacks its own bash output for round " + std::to_string(round));
    for (const Agent &other : all)
        if (&other != &agent && history_has(agent, other.tag))
            fail("holds " + other.tag + "'s conversation");

    std::set<std::string> calls;
    std::set<std::string> results;
    for (size_t i = 0; i < agent.host.session.n_items; i++) {
        const struct item &item = agent.host.session.items[i];
        if (item.kind == ITEM_TOOL_CALL && item.call_id)
            calls.insert(item.call_id);
        else if (item.kind == ITEM_TOOL_RESULT && item.call_id)
            results.insert(item.call_id);
    }
    if (calls != results)
        fail("has a tool call without a result, or a result without a call");
    return failures;
}

bool parse_options(int argc, char *const *argv, Options &options)
{
    for (int i = 1; i < argc; i++) {
        const std::string_view flag = argv[i];
        if (flag == "--no-bash") {
            options.bash = false;
            continue;
        }
        if (i + 1 == argc)
            return false;
        const int value = std::atoi(argv[++i]);
        if (flag == "--agents" && value > 0)
            options.agents = value;
        else if (flag == "--rounds" && value > 0)
            options.rounds = value;
        else if (flag == "--cancel-every" && value >= 0)
            options.cancel_every = value;
        else
            return false;
    }
    return true;
}

int stress(const Options &options)
{
    char scratch[] = "/tmp/hax-parallel-XXXXXX";
    if (!mkdtemp(scratch)) {
        std::perror("mkdtemp");
        return 1;
    }

    struct hax_embed_options init = {};
    init.own_curl_global = 1;
    init.diag = diag_sink;
    if (hax_init(&init) != 0)
        return 1;
    config_set_override("provider", "mock");
    config_set_override("no_session", "1");

    Ledger ledger;
    ledger.records.assign(options.agents, 0);
    std::vector<Agent> agents(options.agents);
    int constructed = 0;
    for (int i = 0; i < options.agents; i++) {
        Agent &agent = agents[i];
        char tag[16];
        std::snprintf(tag, sizeof(tag), "<A%02d>", i);
        agent.index = i;
        agent.tag = tag;
        agent.cancel_target = options.cancel_every > 0 && i % options.cancel_every == 0;
        agent.ledger = &ledger;
        agent.script_path = std::string(scratch) + "/agent" + std::to_string(i) + ".txt";
        FILE *file = std::fopen(agent.script_path.c_str(), "w");
        if (!file || std::fputs(script_for(agent, options).c_str(), file) < 0) {
            std::perror(agent.script_path.c_str());
            break;
        }
        std::fclose(file);
        if (!construct(agent))
            break;
        constructed++;
    }

    int status = 1;
    if (constructed == options.agents) {
        std::latch start(options.agents + 1);
        std::vector<std::thread> threads;
        threads.reserve(agents.size());
        for (Agent &agent : agents)
            threads.emplace_back(run, std::ref(agent), options.rounds, std::ref(start));
        const auto started = Clock::now();
        start.arrive_and_wait();

        /* Cancel each target once it is inside its hold; its siblings keep running. */
        for (Agent &agent : agents) {
            if (!agent.cancel_target || options.rounds < 2)
                continue;
            /* Bounded: an agent that failed before its hold must not wedge the run. */
            const auto deadline = Clock::now() + HOLD_LIMIT;
            while (!agent.holding && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancel_state_request_abort(&agent.host.cancel);
        }
        for (std::thread &thread : threads)
            thread.join();
        const double seconds = std::chrono::duration<double>(Clock::now() - started).count();

        std::vector<std::string> failures;
        for (const Agent &agent : agents)
            for (std::string &failure : check(agent, agents, options))
                failures.push_back(std::move(failure));
        if (ledger.cross_talk)
            failures.push_back(std::to_string(ledger.cross_talk) +
                               " tool calls named another agent");
        if (cancel_abort_requested())
            failures.push_back("a per-agent cancel reached the process state");

        size_t sends = 0;
        size_t cancelled = 0;
        for (const Agent &agent : agents) {
            sends += agent.outcomes.size();
            cancelled += agent.cancel_target && options.rounds >= 2;
        }
        for (const std::string &failure : failures)
            std::fprintf(stderr, "FAIL %s\n", failure.c_str());
        std::printf("%d agents x %d rounds: %zu sends in %.2f s, %zu cancelled and resumed: %s\n",
                    options.agents, options.rounds, sends, seconds, cancelled,
                    failures.empty() ? "ok" : "FAILED");
        status = failures.empty() ? 0 : 1;
    }

    for (Agent &agent : agents) {
        embed_host::close(agent.host);
        std::remove(agent.script_path.c_str());
    }
    rmdir(scratch);
    hax_shutdown();
    return status;
}

} // namespace

int main(int argc, char *const *argv)
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr, "usage: %s [--agents N] [--rounds R] [--cancel-every K] [--no-bash]\n",
                     argv[0]);
        return 2;
    }
    try {
        return stress(options);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
