#!/usr/bin/env python3
"""ACP mode (--acp): the JSON-RPC exchange on stdin and stdout, permission requests, cancellation,
and session/load, driven by a minimal client against the mock provider."""

import json
import os
import queue
import subprocess
import threading
import time

import harness

# The mock provider defaults to an unrecorded session; session/load needs the file.
RECORD = {"HAX_NO_SESSION": "0"}
TIMEOUT_S = 20


class Client:
    """One `hax --acp` process. Every stdout line must parse as JSON-RPC 2.0; `answer` replies to
    each session/request_permission with the optionId it returns, or cancels for None.
    `host_tool` answers each _hax/tool_call with the result it returns; None leaves it unanswered.
    `reply_id` maps the request's id to the one the answer carries."""

    def __init__(
        self,
        mock_script,
        home=None,
        workdir=None,
        extra_env=None,
        answer=None,
        cancel_on_ask=None,
        nonblocking_stdin=False,
        host_tool=None,
        reply_id=None,
    ):
        if home is None:
            home, workdir = harness.make_home()
        self.home, self.workdir = home, workdir
        env = harness.mock_env(home, mock_script, {**RECORD, **(extra_env or {})})
        env["XDG_STATE_HOME"] = str(home / "state")
        stdin = subprocess.PIPE
        if nonblocking_stdin:
            # O_NONBLOCK lives on the open file, so hax's reads see it too.
            stdin, writer = os.pipe()
            os.set_blocking(stdin, False)
        self.proc = subprocess.Popen(
            [str(harness.hax_binary()), "--acp"],
            cwd=workdir,
            env=env,
            stdin=stdin,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding="utf-8",
        )
        if nonblocking_stdin:
            os.close(stdin)
            self.proc.stdin = os.fdopen(writer, "w", encoding="utf-8")
        self.answer = answer
        # A session id: cancel that session and answer each permission request in one write.
        self.cancel_on_ask = cancel_on_ask
        self.lines = queue.Queue()
        self.updates = []
        self.permission_requests = []
        self.host_tool = host_tool
        self.reply_id = reply_id or (lambda request_id: request_id)
        self.host_calls = []
        self.notifications = []  # anything else hax sends with a method and no id
        self.replies = []  # responses to ids `wait` was not waiting for
        self.bad_lines = []
        self.next_id = 0
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for line in self.proc.stdout:
            self.lines.put(line)
        self.lines.put(None)

    def send(self, *messages):
        """Write `messages` in one write, one line each."""
        for message in messages:
            message["jsonrpc"] = "2.0"
        self.proc.stdin.write("".join(json.dumps(m) + "\n" for m in messages))
        self.proc.stdin.flush()

    def send_raw(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def notify(self, method, params):
        self.send({"method": method, "params": params})

    def start(self, method, params):
        self.next_id += 1
        self.send({"id": self.next_id, "method": method, "params": params})
        return self.next_id

    def wait(self, request_id=None, until=None):
        """Handle messages until the response to `request_id`, or until `until(update)` holds."""
        deadline = time.monotonic() + TIMEOUT_S
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=deadline - time.monotonic())
            except queue.Empty:
                break
            if line is None:
                break
            try:
                message = json.loads(line)
            except ValueError:
                self.bad_lines.append(line)
                continue
            if message.get("jsonrpc") != "2.0":
                self.bad_lines.append(line)
            method = message.get("method")
            if method == "session/update":
                update = message["params"]["update"]
                self.updates.append(update)
                if until and until(update):
                    return update
            elif method == "session/request_permission":
                self.permission_requests.append(message["params"])
                choice = self.answer(message["params"]) if self.answer else None
                outcome = (
                    {"outcome": "selected", "optionId": choice}
                    if choice
                    else {"outcome": "cancelled"}
                )
                response = {"id": message["id"], "result": {"outcome": outcome}}
                if self.cancel_on_ask:
                    cancel = {"method": "session/cancel", "params": {"sessionId": self.cancel_on_ask}}
                    self.send(cancel, response)
                else:
                    self.send(response)
            elif method == "_hax/tool_call":
                self.host_calls.append(message)
                result = self.host_tool(message["params"]) if self.host_tool else None
                if result is not None:
                    self.send({"id": self.reply_id(message["id"]), **result})
            elif method and "id" not in message:
                self.notifications.append(message)
            elif request_id is not None and message.get("id") == request_id:
                return message
            elif not method:
                self.replies.append(message)
        raise harness.ScenarioFailure(
            f"no answer to request {request_id} within {TIMEOUT_S} s", self.finish().stderr
        )

    def call(self, method, params):
        return self.wait(self.start(method, params))

    def setup(self):
        self.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
        return self.call("session/new", {"cwd": str(self.workdir), "mcpServers": []})["result"][
            "sessionId"
        ]

    def prompt(self, session, text):
        return self.call(
            "session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": text}]}
        )

    def finish(self):
        self.proc.stdin.close()
        stderr = self.proc.stderr.read()
        self.proc.wait(timeout=TIMEOUT_S)
        return harness.Result(
            subprocess.CompletedProcess(self.proc.args, self.proc.returncode, "", stderr),
            self.workdir,
        )

    def kinds(self):
        return [update["sessionUpdate"] for update in self.updates]

    def session_record(self, session):
        """The text of the session file named for `session`."""
        files = [p for p in (self.home / "state").rglob("*") if p.is_file() and session in p.name]
        harness.expect(len(files) == 1, f"one session file for {session}: {files}")
        return files[0].read_text()


def test_flag_conflicts_are_usage_errors():
    """--acp refuses the one-shot flags, resuming flags, and prompt words before starting."""
    for extra in (["-p", "hi"], ["--json"], ["hello"], ["--continue"], ["--resume=x"]):
        home, workdir = harness.make_home()
        proc = subprocess.run(
            [str(harness.hax_binary()), "--acp", *extra],
            cwd=workdir,
            env=harness.mock_env(home, "end-turn\n"),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            encoding="utf-8",
            timeout=TIMEOUT_S,
        )
        result = harness.Result(proc, workdir)
        harness.expect(proc.returncode == 1, f"--acp {' '.join(extra)} exits 1", result)
        harness.expect(proc.stdout == "", "a refused run writes nothing to stdout", result)
        harness.expect("--acp cannot be combined" in proc.stderr, "stderr says why", result)


def test_requests_before_initialize_and_unknown_methods():
    """Before initialize a request is refused with -32600; after it, an unknown method is
    -32601, and a line that is not JSON is -32700."""
    client = Client("end-turn\n")
    early = client.call("session/new", {"cwd": str(client.workdir), "mcpServers": []})
    harness.expect(early["error"]["code"] == -32600, "session/new before initialize is -32600")
    init = client.call("initialize", {"protocolVersion": 1})["result"]
    harness.expect(init["protocolVersion"] == 1, "initialize answers protocol version 1")
    harness.expect(init["agentCapabilities"]["loadSession"], "a recording run advertises load")
    harness.expect(init["agentInfo"]["name"] == "hax", "agentInfo names hax")
    unknown = client.call("no/such_method", {})
    harness.expect(unknown["error"]["code"] == -32601, "an unknown method is -32601")
    client.proc.stdin.write("not json\n")
    client.proc.stdin.flush()
    line = json.loads(client.lines.get(timeout=TIMEOUT_S))
    harness.expect(line["error"]["code"] == -32700 and line["id"] is None, "bad JSON is -32700")
    result = client.finish()
    harness.expect(result.returncode == 0, "hax exits 0 when stdin closes", result)


def test_prompt_streams_text_then_ends_the_turn():
    """Reply text arrives as agent_message_chunk updates before the end_turn response, and stdout
    carries nothing but protocol messages."""
    client = Client(
        """
        text Hello from mock
        end-turn
        """
    )
    session = client.setup()
    response = client.prompt(session, "hi")
    harness.expect(response["result"]["stopReason"] == "end_turn", "the turn ends with end_turn")
    text = "".join(
        u["content"]["text"] for u in client.updates if u["sessionUpdate"] == "agent_message_chunk"
    )
    harness.expect(text == "Hello from mock", f"the chunks carry the reply, got {text!r}")
    result = client.finish()
    harness.expect(not client.bad_lines, f"stdout had non-protocol lines: {client.bad_lines}")
    harness.expect(result.returncode == 0, "hax exits 0", result)


BASH_TWICE = """
    tool bash {"command":"echo one >> out.txt"}
    end-turn

    tool bash {"command":"echo two >> out.txt"}
    end-turn

    text Done.
    end-turn
    """


def test_allow_always_runs_the_tool_and_is_remembered():
    """An allow_always answer runs the call and covers the next call to the same tool."""
    client = Client(BASH_TWICE, answer=lambda params: "allow_always")
    session = client.setup()
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "end_turn", "the turn ends with end_turn")
    harness.expect(len(client.permission_requests) == 1, "the client is asked once")
    asked = client.permission_requests[0]
    harness.expect(asked["toolCall"]["kind"] == "execute", "bash is an execute tool call")
    harness.expect(
        {o["kind"] for o in asked["options"]}
        == {"allow_once", "allow_always", "reject_once", "reject_always"},
        "the request offers all four option kinds",
    )
    harness.expect(
        (client.workdir / "out.txt").read_text() == "one\ntwo\n", "both commands ran in the cwd"
    )
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses.count("completed") == 2, f"both calls completed: {statuses}")
    client.finish()


def test_a_denied_or_cancelled_permission_does_not_run_the_tool():
    """reject_once and a cancelled request both deny: the command never runs and its call fails,
    while the turn goes on to end normally."""
    for answer in (lambda params: "reject_once", None):
        client = Client(BASH_TWICE, answer=answer)
        session = client.setup()
        response = client.prompt(session, "go")
        harness.expect(response["result"]["stopReason"] == "end_turn", "the turn still ends")
        harness.expect(len(client.permission_requests) == 2, "each call is asked about")
        harness.expect(not (client.workdir / "out.txt").exists(), "no command ran")
        statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
        harness.expect(statuses == ["failed", "failed"], f"both calls failed: {statuses}")
        client.finish()


def test_cancel_stops_a_running_tool():
    """session/cancel during a long command kills it and ends the prompt with cancelled."""
    client = Client(
        """
        tool bash {"command":"sleep 30; echo late > out.txt"}
        end-turn
        """,
        answer=lambda params: "allow_once",
    )
    session = client.setup()
    request = client.start(
        "session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "go"}]}
    )
    client.wait(until=lambda u: u.get("status") == "in_progress")
    started = time.monotonic()
    client.notify("session/cancel", {"sessionId": session})
    response = client.wait(request)
    harness.expect(response["result"]["stopReason"] == "cancelled", "the prompt ends cancelled")
    harness.expect(time.monotonic() - started < 10, "the command was stopped, not waited for")
    harness.expect(not (client.workdir / "out.txt").exists(), "the command did not finish")
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses[-1] == "failed", f"the killed call failed: {statuses}")
    client.finish()


def test_an_answer_racing_a_cancel_does_not_run_the_tool():
    """A permission granted right after session/cancel, in the same write, leaves the call unrun:
    write checks no cancel itself, so the file would appear if it ran."""
    client = Client(
        """
        tool write {"path":"out.txt","content":"late"}
        end-turn

        text Done.
        end-turn
        """,
        answer=lambda params: "allow_once",
    )
    session = client.setup()
    client.cancel_on_ask = session
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "cancelled", "the prompt ends cancelled")
    harness.expect(not (client.workdir / "out.txt").exists(), "the write did not run")
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses == ["failed"], f"the call failed unrun: {statuses}")
    client.finish()


def test_queued_prompts_end_cancelled_when_stdin_closes():
    """Prompts still queued when the client goes away end cancelled without a provider call."""
    client = Client(
        """
        delay 500
        text First.
        end-turn

        text Second.
        end-turn
        """
    )
    session = client.setup()
    prompt = {"sessionId": session, "prompt": [{"type": "text", "text": "hi"}]}
    first = {"id": 101, "method": "session/prompt", "params": prompt}
    second = {"id": 102, "method": "session/prompt", "params": prompt}
    client.send(first, second)
    client.proc.stdin.close()
    stops = [client.wait(request)["result"]["stopReason"] for request in (101, 102)]
    harness.expect(stops == ["cancelled", "cancelled"], f"both prompts end cancelled: {stops}")
    text = [u for u in client.updates if u["sessionUpdate"] == "agent_message_chunk"]
    harness.expect(not text, f"no reply streamed: {text}")
    client.proc.wait(timeout=TIMEOUT_S)


def test_malformed_requests_and_a_cancel_sent_as_a_request():
    """A non-string method is -32600 with the request's id. session/cancel with an id cancels
    the prompt and is answered."""
    client = Client(
        """
        delay 3000
        text Slow.
        end-turn
        """
    )
    session = client.setup()
    client.send({"id": 7, "method": 5})
    bad = client.wait(7)
    harness.expect(bad.get("error", {}).get("code") == -32600, f"method 5 is -32600: {bad}")
    request = client.start(
        "session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "go"}]}
    )
    time.sleep(0.5)
    cancel = client.start("session/cancel", {"sessionId": session})
    answered = client.wait(cancel)
    harness.expect(answered.get("result") == {}, f"the cancel request is answered: {answered}")
    response = client.wait(request)
    harness.expect(response["result"]["stopReason"] == "cancelled", "the prompt ends cancelled")
    client.finish()


def test_an_overlong_line_is_dropped_with_a_parse_error():
    """A line over 16 MiB gets -32700 and is discarded; the next line is served."""
    client = Client("end-turn\n")
    client.send_raw("x" * (16 * 1024 * 1024 + 1))
    line = json.loads(client.lines.get(timeout=TIMEOUT_S))
    harness.expect(line["error"]["code"] == -32700 and line["id"] is None, f"-32700: {line}")
    init = client.call("initialize", {"protocolVersion": 1})
    harness.expect(init["result"]["protocolVersion"] == 1, "the next request is served")
    result = client.finish()
    harness.expect(result.returncode == 0, "hax exits 0", result)


def test_task_notes_reach_only_the_session_that_started_the_task():
    """A background task's completion note lands in its own session, not in the one that
    prompts next."""
    client = Client(
        """
        tool bash {"command":"sleep 1; echo bg-done","background":true}
        end-turn

        text Started.
        end-turn

        text B reply.
        end-turn

        text A reply.
        end-turn
        """,
        extra_env={"HAX_BASH_BACKGROUND_YIELD": "100ms"},
        answer=lambda params: "allow_once",
    )
    first = client.setup()
    second = client.call("session/new", {"cwd": str(client.workdir), "mcpServers": []})["result"][
        "sessionId"
    ]
    client.prompt(first, "start a task")
    time.sleep(2)
    client.prompt(second, "unrelated")
    client.prompt(first, "again")
    client.finish()
    harness.expect("finished (exit 0)" not in client.session_record(second), "B got no note")
    harness.expect("finished (exit 0)" in client.session_record(first), "A got its note")


def test_a_nonblocking_stdin_is_not_end_of_input():
    """A read that would block waits for input instead of ending the session."""
    client = Client("text Hi\nend-turn\n", nonblocking_stdin=True)
    time.sleep(0.3)
    session = client.setup()
    response = client.prompt(session, "hi")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the prompt ran: {response}")
    client.finish()


def test_a_prompt_cancelled_before_it_starts_records_no_turn():
    """A prompt cancelled while queued adds no user_turn record to its session."""
    client = Client(
        """
        text Warm.
        end-turn

        text Slow reply.
        delay 300
        space
        end-turn
        """
    )
    queued = client.setup()
    client.prompt(queued, "warm up")
    busy = client.call("session/new", {"cwd": str(client.workdir), "mcpServers": []})["result"][
        "sessionId"
    ]
    running = client.start(
        "session/prompt", {"sessionId": busy, "prompt": [{"type": "text", "text": "slow"}]}
    )
    client.wait(until=lambda u: u["sessionUpdate"] == "agent_message_chunk")
    prompt = {"sessionId": queued, "prompt": [{"type": "text", "text": "never"}]}
    client.send(
        {"id": 900, "method": "session/prompt", "params": prompt},
        {"method": "session/cancel", "params": {"sessionId": queued}},
    )
    # Prompts run in order, so the queued one is dispatched, already cancelled, after the other.
    harness.expect(client.wait(running)["result"]["stopReason"] == "end_turn", "the other ends")
    harness.expect(client.wait(900)["result"]["stopReason"] == "cancelled", "it ends cancelled")
    client.finish()
    turns = client.session_record(queued).count('"type":"user_turn"')
    harness.expect(turns == 1, f"only the warm-up prompt recorded a turn: {turns}")


def test_a_reply_cut_by_overflow_stays_apart_from_its_retry():
    """After an overflow compacts the session, the retried reply follows the cut one after a
    paragraph break, and session/load replays the text the client saw."""
    script = """
        text The answer so
        overflow
        end-turn

        text ## Goal\\n- answer
        end-turn

        text The full answer.
        end-turn
        """
    first = Client(script)
    session = first.setup()
    first.prompt(session, "explain")
    first.finish()
    shown = "".join(
        u["content"]["text"] for u in first.updates if u["sessionUpdate"] == "agent_message_chunk"
    )
    harness.expect(shown == "The answer so\n\nThe full answer.", f"the client saw {shown!r}")

    second = Client(script, home=first.home, workdir=first.workdir)
    second.call("initialize", {"protocolVersion": 1})
    second.call("session/load", {"sessionId": session, "cwd": str(first.workdir), "mcpServers": []})
    replayed = "".join(
        u["content"]["text"] for u in second.updates if u["sessionUpdate"] == "agent_message_chunk"
    )
    harness.expect(replayed == shown, f"the replay matches: {replayed!r}")
    second.finish()


def test_provider_error_is_a_json_rpc_error():
    """A provider failure answers the prompt with a JSON-RPC error, not a stop reason."""
    client = Client("end-turn\n", extra_env={"HAX_MOCK_SCRIPT": "/nonexistent/script.txt"})
    session = client.setup()
    response = client.prompt(session, "hi")
    harness.expect("error" in response, f"the prompt answered an error: {response}")
    harness.expect(response["error"]["code"] == -32603, "the error is -32603")
    client.finish()


LOOKUP_ORDER = {
    "name": "lookup_order",
    "description": "Return the contents of an order.",
    "inputSchema": {
        "type": "object",
        "properties": {"order_id": {"type": "string"}},
        "required": ["order_id"],
    },
}
HOST_BASH = {"name": "bash", "inputSchema": {"type": "object"}}


def host_session(client, tools):
    client.call("initialize", {"protocolVersion": 1})
    return client.call(
        "session/new",
        {"cwd": str(client.workdir), "mcpServers": [], "_meta": {"hax": {"tools": tools}}},
    )


def test_host_tools_are_advertised_and_answered_by_the_client():
    """Tools from session/new reach the model, and their calls go to the client without a
    permission request; a host tool named bash replaces the built-in."""

    def answer(params):
        if params["name"] == "lookup_order":
            return {"result": {"content": [{"type": "text", "text": "2x widget"}]}}
        return {"result": {"content": [{"type": "text", "text": "sandboxed"}], "isError": True}}

    home, workdir = harness.make_home()
    transcript = home / "transcript.txt"
    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        tool bash {"command":"echo ran > out.txt"}
        end-turn

        text Done.
        end-turn
        """,
        home=home,
        workdir=workdir,
        extra_env={"HAX_TRANSCRIPT": str(transcript)},
        host_tool=answer,
    )
    init = client.call("initialize", {"protocolVersion": 1})["result"]
    harness.expect(
        init["agentCapabilities"]["_meta"]["hax"]["hostTools"], "initialize offers host tools"
    )
    bad = client.call(
        "session/new",
        {"cwd": str(workdir), "mcpServers": [], "_meta": {"hax": {"tools": [{"name": "x"}]}}},
    )
    harness.expect(bad.get("error", {}).get("code") == -32602, f"a bad tool is -32602: {bad}")
    session = client.call(
        "session/new",
        {
            "cwd": str(workdir),
            "mcpServers": [],
            "_meta": {"hax": {"tools": [LOOKUP_ORDER, HOST_BASH]}},
        },
    )["result"]["sessionId"]
    response = client.prompt(session, "what is in order 4417?")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the turn ends: {response}")
    calls = [(c["params"]["name"], c["params"]["arguments"]) for c in client.host_calls]
    harness.expect(
        calls == [("lookup_order", {"order_id": "4417"}), ("bash", {"command": "echo ran > out.txt"})],
        f"both calls went to the client: {calls}",
    )
    harness.expect(not client.permission_requests, "host tools ask no permission")
    harness.expect(not (workdir / "out.txt").exists(), "the built-in bash did not run")
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(
        statuses == ["in_progress", "completed", "in_progress", "failed"], f"statuses: {statuses}"
    )
    client.finish()
    text = transcript.read_text()
    harness.expect("[lookup_order]" in text and '"order_id"' in text, "the model was offered it")
    harness.expect("2x widget" in text, "the model read the client's result")
    harness.expect("error: sandboxed" in text, "the model read the client's error")


def expect_abandoned(client, session, reason):
    """hax told the client, once, that it gave up on the one host call it sent."""
    abandoned = [n for n in client.notifications if n["method"] == "_hax/tool_call_abandoned"]
    expected = {
        "sessionId": session,
        "toolCallId": client.host_calls[0]["params"]["toolCallId"],
        "reason": reason,
    }
    harness.expect(
        [n["params"] for n in abandoned] == [expected], f"one abandon notice: {abandoned}"
    )


def test_a_cancel_during_a_host_tool_call_leaves_it_unrun():
    """session/cancel while the client holds a call ends the prompt cancelled; the late answer is
    dropped and the session takes its next prompt."""
    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        text After.
        end-turn
        """
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    request = client.start(
        "session/prompt", {"sessionId": session, "prompt": [{"type": "text", "text": "go"}]}
    )
    client.wait(until=lambda u: u.get("status") == "in_progress")
    client.notify("session/cancel", {"sessionId": session})
    response = client.wait(request)
    harness.expect(response["result"]["stopReason"] == "cancelled", "the prompt ends cancelled")
    harness.expect(len(client.host_calls) == 1, f"the client got the call: {client.host_calls}")
    late = {"result": {"content": [{"type": "text", "text": "too late"}]}}
    client.send({"id": client.host_calls[0]["id"], **late})
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses[-1] == "failed", f"the call failed unrun: {statuses}")
    expect_abandoned(client, session, "cancelled")
    after = client.prompt(session, "again")
    harness.expect(after["result"]["stopReason"] == "end_turn", f"the session goes on: {after}")
    result = client.finish()
    harness.expect("too late" not in client.session_record(session), "the late answer was dropped")
    harness.expect("dropped a response" not in result.stderr, "a late answer is not warned about")


def test_a_client_that_never_answers_fails_the_call_after_the_timeout():
    """acp.tool_timeout fails a host call the client never answers; the model reads why and the
    prompt goes on. The answer that arrives afterwards is dropped quietly."""
    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        text After.
        end-turn
        """,
        extra_env={"HAX_ACP_TOOL_TIMEOUT": "1s"},
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    started = time.monotonic()
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the prompt ends: {response}")
    harness.expect(time.monotonic() - started < 10, "the wait ended at the timeout")
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses == ["in_progress", "failed"], f"the call failed: {statuses}")
    expect_abandoned(client, session, "timeout")
    late = {"result": {"content": [{"type": "text", "text": "too late"}]}}
    client.send({"id": client.host_calls[0]["id"], **late})
    result = client.finish()
    record = client.session_record(session)
    harness.expect("error: the client did not answer within 1s" in record, "the model read why")
    harness.expect("did not answer lookup_order within 1s" in result.stderr, "stderr says so")
    harness.expect("too late" not in record, "the late answer was dropped", result)
    harness.expect("dropped a response" not in result.stderr, "quietly", result)


def test_an_unreadable_answer_fails_the_call_at_once():
    """A line that is not JSON, or is over 16 MiB, arriving while hax waits on a host call fails
    that call straight away instead of waiting out acp.tool_timeout."""

    def answer(params):
        if len(client.host_calls) == 1:
            client.send_raw('{"id": 1, "result": {"content": [{"text": "\xff')
        else:
            client.send_raw("x" * (16 * 1024 * 1024 + 1))
        return None

    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        tool lookup_order {"order_id":"4418"}
        end-turn

        text Done.
        end-turn
        """,
        host_tool=answer,
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    started = time.monotonic()
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the prompt ends: {response}")
    harness.expect(time.monotonic() - started < 10, "neither call waited for the timeout")
    statuses = [u["status"] for u in client.updates if u["sessionUpdate"] == "tool_call_update"]
    harness.expect(statuses.count("failed") == 2, f"both calls failed: {statuses}")
    result = client.finish()
    record = client.session_record(session)
    harness.expect("error: the client's answer could not be parsed as JSON" in record, "bad JSON")
    harness.expect("error: the client's answer exceeds 16 MiB" in record, "the overlong line")
    harness.expect(result.stderr.count("failing request") == 2, "both warned", result)


def test_an_answer_without_result_or_error_fails_the_call_at_once():
    """An answer to the waiting call that carries neither `result` nor `error` fails the call
    and gets no reply. A malformed answer to an id hax never sent still gets -32600."""

    def answer(params):
        client.send({"id": client.host_calls[-1]["id"], "output": "2x widget"})
        return None

    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        text Done.
        end-turn
        """,
        host_tool=answer,
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    started = time.monotonic()
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the prompt ends: {response}")
    harness.expect(time.monotonic() - started < 10, "the call did not wait for the timeout")
    called = client.host_calls[0]["id"]
    harness.expect(
        not [r for r in client.replies if r.get("id") == called], f"no reply: {client.replies}"
    )
    client.send({"id": 999, "output": "x"})
    stray = client.wait(999)
    harness.expect(stray.get("error", {}).get("code") == -32600, f"a stray one is -32600: {stray}")
    result = client.finish()
    record = client.session_record(session)
    harness.expect(
        "error: the client's answer has neither a result nor an error" in record, "the model read why"
    )
    harness.expect("failing request" in result.stderr, "stderr says so", result)


def test_answers_with_mismatched_ids_are_taken_with_a_warning():
    """hax has one request out at a time, so an answer carrying the same number as a string, or a
    null id on an error, still answers it. An id hax never sent is dropped. Each is warned."""

    def answer(params):
        if len(client.host_calls) == 1:
            return {"result": {"content": [{"type": "text", "text": "2x widget"}]}}
        return {"error": {"code": -32700, "message": "cannot parse the request"}}

    def reply_id(request_id):
        return str(request_id) if len(client.host_calls) == 1 else None

    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        tool lookup_order {"order_id":"4418"}
        end-turn

        text Done.
        end-turn
        """,
        host_tool=answer,
        reply_id=reply_id,
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    response = client.prompt(session, "go")
    harness.expect(response["result"]["stopReason"] == "end_turn", f"the prompt ends: {response}")
    client.send({"id": 999, "result": {}})
    result = client.finish()
    record = client.session_record(session)
    harness.expect("2x widget" in record, "the string-id answer was taken", result)
    harness.expect("error: cannot parse the request" in record, "the null-id error was taken")
    harness.expect(result.stderr.count("took a response with id") == 2, "both warned", result)
    harness.expect("dropped a response with id 999" in result.stderr, "the stray one", result)


def test_a_host_result_over_the_cap_is_truncated():
    """tool_output_cap holds a client's result as it holds hax's own tools."""
    lines = [f"order line {i:04d}" for i in range(400)]
    client = Client(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        text Done.
        end-turn
        """,
        extra_env={"HAX_TOOL_OUTPUT_CAP": "1k"},
        host_tool=lambda params: {"result": {"content": [{"type": "text", "text": "\n".join(lines)}]}},
    )
    session = host_session(client, [LOOKUP_ORDER])["result"]["sessionId"]
    client.prompt(session, "go")
    client.finish()
    record = client.session_record(session)
    harness.expect("[output truncated: kept " in record, "the result carries the marker")
    harness.expect("order line 0000" in record and "order line 0399" in record, "head and tail")
    harness.expect("order line 0200" not in record, "the middle was dropped")


def test_a_client_that_stops_reading_cannot_deadlock_hax():
    """While a long reply fills hax's stdout and the client reads nothing, hax keeps reading its
    stdin, so the client's own writes never block on it. Reading again lets the reply finish.

    Drives the process directly: Client drains stdout on a thread, so it cannot stop reading."""
    filler = "x" * 4000
    script = "".join(f"text {filler}\n" for _ in range(300)) + "end-turn\n"
    home, workdir = harness.make_home()
    env = harness.mock_env(home, script, RECORD)
    env["XDG_STATE_HOME"] = str(home / "state")
    proc = subprocess.Popen(
        [str(harness.hax_binary()), "--acp"],
        cwd=workdir,
        env=env,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        encoding="utf-8",
    )

    def send(message):
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", **message}) + "\n")
        proc.stdin.flush()

    def read_until(request_id):
        """Messages up to and including the response to `request_id`."""
        seen = []
        for line in proc.stdout:
            seen.append(json.loads(line))
            if seen[-1].get("id") == request_id and "method" not in seen[-1]:
                return seen
        raise harness.ScenarioFailure(f"hax exited before answering {request_id}", "")

    send({"id": 1, "method": "initialize", "params": {"protocolVersion": 1}})
    read_until(1)
    send({"id": 2, "method": "session/new", "params": {"cwd": str(workdir), "mcpServers": []}})
    session = read_until(2)[-1]["result"]["sessionId"]
    prompt = {"sessionId": session, "prompt": [{"type": "text", "text": "go"}]}
    send({"id": 3, "method": "session/prompt", "params": prompt})
    time.sleep(0.5)  # let the reply fill the pipe

    # Each malformed line makes hax's reader reply; a reader that waits on output stops reading.
    flood = threading.Thread(
        target=lambda: [send_raw_line(proc, "{" + filler) for _ in range(512)], daemon=True
    )
    flood.start()
    flood.join(10)
    stuck = flood.is_alive()
    if stuck:
        proc.kill()
    harness.expect(not stuck, "hax kept reading stdin while its stdout was full")

    seen = read_until(3)
    harness.expect(seen[-1]["result"]["stopReason"] == "end_turn", "the reply then finished")
    errors = [m for m in seen if m.get("error", {}).get("code") == -32700]
    harness.expect(len(errors) == 512, f"each malformed line was answered: {len(errors)}")
    text = "".join(
        m["params"]["update"]["content"]["text"]
        for m in seen
        if m.get("method") == "session/update"
        and m["params"]["update"]["sessionUpdate"] == "agent_message_chunk"
    )
    harness.expect(text == filler * 300, f"no reply text was lost: {len(text)} chars")
    proc.stdin.close()
    proc.wait(timeout=TIMEOUT_S)


def send_raw_line(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def test_session_load_replays_history_in_a_new_process():
    """A second process loads the first one's session: the prompt and reply replay as updates
    before the response, and the conversation continues."""
    script = """
        text First reply
        end-turn

        text Second reply
        end-turn
        """
    first = Client(script)
    session = first.setup()
    first.prompt(session, "first prompt")
    first.finish()

    second = Client(script, home=first.home, workdir=first.workdir)
    second.call("initialize", {"protocolVersion": 1})
    loaded = second.call(
        "session/load", {"sessionId": session, "cwd": str(first.workdir), "mcpServers": []}
    )
    harness.expect(loaded.get("result") == {}, f"session/load answers {{}}: {loaded}")
    replay = [(u["sessionUpdate"], u["content"]["text"]) for u in second.updates]
    harness.expect(
        replay
        == [("user_message_chunk", "first prompt"), ("agent_message_chunk", "First reply")],
        f"the history replays in order: {replay}",
    )
    missing = second.call(
        "session/load", {"sessionId": "no-such-id", "cwd": str(first.workdir), "mcpServers": []}
    )
    harness.expect(missing["error"]["code"] == -32002, "an unknown session is -32002")
    # An id prefix is not the session's id: loading by it would open a second copy.
    prefix = second.call(
        "session/load", {"sessionId": session[:8], "cwd": str(first.workdir), "mcpServers": []}
    )
    harness.expect(prefix.get("error", {}).get("code") == -32002, f"a prefix is -32002: {prefix}")
    response = second.prompt(session, "second prompt")
    harness.expect(response["result"]["stopReason"] == "end_turn", "the loaded session continues")
    second.finish()


harness.main(globals())
