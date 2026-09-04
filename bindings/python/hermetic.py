"""Give an agent nothing but the tools its host handed it.

`Agent(system_prompt=...)` replaces only the base prompt. hax still appends its Environment
section, any AGENTS.md found from the working directory upward, the skills listing, and the
task and subagent guidance -- and it still advertises read, edit, write and bash, which the
binding can shadow but not unregister. A worker that is supposed to read one document can
otherwise reach the whole filesystem through bash, and its prompt changes with the directory
the script was run from.

confine() drops the ambient prompt for the whole process; seal() refuses the built-in tools on
one agent. Examples that hand an agent host-owned tools call both. The supervisor example does
not: reading the repository is what its librarian is for.
"""

from __future__ import annotations

import os
from typing import Any, Callable

# What hax offers every non-raw session. task_wait is absent because HAX_NO_TASKS withdraws it.
BUILTINS = ("read", "edit", "write", "bash")

REFUSAL = "error: no filesystem or shell access; use the tools you were given"


def confine() -> None:
    """Withdraw hax's ambient context from every agent built in this process afterwards.

    These are ordinary config keys, resolved from the environment each time hax reads them, so
    setting them before the first Agent is enough. There is no per-agent equivalent: the
    settings are process-wide.
    """
    os.environ.update(
        HAX_NO_ENV="1",
        HAX_NO_AGENTS_MD="1",
        HAX_NO_SKILLS="1",
        HAX_NO_SUBAGENTS="1",
        HAX_NO_TASKS="1",
    )


def _refusal(name: str) -> Callable[[], Any]:
    """A zero-argument stub that replaces one built-in's advertised definition.

    A **kwargs shadow would leave the built-in's own description standing, so the model would
    still be shown a working bash and would keep trying to use it. Declaring a signature
    replaces the definition, which costs a line instead of a paragraph and tells the truth.
    """

    def unavailable():
        """Not available to this agent, which has no filesystem or shell access."""
        return REFUSAL

    unavailable.__name__ = name
    return unavailable


def seal(agent, tools: tuple[str, ...] = BUILTINS) -> None:
    """Refuse hax's own tools on one agent, leaving only what the host registered."""
    for name in tools:
        agent.tool(_refusal(name))
