#!/usr/bin/env python3
"""Put one prompt to several models at once, then have a further model rank the answers.

Every candidate is an Agent of its own, so provider and model are per agent rather than per
process: an Anthropic model, an OpenAI model and a local llama.cpp server answer the same
question in the same run, concurrently, and one failing candidate does not stop the others.

The judge sees the answers relabelled A, B, C and is never told which model wrote which. The
mapping is printed afterwards, next to what each answer cost in wall-clock time.

    meson setup build-embed -Dembed=true && meson compile -C build-embed

    export ANTHROPIC_API_KEY=... OPENAI_API_KEY=...
    uv run python bindings/python/example_judge.py \
        --candidate anthropic:claude-sonnet-5 --candidate openai:gpt-5.6 \
        --judge anthropic:claude-opus-5 "why does a compiler need a separate linker?"

A candidate is `provider` or `provider:model`; the provider's configured model is used when the
model is omitted. Repeat --candidate for as many as you want to compare.
"""

from __future__ import annotations

import argparse
import string
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax
from hermetic import confine, seal

JUDGE_PROMPT = (
    "You compare answers to one question. Judge only accuracy, completeness and concision; "
    "ignore length, tone and formatting. Name the best answer by its letter in the first line, "
    "then give one sentence per answer saying what it got right or wrong. You do not know who "
    "wrote any of them, and must not speculate."
)


@dataclass
class Answer:
    """One candidate's attempt: its resolved model, what it said, and what it cost."""

    label: str
    provider: str
    model: str
    text: str
    seconds: float
    error: str | None = None


def parse_candidate(spec: str) -> tuple[str, str | None]:
    provider, _, model = spec.partition(":")
    return provider, model or None


def ask(label: str, spec: str, question: str) -> Answer:
    """Run one candidate. A failed provider becomes a recorded error, not a raised one."""
    provider, model = parse_candidate(spec)
    started = time.monotonic()
    try:
        with hax.Agent(provider=provider, model=model) as agent:
            # Every candidate answers from the question alone. Left unsealed they would each
            # read whatever directory the comparison happened to run in, which is not a
            # difference between the models.
            seal(agent)
            # agent.model is what hax resolved, which is the interesting value when the spec
            # named no model and the provider's configuration chose one.
            resolved = agent.model
            text = agent.send(question)
        return Answer(label, provider, resolved, text, time.monotonic() - started)
    except hax.HaxError as exc:
        return Answer(label, provider, model or "?", "", time.monotonic() - started, str(exc))


def collect(candidates: list[str], question: str) -> list[Answer]:
    labels = string.ascii_uppercase
    with ThreadPoolExecutor(max_workers=len(candidates)) as pool:
        futures = [
            pool.submit(ask, labels[index], spec, question)
            for index, spec in enumerate(candidates)
        ]
        return [future.result() for future in futures]


def judge(question: str, answers: list[Answer], spec: str) -> str:
    provider, model = parse_candidate(spec)
    body = "\n\n".join(f"### Answer {a.label}\n{a.text}" for a in answers)
    with hax.Agent(provider=provider, model=model, system_prompt=JUDGE_PROMPT) as arbiter:
        seal(arbiter)
        return arbiter.send(f"Question: {question}\n\n{body}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("question", nargs="?",
                        default="why does a compiler need a separate linker?")
    parser.add_argument("--candidate", action="append", dest="candidates",
                        metavar="PROVIDER[:MODEL]", help="repeat for each model to compare")
    parser.add_argument("--judge", default=None, metavar="PROVIDER[:MODEL]",
                        help="who ranks the answers (default: the first candidate)")
    args = parser.parse_args()

    confine()
    candidates = args.candidates or ["anthropic", "openai"]
    if len(candidates) > len(string.ascii_uppercase):
        print("too many candidates to label", file=sys.stderr)
        return 1

    started = time.monotonic()
    answers = collect(candidates, args.question)
    elapsed = time.monotonic() - started

    for answer in answers:
        status = f"error: {answer.error}" if answer.error else f"{len(answer.text)} chars"
        identity = f"{answer.provider}/{answer.model}"
        print(f"  {answer.label}  {identity:<34} {answer.seconds:5.1f}s  {status}")
    print(f"{len(candidates)} candidates in {elapsed:.1f}s")

    usable = [answer for answer in answers if not answer.error and answer.text.strip()]
    if len(usable) < 2:
        print("\nfewer than two candidates answered; nothing to compare", file=sys.stderr)
        return 1

    print()
    try:
        print(judge(args.question, usable, args.judge or candidates[0]))
    except hax.HaxError as exc:
        print(f"judge failed: {exc}", file=sys.stderr)
        return 1

    print("\n--- who was who ---")
    for answer in usable:
        print(f"  {answer.label}  {answer.provider}/{answer.model}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
