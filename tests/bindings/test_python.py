#!/usr/bin/env python3
"""The cffi binding drives a full user turn, including host-defined tools.

Registered only when -Dembed=true builds libhax and the cffi extension. Otherwise hermetic, like
the e2e scenarios.
"""

import asyncio
import os
import sys
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MOCK_DIR = REPO_ROOT / "scripts" / "mock"

failures = 0


def expect(condition: bool, description: str) -> None:
    global failures
    if not condition:
        print(f"FAIL: {description}", file=sys.stderr)
        failures += 1


def use_mock(script: str) -> None:
    os.environ["HAX_PROVIDER"] = "mock"
    os.environ["HAX_MOCK_SCRIPT"] = str(MOCK_DIR / script)


# A scratch HOME keeps the developer's real config and sessions out of the run.
scratch = Path(os.environ.get("MESON_BUILD_ROOT", "/tmp")) / "hax-binding-scratch"
scratch.mkdir(parents=True, exist_ok=True)
os.environ["HOME"] = str(scratch)
os.environ["HAX_KEEP_AWAKE"] = "0"
for key in [k for k in os.environ if k.startswith("XDG_")]:
    del os.environ[key]

# A sanitized libhax needs its runtime preloaded into this interpreter, which meson arranges
# where it can. Where it cannot, say why rather than aborting on library order.
_sanitizer_skip = os.environ.get("HAX_SANITIZER_SKIP")
if _sanitizer_skip:
    print(f"SKIP: {_sanitizer_skip}", file=sys.stderr)
    sys.exit(77)

# Meson builds the extension; HAX_EXTENSION_DIR points at the one under test.
sys.path.insert(0, str(REPO_ROOT / "bindings" / "python"))
try:
    import hax
except ImportError as exc:
    print(f"SKIP: the cffi extension is not built ({exc})", file=sys.stderr)
    sys.exit(77)


def test_plain_turn() -> None:
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:
        expect(agent.model == "mock-model", "model resolves from the mock provider")
        reply = agent.send("hello")
        expect(bool(reply), f"a plain turn returns assistant text (got {reply!r})")
        kinds = [item["kind"] for item in agent.items]
        expect("user" in kinds and "assistant" in kinds, f"history records the turn: {kinds}")


def test_host_tool_runs() -> None:
    use_mock("python_tool.txt")
    seen = []
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            seen.append(order_id)
            return "two widgets"

        agent.send("order 4417?")
        expect(seen == ["4417"], f"host tool receives parsed arguments (got {seen})")
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(outputs == ["two widgets"], f"tool output reaches history (got {outputs})")


def test_host_tool_is_advertised() -> None:
    """The gap a mock script cannot expose: a scripted call proves dispatch, not advertisement.

    A live model can only call a tool it was told about, so assert the advertised list itself.
    """
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:
        builtins = {t["name"] for t in agent.tools}
        expect("lookup_order" not in builtins, "the tool is absent before registration")

        @agent.tool
        def lookup_order(order_id: str, include_history: bool = False):
            """Return the contents of an order.

            A second paragraph is rationale for the reader, not for the model.
            """
            return "two widgets"

        advertised = {t["name"]: t for t in agent.tools}
        expect("lookup_order" in advertised, f"the tool is advertised: {sorted(advertised)}")
        expect(builtins <= set(advertised), "registering does not displace the built-ins")

        definition = advertised.get("lookup_order", {})
        expect(
            definition.get("description") == "Return the contents of an order.",
            f"the docstring summary alone becomes the description ({definition.get('description')!r})",
        )
        parameters = {p["name"]: p for p in definition.get("parameters", [])}
        expect(set(parameters) == {"order_id", "include_history"},
               f"both parameters are advertised (got {sorted(parameters)})")
        expect(parameters.get("order_id", {}).get("type") == "string",
               "an str annotation becomes a string property")
        expect(parameters.get("order_id", {}).get("required") is True,
               "a parameter with no default is required")
        expect(parameters.get("include_history", {}).get("type") == "boolean",
               "a bool annotation becomes a boolean property")
        expect(parameters.get("include_history", {}).get("required") is False,
               "a parameter with a default is optional")


def test_unannotated_and_novel_types_still_advertise() -> None:
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def mixed(plain, count: int, tags: list, ratio: float):
            """Take assorted arguments."""
            return "ok"

        definition = {t["name"]: t for t in agent.tools}["mixed"]
        types = {p["name"]: p["type"] for p in definition["parameters"]}
        expect(types == {"plain": "string", "count": "integer", "tags": "array",
                         "ratio": "number"},
               f"annotations map onto JSON types, unannotated falls back to string (got {types})")


def test_kwargs_tool_leaves_the_builtin_definition_alone() -> None:
    """Shadowing dispatch must not silently narrow what the model knows about the built-in."""
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:
        before = {t["name"]: t for t in agent.tools}
        expect("read" in before, "read is a built-in")

        @agent.tool
        def read(**arguments):
            return "shadowed"

        after = {t["name"]: t for t in agent.tools}
        expect(len(after) == len(before), "no entry is added for a kwargs-only shadow")
        expect(after["read"] == before["read"],
               "the built-in read definition is untouched, so its arguments still reach the host")
        expect(agent._tools.get("read") is not None, "dispatch is still redirected to the host")


def test_declared_shadow_replaces_the_builtin_definition() -> None:
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as agent:
        count_before = len(agent.tools)

        @agent.tool
        def read(path: str):
            """Read through the host instead."""
            return "shadowed"

        advertised = [t for t in agent.tools if t["name"] == "read"]
        expect(len(advertised) == 1, f"one entry per name (got {len(advertised)})")
        expect(len(agent.tools) == count_before, "replacement, not an addition")
        expect(advertised[0]["description"] == "Read through the host instead.",
               "the host's declaration wins over the built-in's")
        expect([p["name"] for p in advertised[0]["parameters"]] == ["path"],
               "the advertised parameters are the host's")


def test_builtin_tool_still_runs() -> None:
    use_mock("tool_roundtrip.txt")
    workdir = scratch / "work"
    workdir.mkdir(exist_ok=True)
    marker = workdir / "out.txt"
    if marker.exists():
        marker.unlink()
    previous = Path.cwd()
    os.chdir(workdir)
    try:
        with hax.Agent(provider="mock") as agent:
            agent.send("go")
    finally:
        os.chdir(previous)
    expect(marker.exists(), "an unshadowed call still runs hax's own bash tool")
    if marker.exists():
        expect(marker.read_text() == "marker42\n", "built-in tool produced the scripted content")


def test_builtin_tool_inherits_the_provider_selection() -> None:
    """A dispatched built-in must receive this session's selection, not a process-wide one.

    The selection moved onto agent_session and travels through tool_run_ctx, so a binding that
    builds a run context without it silently stops exporting HAX_PROVIDER to subprocesses.
    """
    use_mock("tool_env.txt")
    workdir = scratch / "work-env"
    workdir.mkdir(exist_ok=True)
    marker = workdir / "env.txt"
    if marker.exists():
        marker.unlink()
    previous = Path.cwd()
    os.chdir(workdir)
    try:
        with hax.Agent(provider="mock", model="mock-model") as agent:
            agent.send("go")
    finally:
        os.chdir(previous)
    expect(marker.exists(), "the built-in bash tool ran")
    if marker.exists():
        recorded = marker.read_text().strip()
        expect(recorded == "p=mock m=mock-model",
               f"the child inherited this session's selection (got {recorded!r})")


def test_host_exception_propagates_and_history_stays_paired() -> None:
    use_mock("python_tool.txt")
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            raise ValueError("database is down")

        raised = None
        try:
            agent.send("order 4417?")
        except ValueError as exc:
            raised = exc
        expect(raised is not None, "the host exception reaches the caller")
        expect(str(raised) == "database is down", f"the original exception survives ({raised})")

        kinds = [i["kind"] for i in agent.items]
        expect(
            kinds.count("tool_call") == kinds.count("tool_result"),
            f"every call is paired with a result after an aborted turn: {kinds}",
        )


def test_database_example_enforces_read_only() -> None:
    """The shipped example's guard must actually reject a write, not rely on the prompt."""
    use_mock("database_agent.txt")
    sys.path.insert(0, str(REPO_ROOT / "bindings" / "python"))
    from example_database import build_demo_database, register_tools

    connection = build_demo_database()
    try:
        with hax.Agent(provider="mock") as agent:
            executed = register_tools(agent, connection)
            agent.send("which customer spent the most?")

        # items stays readable after close, which is where callers usually inspect it.
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(
            any("only SELECT and WITH queries are allowed" in o for o in outputs),
            f"a write is refused with a recoverable error (got {outputs})",
        )
        expect(
            all("drop" not in statement.lower() for statement in executed),
            f"the refused statement never reached sqlite (log: {executed})",
        )
        survived = connection.execute(
            "SELECT name FROM sqlite_master WHERE name = 'orders'"
        ).fetchone()
        expect(survived is not None, "the table the model tried to drop is still there")
    finally:
        connection.close()


def test_skipped_calls_carry_hax_markers() -> None:
    """A call the loop declines to dispatch must read the same here as from the C frontends."""
    use_mock("python_tool_batch.txt")
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            raise ValueError("database is down")

        try:
            agent.send("orders 4417 and 4418?")
        except ValueError:
            pass

        results = [i for i in agent.items if i["kind"] == "tool_result"]
        expect(len(results) == 2, f"both calls in the batch are paired (got {len(results)})")
        if len(results) == 2:
            skipped = results[1]
            expect(
                skipped["output"] == "[interrupted]",
                f"the skipped call uses hax's own marker (got {skipped['output']!r})",
            )
            expect(
                skipped["origin"] == "skipped",
                f"the skipped call is marked as skipped (got {skipped['origin']!r})",
            )
        ran = [i for i in agent.items if i["kind"] == "tool_result" and i["origin"] == ""]
        expect(len(ran) == 1, f"only the dispatched call reads as ordinary output: {ran}")


def test_malformed_arguments_are_recoverable() -> None:
    """A model that gets the call wrong should be told, not have the turn ended under it."""
    use_mock("python_tool_badargs.txt")
    calls = []
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            calls.append(order_id)
            return "two widgets"

        reply = agent.send("order 4417?")
        expect(calls == [], f"a call the tool cannot accept never reaches it (got {calls})")
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(len(outputs) == 3, f"every malformed call is still paired (got {outputs})")
        expect(
            all(o.startswith("error: ") for o in outputs),
            f"each one comes back as a tool error (got {outputs})",
        )
        expect(bool(reply), "the turn runs to completion rather than raising")


def test_structured_return_values_are_json() -> None:
    """A dict must reach the model as JSON, not as a Python repr it cannot parse."""
    use_mock("python_tool.txt")
    with hax.Agent(provider="mock") as agent:

        @agent.tool
        def lookup_order(order_id):
            return {"id": order_id, "items": ["widget", "widget"], "shipped": None}

        agent.send("order 4417?")
        outputs = [i["output"] for i in agent.items if i["kind"] == "tool_result"]
        expect(
            outputs == ['{"id": "4417", "items": ["widget", "widget"], "shipped": null}'],
            f"a dict return value is serialized as JSON (got {outputs})",
        )


def test_cancel_from_another_thread_stops_the_turn() -> None:
    """The GIL is released for the loop, so a second thread can reach cancel() mid-stream."""
    use_mock("python_tool_cancel.txt")
    with hax.Agent(provider="mock") as agent:
        stopper = threading.Timer(0.4, agent.cancel)
        stopper.start()
        started = time.monotonic()
        cancelled = False
        try:
            agent.send("tell me something slowly")
        except hax.HaxCancelled:
            cancelled = True
        finally:
            stopper.cancel()
        elapsed = time.monotonic() - started

        expect(cancelled, "cancel() from another thread raises HaxCancelled out of send()")
        expect(elapsed < 8.0, f"the turn stops promptly rather than running out ({elapsed:.1f}s)")
        kinds = [i["kind"] for i in agent.items]
        expect(
            kinds.count("tool_call") == kinds.count("tool_result"),
            f"a cancelled turn leaves paired history: {kinds}",
        )

        # The latch is cleared per send(), so the conversation stays usable afterwards.
        use_mock("hello.txt")
        expect(bool(agent.send("still there?")), "a later send() runs normally after a cancel")


def test_cancel_stops_only_the_targeted_agent() -> None:
    """Cancellation is per agent, so one stopped turn leaves its siblings running.

    The flags used to be process-wide: cancelling either agent stopped whichever turn noticed
    first, and each send() cleared the other's pending request on the way in.
    """
    use_mock("interrupt_stall.txt")
    with hax.Agent(provider="mock") as target, hax.Agent(provider="mock") as bystander:
        outcome: dict[str, str] = {}

        def run(name: str, agent) -> None:
            try:
                agent.send(f"stall for {name}")
                outcome[name] = "completed"
            except hax.HaxCancelled:
                outcome[name] = "cancelled"
            except BaseException as exc:
                outcome[name] = f"raised {exc!r}"

        threads = [
            threading.Thread(target=run, args=("target", target)),
            threading.Thread(target=run, args=("bystander", bystander)),
        ]
        for thread in threads:
            thread.start()
        # Both turns stall for two seconds; cancel one well inside that window.
        time.sleep(0.4)
        target.cancel()
        for thread in threads:
            thread.join()

        expect(outcome.get("target") == "cancelled",
               f"the cancelled agent stopped ({outcome.get('target')})")
        expect(outcome.get("bystander") == "completed",
               f"its sibling ran to completion ({outcome.get('bystander')})")

        # Each agent's history is its own and stays paired whichever way its turn ended.
        for agent in (target, bystander):
            kinds = [i["kind"] for i in agent.items]
            expect(kinds.count("tool_call") == kinds.count("tool_result"),
                   f"history stays paired: {kinds}")

        use_mock("hello.txt")
        expect(bool(target.send("still there?")), "the cancelled agent is usable again")


async def _send_async(agent, prompt: str) -> str:
    """The pattern bindings/python/example_async.py documents: task cancellation -> agent.cancel().

    A thread cannot be interrupted from outside, so the executor future is shielded, hax is asked
    to stop, and the turn unwinds through HaxCancelled.
    """
    loop = asyncio.get_running_loop()
    future = loop.run_in_executor(None, agent.send, prompt)
    try:
        return await asyncio.shield(future)
    except asyncio.CancelledError:
        agent.cancel()
        try:
            await future
        except hax.HaxCancelled:
            pass
        raise


def test_asyncio_runs_agents_concurrently() -> None:
    """send() releases the GIL, so a thread executor is all asyncio needs — no async API required.

    The mock stalls each turn for two seconds, so three of them overlapping have to finish in
    well under the six a serial run would take.
    """
    use_mock("interrupt_stall.txt")

    async def scenario() -> None:
        with hax.Agent(provider="mock") as one, hax.Agent(provider="mock") as two, \
             hax.Agent(provider="mock") as three:
            started = time.monotonic()
            replies = await asyncio.gather(
                _send_async(one, "one"), _send_async(two, "two"), _send_async(three, "three")
            )
            elapsed = time.monotonic() - started
            expect(all(replies), "every concurrent turn returned text")
            expect(elapsed < 4.5, f"three 2s turns overlapped rather than serializing ({elapsed:.1f}s)")

        # Cancelling one task stops one agent. Fresh agents: the script above is one turn long.
        with hax.Agent(provider="mock") as target, hax.Agent(provider="mock") as bystander:
            stopped = asyncio.create_task(_send_async(target, "cancel me"))
            survivor = asyncio.create_task(_send_async(bystander, "let me finish"))
            await asyncio.sleep(0.4)
            stopped.cancel()

            cancelled = False
            try:
                await stopped
            except asyncio.CancelledError:
                cancelled = True
            expect(cancelled, "cancelling the task cancelled that agent's turn")
            expect(bool(await survivor), "its sibling's turn completed")

    asyncio.run(scenario())


def test_context_is_compacted_when_it_crosses_the_threshold() -> None:
    """Without the compaction hook a long conversation just grows until the provider rejects it."""
    use_mock("python_tool_compact.txt")
    os.environ["HAX_CONTEXT_LIMIT"] = "1000"  # the scripted turn reports 950 tokens of context
    try:
        with hax.Agent(provider="mock") as agent:

            @agent.tool
            def lookup_order(order_id):
                return "two widgets"

            agent.send("tell me about widgets")
            expect(agent.compactions == 1, f"the loop compacted once (got {agent.compactions})")
            seeds = [i for i in agent.items if i["origin"] == "compact_seed"]
            expect(len(seeds) == 1, f"a summary seed was appended (got {len(seeds)})")
            expect(
                all("widgets" in seed["text"] for seed in seeds),
                f"the seed carries the scripted summary (got {seeds})",
            )
    finally:
        del os.environ["HAX_CONTEXT_LIMIT"]


def test_failed_construction_releases_hax() -> None:
    """hax_init() refuses a second call, so a half-built Agent must not keep the process claimed."""
    use_mock("hello.txt")
    broke = False
    try:
        # Any exception after hax_init() and before the session exists takes this path; an
        # unencodable prompt is the cheapest way to reach it.
        hax.Agent(provider="mock", system_prompt="bad\udcff")
    except UnicodeEncodeError:
        broke = True
    expect(broke, "an unencodable system prompt fails construction")

    with hax.Agent(provider="mock") as agent:
        expect(agent.model == "mock-model", "a later Agent still works after a failed one")


def test_several_agents_coexist() -> None:
    """hax_init() is per process, an Agent is per session, so Agents nest and outlive each other."""
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as first:
        with hax.Agent(provider="mock") as second:
            expect(first.model == "mock-model", "the first agent is usable")
            expect(second.model == "mock-model", "a second agent is usable alongside it")
            expect(first._session != second._session, "each agent owns its own session")

            first.send("hello to the first")
            second.send("hello to the second")

            first_text = " ".join(i["text"] or "" for i in first.items)
            second_text = " ".join(i["text"] or "" for i in second.items)
            expect("hello to the first" in first_text, "the first agent kept its own prompt")
            expect("hello to the second" not in first_text, "and none of its sibling's")
            expect("hello to the second" in second_text, "the second agent kept its own prompt")
            expect("hello to the first" not in second_text, "and none of its sibling's")

        # Closing one releases only its own session; the runtime stays up for the survivor.
        expect(first.model == "mock-model", "the outer agent survives the inner one closing")
        expect(first.send("still here") != "", "and still runs turns")

    # The last close tears the runtime down, and a later Agent brings it back.
    with hax.Agent(provider="mock") as later:
        expect(later.model == "mock-model", "an Agent works again after the last one closed")


def test_agents_run_concurrently() -> None:
    """Turns run on separate threads: the loop releases the GIL and the state is per session."""
    use_mock("hello.txt")
    with hax.Agent(provider="mock") as first, hax.Agent(provider="mock") as second:
        replies: dict[str, str] = {}
        errors: list[BaseException] = []

        def run(name: str, agent) -> None:
            try:
                replies[name] = agent.send(f"prompt for {name}")
            except BaseException as exc:  # reported, not raised, off the main thread
                errors.append(exc)

        threads = [
            threading.Thread(target=run, args=("first", first)),
            threading.Thread(target=run, args=("second", second)),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        expect(not errors, f"neither concurrent turn raised ({errors})")
        expect(replies.get("first") == "Hello from mock", "the first turn completed")
        expect(replies.get("second") == "Hello from mock", "the second turn completed")
        for name, agent in (("first", first), ("second", second)):
            texts = " ".join(i["text"] or "" for i in agent.items)
            expect(f"prompt for {name}" in texts, f"the {name} agent kept its own prompt")
            other = "second" if name == "first" else "first"
            expect(f"prompt for {other}" not in texts, f"and none of the {other} agent's")


def test_unknown_provider_reports_a_diagnostic() -> None:
    use_mock("hello.txt")
    raised = None
    try:
        hax.Agent(provider="no-such-provider")
    except hax.HaxError as exc:
        raised = exc
    expect(raised is not None, "an unknown provider raises")
    expect("no-such-provider" in str(raised), f"the diagnostic names the provider ({raised})")


test_plain_turn()
test_host_tool_runs()
test_host_tool_is_advertised()
test_unannotated_and_novel_types_still_advertise()
test_kwargs_tool_leaves_the_builtin_definition_alone()
test_declared_shadow_replaces_the_builtin_definition()
test_builtin_tool_still_runs()
test_builtin_tool_inherits_the_provider_selection()
test_host_exception_propagates_and_history_stays_paired()
test_skipped_calls_carry_hax_markers()
test_malformed_arguments_are_recoverable()
test_structured_return_values_are_json()
test_cancel_from_another_thread_stops_the_turn()
test_cancel_stops_only_the_targeted_agent()
test_asyncio_runs_agents_concurrently()
test_context_is_compacted_when_it_crosses_the_threshold()
test_failed_construction_releases_hax()
test_database_example_enforces_read_only()
test_several_agents_coexist()
test_agents_run_concurrently()
test_unknown_provider_reports_a_diagnostic()

sys.exit(1 if failures else 0)
