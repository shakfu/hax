#!/usr/bin/env python3
"""Inside a herdr pane, the REPL reports idle at the prompt and working during a turn, names its
session for herdr to resume once the session is recorded, and releases the pane on exit."""

import time
from pathlib import Path

import harness

# Each call writes its arguments, one per line, to its own file, published by rename.
FAKE_HERDR = """#!/bin/sh
/usr/bin/printf '%s\\n' "$@" > "$RECORD_DIR/tmp.$$" && /bin/mv "$RECORD_DIR/tmp.$$" "$RECORD_DIR/rec.$$"
"""


def herdr_env(directory: Path) -> dict[str, str]:
    fake = directory / "herdr"
    fake.write_text(FAKE_HERDR)
    fake.chmod(0o755)
    return {
        "HERDR_ENV": "1",
        "HERDR_PANE_ID": "w1:p1",
        "HERDR_BIN_PATH": str(fake),
        "HERDR_SOCKET_PATH": str(directory / "herdr.sock"),
        "RECORD_DIR": str(directory),
        # The mock provider defaults to an unrecorded session; the resume command needs the file.
        "HAX_NO_SESSION": "0",
    }


def records(directory: Path, count: int, timeout: float = 10) -> list[list[str]]:
    """Wait for `count` calls and return their arguments in the order hax sent them."""
    deadline = time.monotonic() + timeout
    while True:
        calls = [path.read_text().splitlines() for path in directory.glob("rec.*")]
        if len(calls) >= count or time.monotonic() > deadline:
            break
        time.sleep(0.01)
    harness.expect(len(calls) == count, f"{count} herdr calls, got {len(calls)}: {calls}")
    return sorted(calls, key=lambda argv: int(argv[argv.index("--seq") + 1]))


def without_seq(argv: list[str]) -> list[str]:
    at = argv.index("--seq")
    return argv[:at] + argv[at + 2 :]


def test_reports_turn_lifecycle():
    directory = harness.scratch_dir()
    term = harness.Terminal(
        """
        text Hello from mock
        end-turn
        """,
        extra_env=herdr_env(directory),
    )
    term.wait_for("❯")
    report = ["pane", "report-agent", "w1:p1", "--source", "hax", "--agent", "hax", "--state"]
    harness.expect(
        without_seq(records(directory, 1)[0]) == [*report, "idle"],
        "idle at the first prompt, with nothing to resume yet",
    )

    term.type("hi")
    term.send("Enter")
    term.expect_screen(
        """
        ▌ hax › mock · mock-model
        ▌ ctrl-d quit · try /help

        ▌ hi

        Hello from mock

        {*}s

        ❯
        """
    )
    term.send("C-d")
    harness.expect(term.wait_exit() == 0, "Ctrl-D exits 0")

    calls = [without_seq(argv) for argv in records(directory, 4)]
    harness.expect(calls[1] == [*report, "working"], f"working during the turn: {calls[1]}")
    idle = calls[2]
    harness.expect(
        idle[: len(report) + 2] == [*report, "idle", "--agent-session-id"],
        f"idle after the turn names the session: {idle}",
    )
    session_id = idle[len(report) + 2]
    harness.expect(
        idle[len(report) + 3 :] == ["--", "hax", f"--resume={session_id}"],
        f"resume command for the session: {idle}",
    )
    harness.expect(
        f"resume with: hax --resume={session_id}" in term.screen(),
        "the session herdr resumes is the one hax recorded",
    )
    harness.expect(
        calls[3] == ["pane", "release-agent", "w1:p1", "--source", "hax", "--agent", "hax"],
        f"released on exit: {calls[3]}",
    )


harness.main(globals())
