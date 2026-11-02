#!/usr/bin/env python3
"""Positional words without -p become the REPL's first message, and stdin then supplies the
following ones. Without positional words the REPL waits for input as before."""

import subprocess

import harness


def run_repl(args: list[str], stdin: str) -> harness.Result:
    home, workdir = harness.make_home()
    env = harness.mock_env(home, "hello.txt", {"HAX_TRANSCRIPT": str(workdir / "transcript.txt")})
    proc = subprocess.run(
        [str(harness.hax_binary()), *args],
        cwd=workdir,
        env=env,
        input=stdin,
        capture_output=True,
        encoding="utf-8",
        timeout=30,
    )
    return harness.Result(proc, workdir)


def user_messages(result: harness.Result) -> list[str]:
    path = result.workdir / "transcript.txt"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    return [block.split("\n\n", 1)[0] for block in text.split("── user ──\n")[1:]]


result = run_repl(["do", "it"], "second\n")
harness.expect(result.returncode == 0, "exit status is 0", result)
harness.expect("Hello from mock" in result.stdout, "the first turn runs unprompted", result)
harness.expect(user_messages(result) == ["do it", "second"],
               "joined words are the first message and stdin the next", result)

result = run_repl([], "")
harness.expect(result.returncode == 0, "exit status is 0", result)
harness.expect("Hello from mock" not in result.stdout, "no turn runs without input", result)
harness.expect(user_messages(result) == [], "no message is submitted", result)
