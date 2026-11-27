/* SPDX-License-Identifier: MIT */
/* Answer one objective by splitting it across agents that run in parallel.
 *
 * The input is a Markdown document. Its one `#` heading is the main objective or question, and
 * each `##` heading is a part of it. Text under a heading is context for that heading's agent.
 * Headings inside fenced code blocks are ignored; setext (underlined) headings are not read.
 *
 * A line `depends: Title, Other title` under a `##` heading makes that part wait for the named
 * parts and hands it their reports. Titles match ignoring case. A part whose dependency failed
 * is skipped. Unknown titles and cycles are refused before anything runs.
 *
 * One agent works on each part, at most --jobs at a time, starting each as soon as its
 * dependencies have finished. When every part has returned, a final agent addresses the main
 * objective from their reports. stdout gets a Markdown report: the
 * answer under the `#` heading, then each part's report under its own heading. Progress goes to
 * stderr.
 *
 *   meson setup build-embed -Dembed=true && meson compile -C build-embed
 *   build-embed/decompose [--provider ID] [--model ID] [--jobs N] [--max-turns N]
 *                         [--tools none|read|all] [--dry-run] FILE
 *
 * --tools read, the default, lets agents read files but refuses edit, write and bash: an
 * embedded agent asks no permission, so anything allowed runs unattended. --dry-run prints the
 * prompts each agent would receive and runs nothing. Provider setup is hax's own. Ctrl-C cancels
 * every agent and prints what finished; a second Ctrl-C exits at once.
 *
 * An agent that reaches the turn limit gets one more turn, without tools, to report what it found;
 * the report says it was written at the limit. The total cost goes to stderr at the end.
 *
 * Exit status: 0 when every agent answered, 1 when any failed, 2 for a usage or document error,
 * 130 when interrupted. */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <istream>
#include <mutex>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "agent_loop.h"
#include "agent_stats.h"
#include "config.h"
#include "diag.h"
#include "embed_host.hpp"
#include "hax_embed.h"
#include "provider.h"
#include "system/cancel.h"

namespace
{

using Clock = std::chrono::steady_clock;

struct Section {
    std::string title;
    std::string context;
    std::vector<size_t> depends; /* indexes of the parts this one waits for */
};

struct Document {
    Section objective;
    std::vector<Section> parts;
};

enum class Tools : std::uint8_t { None, Read, All };

struct Options {
    std::string provider; /* empty: the configured provider */
    std::string model;    /* empty: the provider's configured model */
    int jobs = 4;
    int max_turns = 30;
    Tools tools = Tools::Read;
    bool dry_run = false;
    std::string path;
};

/* One finished part, or the synthesis. */
struct Result {
    embed_host::Reply reply;
    double seconds = 0;
    bool ran = false;
    bool at_limit = false; /* written in the extra turn after the turn limit */
    std::string skipped;   /* why a part never ran, when a dependency failed */
};

/* --- Parsing --- */

std::string_view trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/* The level of an ATX heading (1-6), with its text in `title`; 0 for any other line. */
int heading_level(std::string_view line, std::string &title)
{
    size_t at = 0;
    while (at < 3 && at < line.size() && line[at] == ' ')
        at++;
    int level = 0;
    while (at < line.size() && line[at] == '#' && level < 7) {
        at++;
        level++;
    }
    if (level == 0 || level > 6 || (at < line.size() && line[at] != ' ' && line[at] != '\t'))
        return 0;
    std::string_view text = trim(line.substr(at));
    /* An optional closing run of '#', when a space separates it from the text. */
    const size_t hashes = text.find_last_not_of('#');
    if (hashes == std::string_view::npos)
        text = {};
    else if (hashes + 1 < text.size() && (text[hashes] == ' ' || text[hashes] == '\t'))
        text = trim(text.substr(0, hashes));
    title = text;
    return level;
}

/* A code fence of at least three backticks or tildes: its character and length, or 0. */
size_t fence_length(std::string_view line, char &marker)
{
    size_t at = 0;
    while (at < 3 && at < line.size() && line[at] == ' ')
        at++;
    if (at >= line.size() || (line[at] != '`' && line[at] != '~'))
        return 0;
    marker = line[at];
    const size_t end = line.find_first_not_of(marker, at);
    const size_t length = (end == std::string_view::npos ? line.size() : end) - at;
    return length >= 3 ? length : 0;
}

/* A `depends:` line, case-insensitive; the names after the colon go in `names`. */
bool depends_line(std::string_view line, std::string &names)
{
    constexpr std::string_view KEY = "depends:";
    const std::string_view text = trim(line);
    if (text.size() < KEY.size())
        return false;
    for (size_t i = 0; i < KEY.size(); i++)
        if (std::tolower(static_cast<unsigned char>(text[i])) != KEY[i])
            return false;
    names = trim(text.substr(KEY.size()));
    return true;
}

std::string lowercase(std::string_view text)
{
    std::string lower(text);
    for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

/* One name from a `depends:` line, before it is resolved to a part. */
struct Reference {
    size_t part;
    std::string name;
    int line;
};

/* Turn names into part indexes, then refuse a cycle. */
bool resolve(Document &document, const std::vector<Reference> &references, std::string &error)
{
    const size_t n = document.parts.size();
    for (const Reference &reference : references) {
        const std::string where = "line " + std::to_string(reference.line) + ": ";
        size_t found = n;
        size_t matches = 0;
        for (size_t i = 0; i < n; i++) {
            if (lowercase(document.parts[i].title) == lowercase(reference.name)) {
                found = i;
                matches++;
            }
        }
        if (matches != 1) {
            error = where + "depends on '" + reference.name + "', which " +
                    (matches ? "names more than one part" : "is not a part title");
            return false;
        }
        if (found == reference.part) {
            error = where + "a part cannot depend on itself";
            return false;
        }
        std::vector<size_t> &depends = document.parts[reference.part].depends;
        if (std::find(depends.begin(), depends.end(), found) == depends.end())
            depends.push_back(found);
    }

    /* Kahn's algorithm: whatever never becomes ready sits on or behind a cycle. */
    std::vector<size_t> waiting(n);
    for (size_t i = 0; i < n; i++)
        waiting[i] = document.parts[i].depends.size();
    std::vector<bool> done(n, false);
    for (size_t progress = 1; progress;) {
        progress = 0;
        for (size_t i = 0; i < n; i++) {
            if (done[i] || waiting[i])
                continue;
            done[i] = true;
            progress++;
            for (size_t j = 0; j < n; j++)
                for (const size_t dependency : document.parts[j].depends)
                    waiting[j] -= dependency == i;
        }
    }
    std::string stuck;
    for (size_t i = 0; i < n; i++)
        if (!done[i])
            stuck += (stuck.empty() ? "'" : ", '") + document.parts[i].title + "'";
    if (!stuck.empty()) {
        error = "the dependencies form a cycle among " + stuck;
        return false;
    }
    return true;
}

/* Fill `document` from Markdown. Returns false with a message naming the line at fault. */
bool parse(std::istream &input, Document &document, std::string &error)
{
    std::string line;
    std::string title;
    std::string *body = nullptr; /* the section the current text belongs to */
    int objective_line = 0;
    std::vector<Reference> references;
    char fence = 0;
    size_t fence_open = 0;

    for (int number = 1; std::getline(input, line); number++) {
        char marker = 0;
        const size_t length = fence_length(line, marker);
        if (fence_open) {
            if (length >= fence_open && marker == fence && trim(line).size() == length)
                fence_open = 0;
        } else if (length) {
            fence = marker;
            fence_open = length;
        } else if (const int level = heading_level(line, title); level == 1 || level == 2) {
            const std::string where = "line " + std::to_string(number) + ": ";
            if (title.empty()) {
                error = where + "a heading needs text";
                return false;
            }
            if (level == 1) {
                if (objective_line) {
                    error = where + "a second '#' heading; the document has one main objective, " +
                            "first given on line " + std::to_string(objective_line);
                    return false;
                }
                objective_line = number;
                document.objective.title = title;
                body = &document.objective.context;
            } else {
                if (!objective_line) {
                    error = where;
                    error += "'## " + title + "' comes before the '#' main objective";
                    return false;
                }
                document.parts.push_back({title, {}, {}});
                body = &document.parts.back().context;
            }
            continue;
        } else if (std::string names; body && depends_line(line, names)) {
            const std::string where = "line " + std::to_string(number) + ": ";
            if (document.parts.empty()) {
                error = where + "'depends:' belongs under a '##' part";
                return false;
            }
            for (size_t start = 0; start <= names.size();) {
                const size_t comma = std::min(names.find(',', start), names.size());
                const std::string_view name =
                    trim(std::string_view(names).substr(start, comma - start));
                if (name.empty()) {
                    error = where + "'depends:' needs part titles separated by commas";
                    return false;
                }
                references.push_back({document.parts.size() - 1, std::string(name), number});
                start = comma + 1;
            }
            continue;
        }
        if (body)
            *body += line + "\n";
    }

    if (!objective_line) {
        error = "no '#' heading: the document needs one main objective or question";
        return false;
    }
    if (document.parts.empty()) {
        error = "no '##' headings: the document needs at least one part to hand out";
        return false;
    }
    document.objective.context = trim(document.objective.context);
    for (Section &part : document.parts)
        part.context = trim(part.context);
    return resolve(document, references, error);
}

/* --- Prompts --- */

void append_section(std::string &prompt, const char *label, const Section &section)
{
    prompt += label;
    prompt += section.title + "\n";
    if (!section.context.empty())
        prompt += "\n" + section.context + "\n";
}

bool succeeded(const Result &result)
{
    return result.ran && result.reply.outcome == AGENT_LOOP_COMPLETE && !result.reply.text.empty();
}

/* The prompt for part `index`. `parts` holds the reports of the parts it depends on. */
std::string part_prompt(const Document &document, size_t index, const std::vector<Result> &parts)
{
    std::string prompt =
        "You are one of " + std::to_string(document.parts.size()) +
        " agents, each working on one part of a larger objective. Work only on "
        "your part; the others cover the rest, and a further agent will combine the reports.\n\n";
    append_section(prompt, "Overall objective: ", document.objective);
    prompt += '\n';
    append_section(prompt, "Your part: ", document.parts[index]);
    const std::vector<size_t> &depends = document.parts[index].depends;
    if (!depends.empty()) {
        prompt += "\nYour part builds on these parts, which are finished. Their reports follow; "
                  "use them rather than redoing their work.\n";
        for (const size_t d : depends)
            prompt += "\n### Part " + std::to_string(d + 1) + ": " + document.parts[d].title +
                      "\n\n" + parts[d].reply.text + "\n";
    }
    prompt += "\nEnd with a self-contained report on your part: what you established, how you "
              "established it, and what remains uncertain. Do not address the other parts.\n";
    return prompt;
}

std::string failure_note(const embed_host::Reply &reply)
{
    std::string note = embed_host::outcome_name(reply.outcome);
    if (!reply.error.empty())
        note += ": " + reply.error;
    else if (reply.outcome == AGENT_LOOP_COMPLETE)
        note = "the agent gave no reply text";
    return note;
}

/* Why a result has no report. */
std::string missing_note(const Result &result)
{
    if (!result.skipped.empty())
        return result.skipped;
    if (!result.ran)
        return "not started";
    return failure_note(result.reply);
}

std::string synthesis_prompt(const Document &document, const std::vector<Result> &parts)
{
    std::string prompt =
        "Several agents each worked on one part of the objective below. Their "
        "reports follow. Address the overall objective directly, using the reports as your "
        "evidence. Reconcile overlaps, point out where reports contradict each other, and say "
        "which parts failed or left gaps. Redo a part's work only where its report is missing "
        "or contradicted.\n\n";
    append_section(prompt, "Objective: ", document.objective);
    for (size_t i = 0; i < document.parts.size(); i++) {
        prompt += "\n## Part " + std::to_string(i + 1) + ": " + document.parts[i].title + "\n\n";
        if (parts[i].at_limit)
            prompt += "(This agent reached its turn limit before finishing; its report says what "
                      "it did not get to.)\n\n";
        if (succeeded(parts[i]))
            prompt += parts[i].reply.text + "\n";
        else
            prompt += "(no report: " + missing_note(parts[i]) + ")\n";
    }
    return prompt;
}

/* --- Running --- */

/* Everything a Ctrl-C needs to reach. Written before the handler is installed. */
std::vector<embed_host::Agent> *signal_agents = nullptr;
volatile std::sig_atomic_t interrupted = 0;

/* Cancels every agent. SA_RESETHAND makes a second Ctrl-C exit at once. */
void on_interrupt(int /* signal */)
{
    interrupted = 1;
    for (embed_host::Agent &agent : *signal_agents)
        cancel_state_request_abort(&agent.cancel);
}

std::mutex output_lock;

void diag_sink(enum hax_diag_level level, const char *message, void * /* user */)
{
    (void)level;
    const std::lock_guard<std::mutex> guard(output_lock);
    std::fprintf(stderr, "hax: %s\n", message);
}

/* Refuse what --tools leaves out; run hax's own implementation for the rest. */
void apply_tool_policy(embed_host::Agent &agent, Tools tools)
{
    agent.tool = [&agent, tools](const struct item *call, int image_input) {
        const std::string_view name = call->tool_name ? call->tool_name : "";
        if (tools == Tools::All || (tools == Tools::Read && name == "read"))
            return embed_host::builtin_result(agent, call, image_input);
        const char *output = tools == Tools::Read
                                 ? "error: only the read tool is enabled in this run"
                                 : "error: tools are disabled in this run";
        return embed_host::result_with_origin(call, output, ITEM_ORIGIN_REFUSED);
    };
}

const char AT_LIMIT_NOTE[] = "_Written at the turn limit, before the agent finished._\n\n";

void print_report(const Document &document, const std::vector<Result> &parts,
                  const Result &synthesis)
{
    std::printf("# %s\n\n", document.objective.title.c_str());
    if (synthesis.at_limit)
        std::fputs(AT_LIMIT_NOTE, stdout);
    if (succeeded(synthesis))
        std::printf("%s\n", synthesis.reply.text.c_str());
    else if (!synthesis.ran)
        std::printf("_No answer: the final agent did not run._\n");
    else
        std::printf("_No answer: %s._\n", failure_note(synthesis.reply).c_str());
    for (size_t i = 0; i < parts.size(); i++) {
        std::printf("\n## %s\n\n", document.parts[i].title.c_str());
        if (parts[i].at_limit)
            std::fputs(AT_LIMIT_NOTE, stdout);
        if (succeeded(parts[i]))
            std::printf("%s\n", parts[i].reply.text.c_str());
        else
            std::printf("_No report: %s._\n", missing_note(parts[i]).c_str());
    }
}

Result timed_send(embed_host::Agent &agent, const std::string &prompt, int max_turns)
{
    const auto started = Clock::now();
    Result result;
    result.reply = embed_host::send(agent, prompt, max_turns);
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    result.ran = true;
    return result;
}

const char WRAP_UP_PROMPT[] =
    "You have reached the turn limit and can no longer call tools. Write your report now from "
    "what you have found, and say what you did not get to.";

/* Run `prompt`; an agent that reaches the turn limit gets one more turn, without tools, to write
 * up what it found rather than lose it. A failed wrap-up leaves the turn-limit result. */
Result run_agent(embed_host::Agent &agent, const std::string &prompt, const Options &options)
{
    Result result = timed_send(agent, prompt, options.max_turns);
    if (result.reply.outcome != AGENT_LOOP_MAX_TURNS || interrupted)
        return result;
    apply_tool_policy(agent, Tools::None);
    /* Two turns: a call the model makes anyway is refused, and it answers on the next. */
    Result wrapped = timed_send(agent, WRAP_UP_PROMPT, 2);
    apply_tool_policy(agent, options.tools);
    if (!succeeded(wrapped))
        return result;
    wrapped.seconds += result.seconds;
    wrapped.at_limit = true;
    return wrapped;
}

void progress(const char *label, const std::string &title, const Result &result)
{
    const char *state = result.at_limit ? "done at the turn limit" : "done";
    const std::lock_guard<std::mutex> guard(output_lock);
    std::fprintf(stderr, "%s %s: %s (%.1f s)\n", label, title.c_str(),
                 succeeded(result) ? state : missing_note(result).c_str(), result.seconds);
}

/* What every agent's requests cost, from the usage each session recorded. */
void print_cost(const std::vector<embed_host::Agent> &agents)
{
    double spend = 0;
    bool estimated = false;
    bool unpriced = false;
    for (const embed_host::Agent &agent : agents) {
        struct agent_stats stats;
        agent_stats_collect(&agent.session, 0, 0, agent.provider, &stats);
        spend += stats.total.spend;
        estimated |= stats.total.spend_estimated != 0;
        unpriced |= stats.total.unpriced != 0;
    }
    const std::lock_guard<std::mutex> guard(output_lock);
    if (unpriced && spend <= 0)
        std::fprintf(stderr, "cost: unknown; the provider reported none and has no rates\n");
    else
        std::fprintf(stderr, "cost: %s$%.4f%s\n", estimated ? "~" : "", spend,
                     unpriced ? ", leaving out requests with no known price" : "");
}

int run(const Options &options, const Document &document)
{
    struct hax_embed_options init = {};
    init.own_curl_global = 1;
    init.diag = diag_sink;
    if (hax_init(&init) != 0)
        return 1;
    config_set_override("no_session", "1");
    if (!options.provider.empty())
        config_set_override("provider", options.provider.c_str());
    if (!options.model.empty())
        config_set_override("model", options.model.c_str());

    /* Every agent is built here, on the main thread: configuration is foreground state. The
     * last one writes the final answer. */
    const size_t n = document.parts.size();
    std::vector<embed_host::Agent> agents(n + 1);
    bool opened = true;
    for (embed_host::Agent &agent : agents) {
        opened =
            embed_host::open(agent, options.provider.empty() ? nullptr : options.provider.c_str());
        if (!opened)
            break;
        apply_tool_policy(agent, options.tools);
    }

    std::vector<Result> parts(n);
    Result synthesis;
    int status = 1;
    if (opened) {
        signal_agents = &agents;
        struct sigaction action = {};
        action.sa_handler = on_interrupt;
        action.sa_flags = SA_RESETHAND;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr);

        /* Each worker takes the first waiting part whose dependencies have all finished. All
         * scheduling state is under `lock`; a send runs outside it. */
        enum class State : std::uint8_t { Waiting, Running, Finished };
        std::vector<State> state(n, State::Waiting);
        std::mutex lock;
        std::condition_variable changed;
        size_t finished = 0;
        const auto finish = [&](size_t i) {
            state[i] = State::Finished;
            const std::string label =
                "[" + std::to_string(++finished) + "/" + std::to_string(n) + "]";
            progress(label.c_str(), document.parts[i].title, parts[i]);
            changed.notify_all();
        };
        const auto worker = [&] {
            std::unique_lock<std::mutex> guard(lock);
            while (!interrupted) {
                size_t pick = n;
                bool blocked = false; /* some part still waits on a running one */
                for (size_t i = 0; i < n && pick == n; i++) {
                    if (state[i] != State::Waiting)
                        continue;
                    const std::vector<size_t> &depends = document.parts[i].depends;
                    const auto pending = [&](size_t d) { return state[d] != State::Finished; };
                    if (std::any_of(depends.begin(), depends.end(), pending)) {
                        blocked = true;
                        continue;
                    }
                    const auto failed = [&](size_t d) { return !succeeded(parts[d]); };
                    const auto missing = std::find_if(depends.begin(), depends.end(), failed);
                    if (missing == depends.end()) {
                        pick = i;
                        continue;
                    }
                    parts[i].skipped =
                        "skipped because '" + document.parts[*missing].title + "' has no report";
                    finish(i);
                }
                if (pick == n) {
                    if (!blocked)
                        break;
                    /* Timed, so a Ctrl-C reaches a worker that nothing else wakes. */
                    changed.wait_for(guard, std::chrono::milliseconds(100));
                    continue;
                }
                state[pick] = State::Running;
                const std::string prompt = part_prompt(document, pick, parts);
                guard.unlock();
                Result result = run_agent(agents[pick], prompt, options);
                guard.lock();
                parts[pick] = std::move(result);
                finish(pick);
            }
        };
        const size_t jobs = std::min(n, static_cast<size_t>(options.jobs));
        std::vector<std::thread> threads;
        threads.reserve(jobs);
        for (size_t j = 0; j < jobs; j++)
            threads.emplace_back(worker);
        for (std::thread &thread : threads)
            thread.join();

        const bool any_part = std::any_of(parts.begin(), parts.end(), succeeded);
        if (!interrupted && any_part) {
            synthesis = run_agent(agents[n], synthesis_prompt(document, parts), options);
            progress("[final]", document.objective.title, synthesis);
        } else if (!any_part) {
            const std::lock_guard<std::mutex> guard(output_lock);
            std::fprintf(stderr, "every part failed; skipping the final answer\n");
        }
        std::signal(SIGINT, SIG_DFL);
        signal_agents = nullptr;

        print_report(document, parts, synthesis);
        print_cost(agents);
        const bool all = std::all_of(parts.begin(), parts.end(), succeeded) && succeeded(synthesis);
        status = interrupted ? 130 : all ? 0 : 1;
    }

    for (embed_host::Agent &agent : agents)
        embed_host::close(agent);
    hax_shutdown();
    return status;
}

void dry_run(const Document &document)
{
    const size_t n = document.parts.size();
    std::vector<Result> placeholders(n);
    for (size_t i = 0; i < n; i++) {
        placeholders[i].ran = true;
        placeholders[i].reply.outcome = AGENT_LOOP_COMPLETE;
        placeholders[i].reply.text = "[the report from part " + std::to_string(i + 1) + "]";
    }
    for (size_t i = 0; i < n; i++) {
        std::string after;
        for (const size_t d : document.parts[i].depends)
            after += (after.empty() ? ", after part " : ", ") + std::to_string(d + 1);
        std::printf("=== part %zu of %zu%s ===\n%s\n", i + 1, n, after.c_str(),
                    part_prompt(document, i, placeholders).c_str());
    }
    std::printf("=== final ===\n%s", synthesis_prompt(document, placeholders).c_str());
}

/* --- Command line --- */

const char USAGE[] = "usage: decompose [--provider ID] [--model ID] [--jobs N] [--max-turns N]\n"
                     "                 [--tools none|read|all] [--dry-run] FILE\n";

bool parse_options(int argc, char *const *argv, Options &options)
{
    for (int i = 1; i < argc; i++) {
        const std::string_view flag = argv[i];
        if (flag == "--dry-run") {
            options.dry_run = true;
            continue;
        }
        if (flag.substr(0, 2) != "--") {
            if (!options.path.empty())
                return false;
            options.path = flag;
            continue;
        }
        if (i + 1 == argc)
            return false;
        const std::string value = argv[++i];
        if (flag == "--provider") {
            options.provider = value;
        } else if (flag == "--model") {
            options.model = value;
        } else if (flag == "--jobs" || flag == "--max-turns") {
            const int number = std::atoi(value.c_str());
            if (number <= 0)
                return false;
            (flag == "--jobs" ? options.jobs : options.max_turns) = number;
        } else if (flag == "--tools") {
            if (value == "none")
                options.tools = Tools::None;
            else if (value == "read")
                options.tools = Tools::Read;
            else if (value == "all")
                options.tools = Tools::All;
            else
                return false;
        } else {
            return false;
        }
    }
    return !options.path.empty();
}

int decompose(int argc, char *const *argv)
{
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fputs(USAGE, stderr);
        return 2;
    }
    std::ifstream input(options.path);
    if (!input) {
        std::fprintf(stderr, "decompose: cannot read %s\n", options.path.c_str());
        return 2;
    }
    Document document;
    std::string error;
    if (!parse(input, document, error)) {
        std::fprintf(stderr, "decompose: %s: %s\n", options.path.c_str(), error.c_str());
        return 2;
    }
    if (options.dry_run) {
        dry_run(document);
        return 0;
    }
    return run(options, document);
}

} // namespace

int main(int argc, char *const *argv)
{
    try {
        return decompose(argc, argv);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "decompose: %s\n", error.what());
        return 1;
    }
}
