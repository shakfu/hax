#!/usr/bin/env python3
"""Work one queue with several agents at once, with the host holding the state and the rules.

Three agents run concurrently against a single in-memory board. Each claims a report, triages it
and submits a verdict; the host serializes the queue, records who holds what, and rejects a
submission for a job the caller does not hold or a severity outside the allowed set.

The identity of the caller comes from the closure, not from an argument. A model cannot claim to
be another worker because it is never asked who it is -- the tool it called already belongs to
one agent. Three `hax -p` processes would need an IPC protocol to reach the same queue; here it
is a list and a lock.

    meson setup build-embed -Dembed=true && meson compile -C build-embed

    export ANTHROPIC_API_KEY=...
    uv run python bindings/python/example_shared_state.py --provider anthropic \
        --model claude-sonnet-5 --workers 3

Any configured provider works.
"""

from __future__ import annotations

import argparse
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax
from hermetic import confine, seal

SEVERITIES = ("blocker", "major", "minor", "trivial")
COMPONENTS = ("provider", "tools", "terminal", "config", "session", "build", "unknown")

WORKER_PROMPT = (
    "You triage incoming bug reports. Loop: call claim_job, decide a severity and component for "
    "the report you were handed, call submit_triage, and claim again. Stop when claim_job says "
    "the queue is empty. Judge severity by user impact, not by how the report is worded. A "
    "rejected submission tells you what was wrong; fix it and resubmit."
)

REPORTS = [
    "hax exits with 'HTTP 401' at startup after I rotated my API key. Nothing else works.",
    "The model picker lists models in an order that changes between runs.",
    "Trailing whitespace in a config.json comment produces a parse error with no line number.",
    "Ctrl-R prompt recall shows prompts from a different project directory.",
    "The spinner keeps drawing after the terminal is resized narrower than 20 columns.",
    "A bash tool call that writes 400MB of output makes the process grow until the OOM killer "
    "takes it.",
    "make wheel succeeds but the wheel fails to import on a colleague's machine.",
    "Resuming a session with --resume loses the last assistant message.",
    "Typo in the --help text: 'non-interative'.",
]


@dataclass
class Job:
    id: str
    report: str
    holder: str | None = None
    verdict: dict[str, str] | None = None


@dataclass
class Board:
    """The shared queue. Every method is called from an agent's turn, on that agent's thread."""

    jobs: list[Job]
    lock: threading.Lock = field(default_factory=threading.Lock)
    log: list[str] = field(default_factory=list)

    def claim(self, worker: str) -> dict[str, str] | str:
        with self.lock:
            if any(job.holder == worker and job.verdict is None for job in self.jobs):
                return "error: you already hold a job; submit it before claiming another"
            for job in self.jobs:
                if job.holder is None:
                    job.holder = worker
                    self.log.append(f"{worker} claimed {job.id}")
                    return {"job_id": job.id, "report": job.report}
            return "the queue is empty"

    def submit(self, worker: str, job_id: str, severity: str, component: str,
               summary: str) -> str:
        # Validation lives here rather than in the prompt: the host owns the queue, so the host
        # decides what a valid verdict is.
        if severity not in SEVERITIES:
            return f"error: severity must be one of {', '.join(SEVERITIES)}"
        if component not in COMPONENTS:
            return f"error: component must be one of {', '.join(COMPONENTS)}"
        with self.lock:
            job = next((j for j in self.jobs if j.id == job_id), None)
            if job is None:
                return f"error: no job {job_id!r}"
            if job.holder != worker:
                return f"error: {job_id} is not yours to submit"
            if job.verdict is not None:
                return f"error: {job_id} is already triaged"
            job.verdict = {"severity": severity, "component": component, "summary": summary}
            self.log.append(f"{worker} triaged {job.id} as {severity}/{component}")
            return f"recorded {job_id}"

    def outstanding(self) -> int:
        with self.lock:
            return sum(1 for job in self.jobs if job.verdict is None)


def register_tools(agent: hax.Agent, board: Board, worker: str) -> None:
    """Give one agent its view of the board, and nothing else.

    The worker name is closed over, never passed in, so a model cannot claim to be another
    worker. seal() refuses hax's own tools, so the queue is the only state a worker can reach.
    """
    seal(agent)

    @agent.tool
    def claim_job():
        """Take the next untriaged report off the queue, or say that it is empty."""
        return board.claim(worker)

    @agent.tool
    def submit_triage(job_id: str, severity: str, component: str, summary: str):
        """Record a verdict for the job you currently hold.

        severity is one of blocker, major, minor, trivial. component is one of provider, tools,
        terminal, config, session, build, unknown. summary is one sentence.
        """
        return board.submit(worker, job_id, severity, component, summary)


def run_worker(name: str, board: Board, provider: str, model: str | None,
               failures: list[str]) -> None:
    try:
        with hax.Agent(provider=provider, model=model, system_prompt=WORKER_PROMPT) as agent:
            register_tools(agent, board, name)
            agent.send("Work the queue until it is empty, then report how many you triaged.")
    except hax.HaxError as exc:  # reported, not raised: this runs off the main thread
        failures.append(f"{name}: {exc}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workers", type=int, default=3)
    parser.add_argument("--reports", type=int, default=len(REPORTS),
                        help=f"how many of the {len(REPORTS)} reports to queue")
    parser.add_argument("--provider", default="anthropic")
    parser.add_argument("--model", default=None, help="defaults to the provider's configuration")
    args = parser.parse_args()

    confine()
    board = Board([Job(f"BUG-{index:03d}", report)
                   for index, report in enumerate(REPORTS[:args.reports], 1)])
    failures: list[str] = []

    names = [f"worker-{index}" for index in range(1, args.workers + 1)]
    threads = [
        threading.Thread(target=run_worker,
                         args=(name, board, args.provider, args.model, failures))
        for name in names
    ]
    print(f"{len(board.jobs)} reports, {len(names)} agent{'s' if len(names) > 1 else ''}")
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    for line in board.log:
        print(f"  {line}")
    print()
    for job in board.jobs:
        verdict = job.verdict
        if verdict is None:
            print(f"  {job.id}  untriaged")
        else:
            print(f"  {job.id}  {verdict['severity']:<8} {verdict['component']:<9} "
                  f"{verdict['summary'][:50]}")

    if failures:
        for failure in failures:
            print(f"failed: {failure}", file=sys.stderr)
        return 1
    return 1 if board.outstanding() else 0


if __name__ == "__main__":
    sys.exit(main())
