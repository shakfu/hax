#!/usr/bin/env python3
"""Subagent budgets: a root run's limits bind the hax processes it starts through bash, and a
child's refusal or budget stop reaches the shell that started it."""

import json
import shlex

import harness


def run_root(child_script: str, children: list[str], extra_env: dict[str, str]):
    """Run a root whose one bash call starts each child in turn, logging to out.txt."""
    home, workdir = harness.make_home()
    (workdir / "child.txt").write_text(child_script)
    hax = shlex.quote(str(harness.hax_binary()))
    command = "; ".join(
        f"HAX_MOCK_SCRIPT=child.txt {hax} -p {shlex.quote(name)} >> out.txt 2>&1; "
        f"echo {name}=$? >> out.txt"
        for name in children
    )
    root_script = f"""
        tool bash {json.dumps({"command": command})}
        end-turn

        text root done
        end-turn
        """
    proc = harness.spawn_hax(["-p", "go"], root_script, home, workdir, extra_env)
    stdout, stderr = proc.communicate(timeout=30)
    result = harness.spawned_result(proc, stdout, stderr, workdir)
    out = workdir / "out.txt"
    return result, out.read_text() if out.exists() else ""


def test_agent_limit_spans_children():
    """With max_agents 1, the first child runs and the second is refused."""
    result, log = run_root(
        "text child answer\nend-turn\n",
        ["first", "second"],
        {"HAX_SUBAGENT_MAX_AGENTS": "1"},
    )
    harness.expect(result.returncode == 0, "the root run completes", result)
    harness.expect("child answer" in log and "first=0" in log, "the first child ran", result)
    harness.expect(
        "subagent limit reached (1 started)" in log and "second=1" in log,
        f"the second child is refused with a reason:\n{log}",
        result,
    )


def test_token_budget_ends_child_with_final_answer():
    """A child that spends the token budget gets one tool-free turn to answer, then stops; later
    children are refused."""
    child = """\
tool bash {"command":"echo child step"}
usage in=80 out=30
end-turn

text partial findings
end-turn

text unreachable
end-turn
"""
    result, log = run_root(child, ["first", "second"], {"HAX_SUBAGENT_MAX_TOKENS": "100"})
    harness.expect(result.returncode == 0, "the root run completes", result)
    harness.expect(
        "partial findings" in log
        and "subagent token budget spent (110 of 100); stopped after a final answer" in log
        and "first=1" in log,
        f"the first child prints its final answer and the reason:\n{log}",
        result,
    )
    harness.expect("unreachable" not in log, "no request follows the final answer", result)
    harness.expect(
        "subagent token budget spent (110 of 100); run the task directly" in log
        and "second=1" in log,
        f"the second child is refused:\n{log}",
        result,
    )


harness.main(globals())
