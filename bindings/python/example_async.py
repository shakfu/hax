#!/usr/bin/env python3
"""Drive several hax agents concurrently from asyncio.

send() blocks but releases the GIL for the whole turn, so a thread executor is all asyncio needs
-- there is no separate async API to wait for. Each Agent owns its conversation and its own
cancellation, so the turns below overlap and stop independently.

    meson setup build-embed -Dembed=true && meson compile -C build-embed
    HAX_PROVIDER=mock HAX_MOCK_SCRIPT=scripts/mock/interrupt_stall.txt \
        uv run python bindings/python/example_async.py

Drop the mock variables and pass a real provider to talk to a live model.
"""

import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hax


async def send(agent: "hax.Agent", prompt: str) -> str:
    """Await one turn, mapping task cancellation onto that agent's cancel().

    A thread cannot be interrupted from outside, so cancelling the task is not enough on its own:
    shield the executor future, ask hax to stop, and let the turn unwind through HaxCancelled.
    Without the shield the future would be abandoned and its thread left running.
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


async def main() -> None:
    # Each agent is one conversation; the runtime underneath them is shared and refcounted.
    with hax.Agent(provider="mock") as first, \
         hax.Agent(provider="mock") as second, \
         hax.Agent(provider="mock") as third:

        # Three turns that each stall for two seconds. Serially that is six.
        started = time.monotonic()
        replies = await asyncio.gather(
            send(first, "one"), send(second, "two"), send(third, "three")
        )
        elapsed = time.monotonic() - started
        print(f"three turns in {elapsed:.1f}s (serially: ~6s)")
        for index, reply in enumerate(replies, start=1):
            print(f"  agent {index}: {reply.splitlines()[0]}")

    # Cancelling one task stops one agent; the other keeps going. Fresh agents, because the mock
    # script above is one turn long and each of those agents has now spent it.
    with hax.Agent(provider="mock") as target, hax.Agent(provider="mock") as bystander:
        stopped = asyncio.create_task(send(target, "this one gets cancelled"))
        survivor = asyncio.create_task(send(bystander, "this one finishes"))
        await asyncio.sleep(0.4)
        stopped.cancel()

        try:
            await stopped
            print("  target:    completed (expected a cancel)")
        except asyncio.CancelledError:
            print("  target:    cancelled")
        print(f"  bystander: {(await survivor).splitlines()[0]}")


if __name__ == "__main__":
    asyncio.run(main())
