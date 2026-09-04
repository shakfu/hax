# hax as a C++ library

An evaluation of a recurring proposal: drop the terminal frontend, rewrite the agent core in
modern C++ with coroutines, and expose it to Python through nanobind. The motivating complaint is
real — hax runs **one agent per process**, as [embedding.md](../embedding.md) states — so this
document separates that limitation from the rewrite that is usually proposed alongside it, and
records what the code actually says about both.

The conclusion, up front: the single-agent limitation is a global-state problem, not a language
problem, and the work to fix it is a prerequisite of every plan considered here. Coroutines and
nanobind are a separate, defensible, but much larger proposal that should be judged on its own
merits after the reentrancy work is done.

That conclusion is no longer a prediction. It was tested — see
[Measured: two agents in one process](#measured-two-agents-in-one-process). Two agents, then four,
already run concurrently in one process today with independent conversation state and no
cross-talk. The measurement also corrected this document's first inventory of blockers, which was
built from a grep that missed an entire class of global.

## What actually blocks more than one agent per process

`struct agent_session` is already a proper per-instance struct: conversation items, tools, model,
and effort are all instance-owned, and the extension seams (`provider.h`, `tool.h`) are already
narrow. The architecture is broadly right. What leaks is ambient process-wide state.

Auditing the mutable state gives a clean split. Note the method: an audit of *file-scope* statics
alone is not sufficient and produced a materially wrong list on the first pass. Sixteen additional
mutable statics are declared inside functions, and three of the races actually observed come from
those. Both forms have to be enumerated.

**Confirmed hazards, and what happened to them.** Sites marked "observed" produced a TSan report
under concurrent agents; "fixed" means the race gate in `tests/test_multi_agent.c` now runs clean
over them.

| Location | State | Status |
| --- | --- | --- |
| `tools/bash_env.c` | `selection_env`, `selection_count`, `assignment[64]` | **fixed** — was 8 race sites *and* a double-free crash |
| `system/keepawake.c` | `helper_pid`, `supported` | **fixed** — `helper_lock` serializes the handle |
| `tools/bash_process.c` | `shell_pgids` | **fixed** — no longer reached concurrently once the selection moved |
| `providers/provider_config.c` | `id[37]` | latent; returns a shared static, semantically shared |
| `src/config.c` | `store` | out of contract to touch off-thread; 74 core-path call sites |
| `tools/task_registry.c` | `tasks`, `next_task_number` | structural; not exercised |
| `providers/registry.c` | `defs`, `count`, `built` | structural lazy singleton |
| `src/hax_embed.c` | `initialized` | the refusal itself |

Two of these are not merely races but **correctness bugs under concurrency**, because they return a
pointer to a shared static buffer that a second thread can overwrite before the caller reads it:

- `tools/bash_env.c:84` `subagent_depth_assignment()` returns `static char assignment[64]`.
- `providers/provider_config.c:236` `provider_process_session_id()` returns `static char id[37]`.

**Corrections to this document's first pass.** Two entries were wrong:

- `system/cancel.c` was listed as a blocker. It is not a data race — the flags are already
  `atomic_int` and TSan reported nothing against them. It still needs *semantic* per-agent
  scoping (an abort must cancel one agent, not all), but that is a design change, not a fix.
- `tools/bash_process.c` `shell_pgids` was filed under "legitimately process-wide." Process-group
  signalling is indeed process-wide, but the table's *publish* path is unsynchronized and races.
  It needs a lock, not a split.

**Legitimately process-wide — leave alone:**

- libcurl global state, `system/locale.c`, `system/tempfiles.c`, `diag.c`.
- `transport/ca.c` — its lazy warn latches are already `atomic_int`.
- `catalog.c` memoization — a shared cache. It already carries `_Atomic g_cache_generation`; it
  needs a lock, not a split.
- `trace.c` — already serialized by `trace_mu`.

The smell is already visible in-tree: `config_snapshot_take()` / `config_snapshot_restore()` exist
and are used exactly once, in `select.c`, to save and restore ambient configuration around the
model picker. That is the shape of a global that does not reenter.

### The C path

Ordered by what the measurement showed actually matters, which is not the order this document
first proposed:

1. ~~**`tools/bash_env.c` and `tools/bash_process.c` first.**~~ **Done.** The selection moved onto
   `agent_session` and travels through `tool_run_ctx`; this also removed the double-free crash.
2. **Fix the remaining returned-static-buffer function.** `subagent_depth_assignment()` is done
   (a `pthread_once` fill of genuinely process-wide state). `provider_process_session_id()` still
   returns a shared `static char id[37]`, and is a latent bug on its own merits.
3. ~~**`system/keepawake.c`.**~~ **Done.** A mutex, not a refcount: the header documents acquire
   and release as idempotent, and refcounting would have changed that contract and its test.
4. **Config becomes a handle.** Introduce `config_str_in(cfg, key)` and make `config_str(key)` a
   thin wrapper over a default instance. This is the largest mechanical change but, per the
   measurement, the *least* urgent: the read path resolves at construction and did not race.
5. ~~**Cancellation gets per-agent scoping.**~~ **Done.** Never a race — the flags were already
   atomic — but an abort now cancels one agent without cancelling its siblings. `struct
   cancel_state` is selectable through `agent_loop_params` and `tool_run_ctx`, so a cancelled
   agent's running tool stops too; NULL keeps the process flags the terminal watcher writes.

Only the config item is large, and dropping the terminal roughly halves it:

| | config sites | cancel sites |
| --- | --- | --- |
| Whole tree | 152 | 60 |
| Core path only, frontend excluded | **74** | **28** |

The remainder is concentrated rather than smeared: `providers/http_provider.c` (23),
`agent_core.c` (7), `tools/bash.c` (6), `oneshot.c` (5), and `agent_env.c` (5) carry 46 of the 74.

One asymmetry falls out and is worth designing for explicitly: **only one agent can own the
terminal.** N agents per process means one interactive frontend plus N headless ones. That
constraint is identical in C and in C++.

## Measured: two agents in one process

The claim above was tested rather than argued. A 126-line spike links against `libhax` from
`meson setup build-embed -Dembed=true`, constructs multiple providers and `agent_session`s under a
single `hax_init()`, and runs them. It was run both normally and against a
`-Db_sanitize=thread` build.

**What works today, unmodified:**

| Scenario | Result |
| --- | --- |
| Two agents, sequential turns | both `AGENT_LOOP_COMPLETE`, independent item logs |
| Two agents, concurrent turns on two threads | both complete; repeated 5× without failure |
| Two agents, **divergent** config (different mock scripts) | each followed its own script |
| Two agents, concurrent `bash` tool dispatch | both got their own tool output, **no cross-talk** |
| Four agents, providers constructed **on worker threads** | all four completed |

So the "one agent per process" rule in [embedding.md](../embedding.md) overstates the situation.
`hax_init()` refuses a *second initialization*; nothing refuses a second `agent_session`, and the
conversation state genuinely does not interfere. The refusal is a conservative guard, not an
architectural limit. `turn.c` and `agent_loop.c` were confirmed by grep to read no globals at all,
and every core signature is fully instance-parameterized.

The per-agent config pattern already exists in-tree: `providers/mock.c:634` reads
`config_str("providers.mock.script")` at *construction* and copies it into the instance. Setting
the override between two constructions gave two agents two different scripts. That is the shape
the config-handle refactor should generalize, and it is why the config read path did not race.

**What broke, and the fix.** Landing the spike as `tests/test_multi_agent.c` immediately found
something the sanitizer-free spike had missed: the test **aborted in 11 of 30 runs** on a plain
build, with `free(): double free detected in tcache 2` in `bash_env_set_selection()` — reached
from `export_selection()` inside `agent_session_init()` itself. Two agents initializing sessions
both freed the same `selection_env[]` entries. `bash_env.h` had documented the assumption all
along ("Called only on the dispatch thread"); multi-agent simply violated it.

The selection is per-agent state, so it moved onto the session: `struct bash_env_selection` is
owned by `agent_session`, borrowed through a new `tool_run_ctx.env_selection`, and handed to
`bash_build_child_env()` by the bash tool. The subagent depth string stayed process-wide — how
deeply *this process* is nested is not an agent's property — but its lazy fill is now a
`pthread_once`. `keepawake` kept its idempotent acquire/release contract and took a mutex rather
than a refcount, so its documented semantics and its existing test are unchanged.

Results after the fix, all three build configurations at 121/121:

| Measure | Before | After |
| --- | --- | --- |
| Crash rate, plain build | 11 / 30 runs | 0 / 40 runs |
| Distinct TSan race locations | 11 | 0 |
| ASan + UBSan | not run | clean |

**One contract clarification came out of it.** The spike had constructed sessions on worker
threads, which raced `config_preset_names`. AGENTS.md is explicit that config and live provider
state are foreground state — "resolve config and prepare owned worker inputs before spawning
background work" — so concurrent *construction* asks for a guarantee the architecture does not
offer, and the test was corrected to build on the calling thread. Concurrent *running* of
already-built sessions is the supported shape, and that race disappeared with it. This is worth
knowing before any binding exposes an agent constructor to a thread pool.

**One semantic bug the race detector would not have caught:** all four agents reported the same
`provider_process_session_id()` — `04c4f224-…` — because it is a process-wide lazy singleton.
Provider affinity keys on that id, so N agents in one process would share affinity. Commit
`6b9954c` ("Key provider affinity by conversation") already moves in the right direction here.

The cost of this experiment is itself evidence for the recommendation at the end of this document:
one spike, a few hours including three sanitizer builds, and it produced a corrected blocker list,
a crash that no amount of reading would have found, and a fix — before any decision about C++ was
needed.

## What the rewrite would cost

Dropping the terminal removes roughly a third of the tree. The rest is ported.

| Area | LOC | Fate under the proposal |
| --- | --- | --- |
| `terminal/` | 6,010 | dropped |
| `render/` | 4,832 | dropped |
| Frontend top level (`agent.c`, `select.c`, `banner.c`, `busy.c`, `history.c`, `login.c`, `cli.c`, `paste_image.c`, `file_mention.c`, `main.c`) | 4,862 | dropped |
| **Presentation subtotal** | **~15,700 (32%)** | |
| `providers/` | 8,862 | ported |
| `tools/` | 5,179 | ported |
| `system/` | 2,183 | ported |
| `transport/` | 1,802 | ported |
| `text/` | 1,805 | ported |
| Core top level (`turn.c`, `agent_*.c`, `config.c`, `provider.c`, `session*.c`, `compact.c`, `catalog.c`, `model_meta.c`, `transcript.c`, `oneshot.c`, `hax_embed.c`) | 9,701 | ported |
| **Port subtotal** | **~29,500** | |

That is a smaller rewrite than porting hax wholesale. It is not a small one.

## Three things the rewrite does not solve

**Global state is still a prerequisite.** A coroutine that reads a `static config_store` is exactly
as broken as a thread that does. The `config.c`, `cancel.c`, and `task_registry.c` work above
happens in every plan. Coroutines do not buy reentrancy; they consume it.

**Python-side concurrency does not require C++-side coroutines.** N agents can be driven from
Python today with threads and futures — `system/bg_job` already exists. Coroutines are an argument
about *internal* composition: SSE parse to turn assembly to tool dispatch to continuation, where
`turn.c`'s hand-rolled state machine would become linear code. That argument is real and good. It
is not the multi-agent argument, and merging the two oversells the rewrite.

**The asyncio bridge is the actual hard part, and it is language-independent.** Either the C++
executor runs on its own thread and bridges to Python via `call_soon_threadsafe` and futures, or
the coroutines are driven *from* asyncio — which means asyncio owns the I/O, so libcurl-multi must
surrender its socket loop or expose descriptors through `curl_multi_socket_action`. That decision
determines whether the library feels native in Python, and it is faced identically in C.

## Bindings: nanobind against the existing binding

The dependency rule in [philosophy.md](../philosophy.md) is satisfied: `nanobind-dev` and
`python3-nanobind` are in Debian main (2.11.0-3), so nanobind is not disqualified the way a
pip-only or CMake-only dependency would be.

But a Python binding already exists — cffi in API mode, 746 lines in `bindings/python/hax/`, with
an `hax.Agent` context manager, an `@agent.tool` decorator, `send()`, and `items`. The trade is
narrower than it first appears:

- **nanobind wins** by retiring `hax_build.py`'s 262-line `cdef` mirror, and on native conversion
  of STL types and `std::expected`.
- **cffi wins** on the property its own docstring calls out: API mode compiles the declarations
  against the real headers, so a struct that gains a field fails the build rather than silently
  misreading memory at runtime. That safety would have to be rebuilt out of C++ types.

Neither is decisive. The binding layer is the cheap part of this plan, and it should not drive the
decision about the other 29,500 lines.

## Keep `tools/` as C

`tools/` is 5,179 lines and the least rewarding to port. `bash_process.c` alone carries process
group tracking, orphan reaping, `SIGTERM`-to-`SIGKILL` deadline escalation, and spool overflow
past the drain limit — edge-case-shaped code, expensive to re-derive, and RAII buys it close to
nothing. If the rewrite happens, `tools/` should stay C behind an `extern "C"` wall indefinitely.

## The positioning question

This proposal inverts the project's stated posture. [embedding.md](../embedding.md) opens with "hax
is a program first… Everything else is better served by a subprocess," and
[philosophy.md](../philosophy.md) rules out a plugin ABI. Dropping the terminal makes the library
the product.

That is a legitimate repositioning rather than a defect in the proposal, but it should be made
deliberately. What remains after the terminal is removed is a provider abstraction plus a tool
loop, which is the commoditized part of an agent. The 10,842 lines of markdown rendering, diff
coloring, pickers, and `vt_resolve` are arguably the differentiated part.

## Recommendation

Do the reentrancy work in C. It is required under every plan, it is five scoped fixes rather than a
rewrite, and it is testable incrementally against the existing suite. The measurement above
changes the sequencing: the concurrency work is smaller and more localized than first estimated,
and the config refactor — the item that looked like the main cost — turns out to be the least
urgent part of it.

A concrete first milestone, worth doing regardless of what is decided about C++: land the spike as
a real test. Registered under `e2e_scenarios` or as a `tests/` binary against
`-Db_sanitize=thread`, "two agents complete concurrently with no cross-talk and no races" becomes a
regression gate, and the eleven racing locations become a checklist that empties.

Then extend the **existing** cffi binding to multiple agents and put it under asyncio. That puts the
real product in front of you for a small fraction of the rewrite cost, and it answers the only
question that matters: is the multi-agent Python API the thing you actually want?

- If the answer is "the API is right, but the internal callback plumbing is unbearable," coroutines
  have earned their rewrite — and it would proceed with a working reference implementation and a
  passing test suite to check against.
- If the answer is "this is fine," roughly 30,000 lines were not written.

### The verdict

Every step above was taken, and the answer came back the same way each time.

The binding holds several agents, runs their turns concurrently, and cancels them independently.
Asyncio needed nothing: `send()` releases the GIL, so a thread executor overlaps three two-second
turns into two seconds, and task cancellation maps onto `Agent.cancel()` through a shielded
future. `bindings/python/example_async.py` is the whole story, and the suite runs it.

So the multi-agent case for the rewrite is closed, and it was answered entirely in C — five
scoped fixes, no new language, no coroutines. What the measurements kept showing is that the
limitation was never architectural: `agent_session` was already per-instance, `turn.c` and
`agent_loop.c` already read no globals, and every core signature was already
instance-parameterized. What stood in the way was a handful of process-wide variables and one
double-free, none of which a rewrite was needed to reach.

The internal-composition argument for coroutines survives on its own merits — `turn.c`'s state
machine and the SSE layer would read better as linear code — but it is now the *only* argument,
and it should be priced against ~29,500 ported lines rather than carried along by a multi-agent
case that no longer needs it. Nothing in this work suggested that composition is unmanageable:
the loop stayed untouched throughout.

What remains genuinely open is smaller than the document originally implied: config-as-handle (74
core call sites), which the measurement ranked least urgent and which nothing has yet demanded;
and per-agent semantics for anything else that is still process-wide by choice, such as the
keepawake inhibitor. Neither is a language question.

The one finding that would change this recommendation is a concrete internal composition problem
that survives the reentrancy work: if driving many concurrent turns through `turn.c` and the SSE
layer proves genuinely unmanageable as a callback-driven state machine, that is a coroutine
argument on its own merits, independent of multi-agent support or the choice of binding.
