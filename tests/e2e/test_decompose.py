#!/usr/bin/env python3
"""examples/decompose.cpp: a Markdown objective split across parallel agents, then answered from
their reports. Every mock agent plays the same script, so which report went where is checked
through --dry-run's prompts; the runs check the pipeline, the tool policy, failures and Ctrl-C."""

import os
import signal
import subprocess
import time

import harness

TIMEOUT_S = 20

DOCUMENT = """\
Text before the main heading is not part of any prompt.

# How do the parts fit?

Main context line.

## First part

Context one.

```
## not a heading
```

## Second part ##

Context two.
"""

ANSWER = """
    tool bash {"command":"echo ran > out.txt"}
    end-turn

    text Finding.
    end-turn
    """


def start(document, args=(), script=ANSWER, extra_env=None):
    home, workdir = harness.make_home()
    (workdir / "objective.md").write_text(document)
    env = harness.mock_env(home, script, extra_env)
    proc = subprocess.Popen(
        [os.environ["DECOMPOSE"], *args, "objective.md"],
        cwd=workdir,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        encoding="utf-8",
    )
    return proc, workdir


def run(document, args=(), script=ANSWER, extra_env=None):
    proc, workdir = start(document, args, script, extra_env)
    stdout, stderr = proc.communicate(timeout=TIMEOUT_S)
    return harness.Result(
        subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr), workdir
    )


def test_dry_run_shows_each_agents_prompt():
    """Each part's agent gets the objective, its own part and context; the final agent gets every
    report under its part's title. Fenced headings and a closing '##' are not structure."""
    result = run(DOCUMENT, ["--dry-run"])
    harness.expect(result.returncode == 0, "a dry run exits 0", result)
    out = result.stdout
    part1 = out[out.index("=== part 1 of 2 ===") : out.index("=== part 2 of 2 ===")]
    part2 = out[out.index("=== part 2 of 2 ===") : out.index("=== final ===")]
    final = out[out.index("=== final ===") :]
    for prompt in (part1, part2):
        harness.expect("Overall objective: How do the parts fit?" in prompt, "names the objective")
        harness.expect("Main context line." in prompt, "carries the objective's context")
    harness.expect("Your part: First part" in part1 and "Context one." in part1, "part 1", result)
    harness.expect("## not a heading" in part1, "a fenced heading stays part 1's context", result)
    harness.expect("Your part: Second part\n" in part2, "the closing '##' is dropped", result)
    harness.expect("Context one." not in part2, "part 2 does not see part 1's context", result)
    harness.expect(
        "## Part 1: First part\n\n[the report from part 1]" in final
        and "## Part 2: Second part\n\n[the report from part 2]" in final,
        "the final prompt lists each report under its part",
        result,
    )
    harness.expect("Text before" not in out, "text before the '#' heading is dropped", result)


def test_document_and_usage_errors_exit_2():
    """A document without exactly one '#' and at least one '##' after it is refused, naming the
    line, before any agent runs."""
    cases = [
        ("## Part\n", "'## Part' comes before the '#' main objective"),
        ("# One\n\n## Part\n\n# Two\n", "line 5: a second '#' heading"),
        ("# Only\n\ntext\n", "no '##' headings"),
        ("just text\n", "no '#' heading"),
        ("# \n## Part\n", "line 1: a heading needs text"),
    ]
    for document, message in cases:
        result = run(document)
        harness.expect(result.returncode == 2, f"{message!r} exits 2", result)
        harness.expect(message in result.stderr, f"stderr says {message!r}", result)
        harness.expect(result.stdout == "", "nothing reaches stdout", result)
    result = run(DOCUMENT, ["--tools", "some"])
    harness.expect(result.returncode == 2 and "usage:" in result.stderr, "a bad flag is usage")


def test_parts_run_then_the_final_answer():
    """Every part and the final agent answer; the report puts the answer first. --tools read
    refuses bash, and --tools all runs it."""
    for jobs in ("1", "4"):
        result = run(DOCUMENT, ["--jobs", jobs])
        harness.expect(result.returncode == 0, f"--jobs {jobs} exits 0", result)
        harness.expect(
            result.stdout
            == "# How do the parts fit?\n\nFinding.\n\n## First part\n\nFinding.\n\n"
            "## Second part\n\nFinding.\n",
            f"the report: {result.stdout!r}",
            result,
        )
        harness.expect(not (result.workdir / "out.txt").exists(), "read-only refuses bash")
        harness.expect("[final] How do the parts fit?: done" in result.stderr, "progress", result)
    result = run(DOCUMENT, ["--tools", "all"])
    harness.expect(result.returncode == 0, "--tools all exits 0", result)
    harness.expect((result.workdir / "out.txt").exists(), "--tools all runs bash", result)


def test_failed_parts_are_reported_and_exit_1():
    """When every part fails, the report says why for each and the final agent does not run."""
    result = run(DOCUMENT, extra_env={"HAX_MOCK_SCRIPT": "/nonexistent/script.txt"})
    harness.expect(result.returncode == 1, "a failed run exits 1", result)
    harness.expect(result.stdout.count("_No report: provider error") == 2, "both parts", result)
    harness.expect("_No answer: the final agent did not run._" in result.stdout, "no answer")


CHAIN = """\
# Plan the migration

## Inventory

List what exists.

## Risks
depends: inventory

Assess risks.

## Plan

Depends: Inventory, Risks
Write the plan.
"""


def test_dry_run_shows_dependencies():
    """A dependent part waits for the parts it names and gets their reports; the depends line
    is not context."""
    result = run(CHAIN, ["--dry-run"])
    harness.expect(result.returncode == 0, "a dry run exits 0", result)
    out = result.stdout
    harness.expect("=== part 1 of 3 ===" in out, "part 1 waits for nothing", result)
    harness.expect("=== part 2 of 3, after part 1 ===" in out, "part 2 waits for part 1", result)
    harness.expect("=== part 3 of 3, after part 1, 2 ===" in out, "part 3 waits for both", result)
    part1 = out[: out.index("=== part 2 of 3")]
    part3 = out[out.index("=== part 3 of 3") : out.index("=== final ===")]
    harness.expect("builds on" not in part1, "an independent part gets no reports", result)
    harness.expect(
        "### Part 1: Inventory\n\n[the report from part 1]" in part3
        and "### Part 2: Risks\n\n[the report from part 2]" in part3,
        "the dependent prompt carries each dependency's report",
        result,
    )
    harness.expect("depends:" not in out.lower(), "the depends lines are not context", result)


def test_dependency_errors_exit_2():
    """Unknown, ambiguous, self and cyclic dependencies are refused before any agent runs."""
    cases = [
        ("# O\n## A\ndepends: B\n", "line 3: depends on 'B', which is not a part title"),
        ("# O\n## A\n## B\ndepends: a\n## A\n", "'a', which names more than one part"),
        ("# O\n## A\ndepends: A\n", "line 3: a part cannot depend on itself"),
        ("# O\n## A\ndepends: B\n## B\ndepends: A\n## C\n", "a cycle among 'A', 'B'"),
        ("# O\ndepends: A\n## A\n", "line 2: 'depends:' belongs under a '##' part"),
        ("# O\n## A\n## B\ndepends: A,\n", "line 4: 'depends:' needs part titles"),
    ]
    for document, message in cases:
        result = run(document)
        harness.expect(result.returncode == 2, f"{message!r} exits 2", result)
        harness.expect(message in result.stderr, f"stderr says {message!r}", result)


def test_dependent_parts_run_in_order():
    """Each part in a chain starts only after the one before it finished, even with jobs to
    spare: their bash calls never overlap."""
    script = """
        tool bash {"command":"echo start >> ../order.log; sleep 0.2; echo end >> ../order.log"}
        end-turn

        text Finding.
        end-turn
        """
    result = run(CHAIN, ["--tools", "all", "--jobs", "4"], script=script)
    harness.expect(result.returncode == 0, "the chain completes", result)
    order = (result.workdir.parent / "order.log").read_text().split()
    harness.expect(
        order[:6] == ["start", "end"] * 3, f"the parts ran one after another: {order}", result
    )


def test_a_failed_dependency_skips_its_dependents():
    """A part whose dependency failed is skipped, with the reason in the report. Every turn calls
    a tool, so the wrap-up after the turn limit fails too and the part keeps its failure."""
    script = """
        tool read {"path":"nothing.txt"}
        end-turn

        tool read {"path":"nothing.txt"}
        end-turn

        tool read {"path":"nothing.txt"}
        end-turn
        """
    result = run(CHAIN, ["--max-turns", "1"], script=script)
    harness.expect(result.returncode == 1, "a failed run exits 1", result)
    harness.expect(
        "## Inventory\n\n_No report: turn limit reached._" in result.stdout, "it failed", result
    )
    harness.expect(
        "## Risks\n\n_No report: skipped because 'Inventory' has no report._" in result.stdout
        and "## Plan\n\n_No report: skipped because 'Inventory' has no report._" in result.stdout,
        "both dependents were skipped",
        result,
    )


def test_an_agent_at_the_turn_limit_still_reports():
    """An agent that reaches --max-turns gets one more turn, without tools, to write up what it
    found; the report says so, and the run counts it as answered."""
    script = """
        tool read {"path":"notes.txt"}
        end-turn

        text Partial finding.
        end-turn
        """
    result = run(DOCUMENT, ["--max-turns", "1"], script=script)
    harness.expect(result.returncode == 0, "a report written at the limit still answers", result)
    note = "_Written at the turn limit, before the agent finished._\n\nPartial finding.\n"
    harness.expect(result.stdout.count(note) == 3, f"each agent's report: {result.stdout!r}")
    harness.expect(result.stderr.count("done at the turn limit") == 3, "progress says so", result)


def test_the_run_reports_its_cost():
    """stderr ends with what every agent's requests cost together."""
    script = """
        text Finding.
        usage in=100 out=10 cost=0.25
        end-turn
        """
    result = run(DOCUMENT, script=script)
    harness.expect(result.returncode == 0, "the run completes", result)
    harness.expect("cost: $0.7500\n" in result.stderr, f"two parts and the final: {result.stderr}")


def test_ctrl_c_cancels_every_agent():
    """SIGINT cancels the agents in flight, prints the report, and exits 130 without waiting
    for the slow replies."""
    proc, workdir = start(DOCUMENT, script="delay 3000\ntext Slow.\nend-turn\n")
    time.sleep(0.5)
    started = time.monotonic()
    proc.send_signal(signal.SIGINT)
    stdout, stderr = proc.communicate(timeout=TIMEOUT_S)
    result = harness.Result(
        subprocess.CompletedProcess(proc.args, proc.returncode, stdout, stderr), workdir
    )
    harness.expect(proc.returncode == 130, "an interrupted run exits 130", result)
    harness.expect(time.monotonic() - started < 5, "the agents stopped promptly", result)
    harness.expect(stdout.count("_No report: cancelled") == 2, "both parts cancelled", result)


harness.main(globals())
