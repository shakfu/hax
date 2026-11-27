/* SPDX-License-Identifier: MIT */
/* Minimal ACP host: runs `hax --acp`, gives the model one tool backed by host state, and answers
 * its calls. "ACP mode" in docs/usage.md documents the protocol.
 *
 *   c++ -std=c++17 examples/acp_host.cpp $(pkg-config --cflags --libs jansson) -o acp_host
 *   ./acp_host "what is in order 4417?"
 *
 * HAX_BIN selects the hax binary; the default is `hax` on PATH. Provider settings are hax's own.
 * Built-in tools that ask permission are refused, so the model can only use the host's tool. */

#include <cstdio>
#include <cstdlib>
#include <jansson.h>
#include <memory>
#include <signal.h>
#include <string_view>
#include <unistd.h>
#include <sys/wait.h>

namespace
{

/* The host state the tool reads. A real host would hold a database handle or a live client. */
struct Order {
    std::string_view id;
    const char *contents;
};
constexpr Order ORDERS[] = {
    {"4417", "2x widget, 1x sprocket"},
    {"4418", "1x gear"},
};

struct JsonDecref {
    void operator()(json_t *json) const
    {
        json_decref(json);
    }
};
using Json = std::unique_ptr<json_t, JsonDecref>;

/* "" when the member is missing or not a string: hax's messages are input, not trusted. */
const char *string_at(const json_t *object, const char *key)
{
    const char *value = json_string_value(json_object_get(object, key));
    return value ? value : "";
}

/* One `hax --acp` child, spoken to in newline-delimited JSON-RPC 2.0. */
class Agent
{
  public:
    explicit Agent(const char *hax)
    {
        int to_child[2];
        int from_child[2];
        if (pipe(to_child) != 0 || pipe(from_child) != 0) {
            perror("pipe");
            std::exit(1);
        }
        pid_ = fork();
        if (pid_ < 0) {
            perror("fork");
            std::exit(1);
        }
        if (pid_ == 0) {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            close(to_child[0]);
            close(to_child[1]);
            close(from_child[0]);
            close(from_child[1]);
            execlp(hax, hax, "--acp", static_cast<char *>(nullptr));
            perror(hax);
            _exit(127);
        }
        close(to_child[0]);
        close(from_child[1]);
        to_ = fdopen(to_child[1], "w");
        from_ = fdopen(from_child[0], "r");
    }

    Agent(const Agent &) = delete;
    Agent &operator=(const Agent &) = delete;

    /* Closing stdin ends hax, which cancels anything still running. */
    ~Agent()
    {
        std::fclose(to_);
        std::fclose(from_);
        int status = 0;
        waitpid(pid_, &status, 0);
        std::free(line_);
    }

    void send(Json message)
    {
        json_object_set_new(message.get(), "jsonrpc", json_string("2.0"));
        char *text = json_dumps(message.get(), JSON_COMPACT);
        std::fprintf(to_, "%s\n", text);
        std::fflush(to_);
        std::free(text);
    }

    /* The next message, or null when hax has exited. */
    Json receive()
    {
        while (getline(&line_, &capacity_, from_) > 0) {
            Json message(json_loads(line_, 0, nullptr));
            if (message)
                return message;
        }
        return nullptr;
    }

    long request(const char *method, json_t *params)
    {
        const long id = ++next_id_;
        send(Json(json_pack("{s:I, s:s, s:o}", "id", static_cast<json_int_t>(id), "method", method,
                            "params", params)));
        return id;
    }

    void reply(const json_t *id, json_t *result)
    {
        send(Json(json_pack("{s:O, s:o}", "id", id, "result", result)));
    }

  private:
    pid_t pid_ = -1;
    FILE *to_ = nullptr;
    FILE *from_ = nullptr;
    long next_id_ = 0;
    char *line_ = nullptr;
    size_t capacity_ = 0;
};

/* The host's answer to one lookup_order call, in MCP's CallToolResult shape. */
json_t *lookup_order(const json_t *arguments)
{
    const char *order_id = string_at(arguments, "order_id");
    for (const Order &order : ORDERS)
        if (order.id == order_id)
            return json_pack("{s:[{s:s, s:s}]}", "content", "type", "text", "text", order.contents);
    return json_pack("{s:[{s:s, s:o}], s:b}", "content", "type", "text", "text",
                     json_sprintf("no order %s", order_id), "isError", 1);
}

/* Handle what hax sends until the response to request `id` arrives. */
Json wait_for(Agent &agent, long id)
{
    while (Json message = agent.receive()) {
        const json_t *params = json_object_get(message.get(), "params");
        const std::string_view method = string_at(message.get(), "method");
        const json_t *message_id = json_object_get(message.get(), "id");

        if (method.empty()) {
            if (json_integer_value(message_id) == id)
                return message;
        } else if (method == "session/update") {
            const json_t *update = json_object_get(params, "update");
            if (std::string_view(string_at(update, "sessionUpdate")) == "agent_message_chunk")
                std::fputs(string_at(json_object_get(update, "content"), "text"), stdout);
        } else if (method == "_hax/tool_call") {
            if (std::string_view(string_at(params, "name")) == "lookup_order")
                agent.reply(message_id, lookup_order(json_object_get(params, "arguments")));
            else
                agent.send(Json(json_pack("{s:O, s:{s:i, s:s}}", "id", message_id, "error", "code",
                                          -32601, "message", "unknown host tool")));
        } else if (method == "session/request_permission") {
            agent.reply(message_id, json_pack("{s:{s:s, s:s}}", "outcome", "outcome", "selected",
                                              "optionId", "reject_once"));
        } else if (message_id) {
            agent.send(Json(json_pack("{s:O, s:{s:i, s:s}}", "id", message_id, "error", "code",
                                      -32601, "message", "method not found")));
        }
    }
    std::fprintf(stderr, "hax exited before answering request %ld\n", id);
    std::exit(1);
}

/* The response's result, or exit after printing its error. */
const json_t *result_of(const Json &response, const char *what)
{
    const json_t *result = json_object_get(response.get(), "result");
    if (!result) {
        const char *error = string_at(json_object_get(response.get(), "error"), "message");
        std::fprintf(stderr, "%s failed: %s\n", what, *error ? error : "(no message)");
        std::exit(1);
    }
    return result;
}

} // namespace

int main(int argc, char *const *argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s PROMPT\n", argv[0]);
        return 2;
    }
    /* A hax that exits early makes writes fail instead of killing the host. */
    signal(SIGPIPE, SIG_IGN);
    const char *hax = std::getenv("HAX_BIN");
    Agent agent(hax && *hax ? hax : "hax");

    const Json init =
        wait_for(agent, agent.request("initialize", json_pack("{s:i}", "protocolVersion", 1)));
    const json_t *capabilities =
        json_object_get(result_of(init, "initialize"), "agentCapabilities");
    const json_t *hax_meta = json_object_get(json_object_get(capabilities, "_meta"), "hax");
    if (!json_is_true(json_object_get(hax_meta, "hostTools"))) {
        std::fprintf(stderr, "this hax does not accept host tools\n");
        return 1;
    }

    /* The session's tools: MCP's Tool shape, with a JSON Schema for the arguments. */
    char *cwd = getcwd(nullptr, 0);
    if (!cwd) {
        perror("getcwd");
        return 1;
    }
    const Json session = wait_for(
        agent,
        agent.request("session/new",
                      json_pack("{s:s, s:[], s:{s:{s:[{s:s, s:s, s:{s:s, s:{s:{s:s, s:s}}, "
                                "s:[s]}}]}}}",
                                "cwd", cwd, "mcpServers", "_meta", "hax", "tools", "name",
                                "lookup_order", "description", "Return the contents of an order.",
                                "inputSchema", "type", "object", "properties", "order_id", "type",
                                "string", "description", "The order number.", "required",
                                "order_id")));
    std::free(cwd);
    const char *session_id = string_at(result_of(session, "session/new"), "sessionId");

    const Json prompt =
        wait_for(agent, agent.request("session/prompt",
                                      json_pack("{s:s, s:[{s:s, s:s}]}", "sessionId", session_id,
                                                "prompt", "type", "text", "text", argv[1])));
    const char *stop = string_at(result_of(prompt, "session/prompt"), "stopReason");
    std::fputc('\n', stdout);
    if (std::string_view(stop) != "end_turn") {
        std::fprintf(stderr, "the prompt stopped: %s\n", stop);
        return 1;
    }
    return 0;
}
