#!/usr/bin/env python3
"""Let one agent delegate to others by making delegation a tool it can call.

The supervisor holds no tools of its own beyond `ask` and `ask_all`. Behind those sit specialist
agents -- a librarian that can read the repository, a writer, a critic -- each its own
conversation with its own tools and its own system prompt. The supervisor decides who to ask and
when; the split is a decision the model makes, not a pipeline the script hard-codes.

Two properties of the binding make this work, and neither is available to a `hax -p` subprocess:

- A specialist is built once and reused, so it remembers what it was asked before. A follow-up
  costs one turn instead of restating the whole thread.
- A specialist's tools run in this process. The librarian's read_file is a Python function that
  enforces its own root, not a shell command the model composes. Every other specialist is
  sealed: reading the repository is the librarian's job, and a critic that can also read it
  spends its turn redoing that work instead of checking the draft it was handed.

    meson setup build-embed -Dembed=true && meson compile -C build-embed

    export ANTHROPIC_API_KEY=...
    uv run python bindings/python/example_supervisor.py --provider anthropic \
        --model claude-sonnet-5 "explain how hax picks a provider, and check it against the docs"

Any configured provider works. `--deadline SECONDS` cancels the supervisor and every specialist
still running, which is what a per-agent cancel is for.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax
from hermetic import seal

REPO_ROOT = Path(__file__).resolve().parents[2]
MAX_FILE_BYTES = 40000
MAX_LISTED = 400
# Only consulted when the root is not a git checkout; git's own ignore rules cover the rest.
# "build" is a prefix match because the project's sanitizer and release trees are build-asan,
# build-embed and so on.
IGNORED_DIRS = frozenset({"dist", "node_modules", "__pycache__", "subprojects"})

# Each specialist sees only the text of the question it is asked -- it has no view of the other
# conversations. Saying so is what stops the critic being sent a draft with no evidence to check
# it against, which costs a refusal and a second delegation to correct.
SUPERVISOR_PROMPT = (
    "You coordinate a small team and do no research yourself. Call list_specialists first, then "
    "delegate with ask or ask_all. A specialist sees only the question you send it, so quote the "
    "evidence you want used inside the question itself. Ground every factual claim in something "
    "a specialist reported, and send the critic your draft together with the evidence it rests "
    "on, in one question, before you answer. Answer in at most two paragraphs."
)

SPECIALISTS = {
    "librarian": (
        "You answer questions about a source repository from its files. Use list_files and "
        "read_file; never guess at a file you have not read. Quote the lines that carry the "
        "answer and name the file each quote came from."
    ),
    "writer": (
        "You turn notes into short technical prose. Plain sentences, no metaphor, no filler. "
        "Write only what the notes support, and say what is missing rather than inventing it."
    ),
    "critic": (
        "You check a draft against the evidence supplied with it. List every claim the evidence "
        "does not support, then state whether the draft is publishable as written."
    ),
}



def _tracked_files(root: Path, target: Path) -> list[str]:
    """Files under `target`, by preference the ones the repository tracks.

    A plain walk is unusable on a working repository: build directories outnumber the source by
    an order of magnitude, so a truncated listing is all object files and the librarian never
    learns that docs/ exists. `git ls-files` is the project's own answer to which files matter,
    and it needs no exclusion list to maintain. Anything that is not a git checkout falls back
    to the walk, minus the obvious build output.
    """
    listing = subprocess.run(
        ["git", "-C", str(target), "ls-files", "--cached", "--others",
         "--exclude-standard"],
        capture_output=True, text=True, check=False,
    )
    if listing.returncode == 0:
        prefix = target.relative_to(root)
        return sorted(str(prefix / name) if prefix.parts else name
                      for name in listing.stdout.splitlines() if name)

    return sorted(
        str(path.relative_to(root))
        for path in target.rglob("*")
        if path.is_file()
        and not any(part.startswith((".", "build")) or part in IGNORED_DIRS
                    for part in path.parts)
    )


@dataclass
class Delegation:
    """One ask, with what it cost. A supervisor run is opaque without this: the wall clock is
    spent inside specialist turns, and which specialist, over how many round trips, is the only
    thing that explains a slow run."""

    specialist: str
    question: str
    answer: str
    seconds: float
    tool_calls: int
    items: int


class Team:
    """The specialists, built on first use and kept for the rest of the run.

    An Agent is a session rather than a process, so these coexist with the supervisor and with
    each other. The lock covers construction, which hax serializes anyway, and the roster itself;
    turns run concurrently without it.
    """

    def __init__(self, provider: str, model: str | None, root: Path):
        self._provider = provider
        self._model = model
        # Resolved here rather than trusting the caller: the guard below compares against it, so
        # an unresolved root refuses every read on a platform where /tmp is a symlink.
        self._root = root.resolve()
        self._agents: dict[str, hax.Agent] = {}
        self._lock = threading.Lock()
        self.transcript: list[Delegation] = []

    def _build(self, name: str) -> hax.Agent:
        agent = hax.Agent(
            provider=self._provider, model=self._model, system_prompt=SPECIALISTS[name]
        )
        if name == "librarian":
            self._equip_librarian(agent)
        else:
            # A specialist that was given no tools still inherits hax's own. Unsealed, the
            # critic answered a "check this draft" question with 27 tool calls, re-reading the
            # repository the librarian had already been paid to read -- and it held write, edit
            # and bash against a live checkout while doing it.
            seal(agent)
        return agent

    def _equip_librarian(self, agent: hax.Agent) -> None:
        root = self._root

        @agent.tool
        def list_files(subdirectory: str = ""):
            """List the files available under the repository root."""
            target = (root / subdirectory).resolve()
            if not target.is_relative_to(root) or not target.is_dir():
                return f"error: {subdirectory!r} is not a directory inside the root"
            names = _tracked_files(root, target)
            if len(names) > MAX_LISTED:
                return "\n".join(names[:MAX_LISTED]) + f"\n[{len(names) - MAX_LISTED} more]"
            return "\n".join(names) or "(empty)"

        @agent.tool
        def read_file(path: str):
            """Read one file from the repository."""
            # The model is not a trust boundary: the root is enforced here, where the filesystem
            # is, rather than in the prompt that asked it to stay inside.
            target = (root / path).resolve()
            if not target.is_relative_to(root) or not target.is_file():
                return f"error: {path!r} is not a readable file inside the root"
            body = target.read_text(errors="replace")
            if len(body) > MAX_FILE_BYTES:
                return body[:MAX_FILE_BYTES] + "\n[truncated]"
            return body

    def ask(self, name: str, question: str) -> str:
        if name not in SPECIALISTS:
            return f"error: no specialist named {name!r}; try {', '.join(SPECIALISTS)}"
        with self._lock:
            agent = self._agents.get(name)
            if agent is None:
                agent = self._agents[name] = self._build(name)
        started = time.monotonic()
        answer = agent.send(question)
        elapsed = time.monotonic() - started
        items = agent.items
        self.transcript.append(
            Delegation(
                specialist=name,
                question=question,
                answer=answer,
                seconds=elapsed,
                # Cumulative for this specialist: it keeps its conversation between delegations,
                # so the count rising faster than the questions is the context growing.
                tool_calls=sum(1 for item in items if item["kind"] == "tool_call"),
                items=len(items),
            )
        )
        return answer

    def cancel_all(self) -> None:
        """Stop every specialist currently in a turn. Cancellation is per agent, so this has to
        reach each one; cancelling the supervisor alone would leave a nested turn running."""
        with self._lock:
            for agent in self._agents.values():
                agent.cancel()

    def close(self) -> None:
        with self._lock:
            for agent in self._agents.values():
                agent.close()
            self._agents.clear()


def register_delegation(supervisor: hax.Agent, team: Team) -> None:
    @supervisor.tool
    def list_specialists():
        """List the specialists you can delegate to and what each one is for."""
        return {name: prompt.split(".")[0] + "." for name, prompt in SPECIALISTS.items()}

    @supervisor.tool
    def ask(specialist: str, question: str):
        """Put one question to one specialist and return its answer.

        Each specialist remembers your earlier questions, so ask follow-ups rather than
        repeating context.
        """
        return team.ask(specialist, question)

    @supervisor.tool
    def ask_all(question: str):
        """Put the same question to every specialist at once and return all their answers."""
        names = list(SPECIALISTS)
        with ThreadPoolExecutor(max_workers=len(names)) as pool:
            answers = pool.map(lambda name: team.ask(name, question), names)
        return dict(zip(names, answers))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "task",
        nargs="?",
        default="explain how hax decides which provider to use, and check it against the docs",
    )
    parser.add_argument("--root", type=Path, default=REPO_ROOT,
                        help="what the librarian may read (default: this repository)")
    parser.add_argument("--provider", default="anthropic")
    parser.add_argument("--model", default=None, help="defaults to the provider's configuration")
    parser.add_argument("--deadline", type=float, default=None,
                        help="cancel the supervisor and every specialist after this many seconds")
    args = parser.parse_args()

    team = Team(args.provider, args.model, args.root)
    started = time.monotonic()
    try:
        with hax.Agent(
            provider=args.provider, model=args.model, system_prompt=SUPERVISOR_PROMPT
        ) as supervisor:
            register_delegation(supervisor, team)

            watchdog = None
            if args.deadline:
                def stop() -> None:
                    supervisor.cancel()
                    team.cancel_all()

                watchdog = threading.Timer(args.deadline, stop)
                watchdog.start()
            try:
                answer = supervisor.send(args.task)
            finally:
                if watchdog:
                    watchdog.cancel()

        print(answer)
        total = time.monotonic() - started
        delegated = sum(record.seconds for record in team.transcript)
        count = len(team.transcript)
        print(f"\n--- {count} delegation{'s' if count != 1 else ''}, "
              f"{delegated:.0f}s of {total:.0f}s ---")
        for record in team.transcript:
            print(f"  {record.specialist:<10} {record.seconds:6.1f}s  "
                  f"{record.tool_calls:3d} calls  {record.items:3d} items  "
                  f"{record.question.splitlines()[0][:44]}")
            print(f"  {'':<10} {'':>6}   -> {record.answer.splitlines()[0][:60]}")
    except hax.HaxCancelled:
        print(f"deadline reached after {args.deadline}s", file=sys.stderr)
        return 130
    except hax.HaxProviderError as exc:
        print(f"provider error: {exc}", file=sys.stderr)
        return 1
    except hax.HaxError as exc:
        print(f"hax: {exc}", file=sys.stderr)
        return 1
    finally:
        team.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
