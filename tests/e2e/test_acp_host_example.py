#!/usr/bin/env python3
"""examples/acp_host.cpp: the C++ host runs hax --acp, its lookup_order tool answers the model,
and a built-in that asks permission is refused."""

import os
import subprocess

import harness

TIMEOUT_S = 20


def run_example(script, prompt="what is in order 4417?"):
    home, workdir = harness.make_home()
    transcript = home / "transcript.txt"
    env = harness.mock_env(home, script, {"HAX_TRANSCRIPT": str(transcript)})
    env["HAX_BIN"] = str(harness.hax_binary())
    proc = subprocess.run(
        [os.environ["ACP_HOST_EXAMPLE"], prompt],
        cwd=workdir,
        env=env,
        capture_output=True,
        encoding="utf-8",
        timeout=TIMEOUT_S,
    )
    return harness.Result(proc, workdir), transcript.read_text()


def test_the_host_tool_answers_and_the_reply_prints():
    result, transcript = run_example(
        """
        tool lookup_order {"order_id":"4417"}
        end-turn

        tool lookup_order {"order_id":"9999"}
        end-turn

        tool bash {"command":"echo ran > out.txt"}
        end-turn

        text Order 4417 holds two widgets.
        end-turn
        """
    )
    harness.expect(result.returncode == 0, "the example exits 0", result)
    harness.expect(
        result.stdout == "Order 4417 holds two widgets.\n", f"stdout: {result.stdout!r}", result
    )
    harness.expect("2x widget, 1x sprocket" in transcript, "the model read the host's order")
    harness.expect("error: no order 9999" in transcript, "a missing order reads as an error")
    harness.expect(not (result.workdir / "out.txt").exists(), "the refused bash did not run")


harness.main(globals())
