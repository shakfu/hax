#!/usr/bin/env python3
"""Answer one question across a corpus by giving every document its own agent.

The map phase is one agent per document, all in flight at once; the reduce phase is a further
agent that merges what they found. A worker sees one document and nothing else: read_document
closes over its own text, hax's filesystem and shell tools are refused, and the ambient prompt
is withdrawn, so the run does not vary with the directory it was started from.

send() blocks but releases the GIL for the whole turn, so a thread pool is all the concurrency
this needs. The reported wall clock is the slowest worker, not the sum of them.

    meson setup build-embed -Dembed=true && meson compile -C build-embed

    export ANTHROPIC_API_KEY=...
    uv run python bindings/python/example_fanout.py --provider anthropic \
        --model claude-sonnet-5 "how does hax decide which provider to use?"

Any configured provider works. `--corpus DIR` points the run at documents of your own.
"""

from __future__ import annotations

import argparse
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax
from hermetic import confine, seal

REPO_ROOT = Path(__file__).resolve().parents[2]

# A worker sees one document and is told to say so when it has nothing, which keeps the reduce
# prompt free of nine paraphrases of "this document does not discuss that".
WORKER_PROMPT = (
    "You read exactly one document and report only what it says. Call read_document before "
    "answering. Keep the answer under four sentences and quote the wording that carries the "
    "claim. If the document does not address the question, reply with exactly: NOTHING."
)

# Each reader saw one document, so partial coverage is the normal case. Without the second
# sentence the reducer reads silence as dissent and writes a paragraph contrasting a document
# that discussed the question with one that merely did not.
REDUCER_PROMPT = (
    "You merge findings that several readers produced from separate documents. Each read one "
    "document and reported only what that document covers, so a report that omits something is "
    "silent about it, not in disagreement; report a conflict only where two reports make claims "
    "that cannot both be true. Answer from the reports alone, attribute each claim to the "
    "document it came from, and keep the whole answer under 250 words."
)

NOTHING = "NOTHING"


def conclusion(finding: str) -> str:
    """The worker's last line. send() joins every assistant message in the turn, so anything the
    model said before calling read_document is prefixed to the answer."""
    lines = [line for line in finding.strip().splitlines() if line.strip()]
    return lines[-1] if lines else ""


def survey(document: Path, question: str, provider: str, model: str | None) -> tuple[str, str]:
    """Run one worker over one document. Returns the document name and what it found."""
    text = document.read_text()

    with hax.Agent(provider=provider, model=model, system_prompt=WORKER_PROMPT) as worker:
        seal(worker)

        @worker.tool
        def read_document():
            """Return the full text of the document assigned to you."""
            return text

        return document.name, worker.send(f"Question: {question}")


def gather(documents: list[Path], question: str, provider: str,
           model: str | None) -> list[tuple[str, str]]:
    """Run every worker concurrently, keeping the corpus order in the results."""
    with ThreadPoolExecutor(max_workers=len(documents)) as pool:
        futures = [pool.submit(survey, doc, question, provider, model) for doc in documents]
        return [future.result() for future in futures]


def reduce_findings(question: str, findings: list[tuple[str, str]], provider: str,
                    model: str | None) -> str:
    report = "\n\n".join(f"## {name}\n{finding}" for name, finding in findings)
    with hax.Agent(provider=provider, model=model, system_prompt=REDUCER_PROMPT) as reducer:
        seal(reducer)
        return reducer.send(f"Question: {question}\n\nReports:\n\n{report}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("question", nargs="?",
                        default="how does hax decide which provider and model to use?")
    parser.add_argument("--corpus", type=Path, default=REPO_ROOT / "docs",
                        help="directory of documents to fan out over (default: the hax docs)")
    parser.add_argument("--glob", default="*.md", help="which files in the corpus to read")
    parser.add_argument("--provider", default="anthropic")
    parser.add_argument("--model", default=None, help="defaults to the provider's configuration")
    args = parser.parse_args()

    confine()
    documents = sorted(args.corpus.glob(args.glob))
    if not documents:
        print(f"no {args.glob} files under {args.corpus}", file=sys.stderr)
        return 1

    plural = "s" if len(documents) > 1 else ""
    print(f"{len(documents)} document{plural}, {len(documents)} agent{plural}")
    started = time.monotonic()
    try:
        findings = gather(documents, args.question, args.provider, args.model)
        mapped = time.monotonic() - started

        for name, finding in findings:
            print(f"  {name:<20} {conclusion(finding)[:70] or '(no answer)'}")
        print(f"map phase: {mapped:.1f}s across {len(documents)} concurrent turns")

        relevant = [(name, text) for name, text in findings if conclusion(text) != NOTHING]
        if not relevant:
            print("\nno document addressed the question")
            return 0

        print(f"\nreducing {len(relevant)} of {len(findings)} reports\n")
        print(reduce_findings(args.question, relevant, args.provider, args.model))
    except hax.HaxProviderError as exc:
        print(f"provider error: {exc}", file=sys.stderr)
        return 1
    except hax.HaxError as exc:
        print(f"hax: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
