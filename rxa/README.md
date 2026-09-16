# rxa

A coding agent harness with a frozen feature set.

`rxa` exists to test one claim: a usable agent harness fits in 3,000 lines when the ecosystem
carries the plumbing. The reference point is [hax](https://github.com/OleksandrChekhovskyi/hax),
50k lines of C, which reached that size in under five months. The difference is not the
language. It is that `rxa` writes down what it will refuse before writing code.

## Scope

Frozen. Each row is a ceiling, not a starting point.

| Dimension | Frozen at |
| --- | --- |
| Wire formats | 1: OpenAI-compatible Chat Completions |
| Providers | any endpoint speaking that format, selected by config |
| Tools | 4: `read`, `write`, `edit`, `bash` |
| Entry modes | 2: interactive REPL, headless `-p` |
| Config | flags, environment, and one cache file |
| Persistence | model list cache, prompt history |
| Test provider | 1: `mock`, replaying a JSON script |

One wire format still reaches many providers. OpenAI-compatible Chat Completions covers OpenAI,
OpenRouter, Groq, DeepSeek, llama.cpp, and Ollama. Provider selection is a base URL, an API key,
and a model id.

## Absent

Not "not yet". These are the refusals the line budget buys.

- Session save, resume, and replay
- Context compaction
- MCP, subagents, task queues
- Slash commands beyond `/quit`
- Model pickers, pricing tables, cost estimation
- Themes, images, paste handling, file mentions
- Streaming markdown rendering beyond code-block fencing
- OAuth and device-code login flows
- A plugin ABI or embedding API

## Non-negotiable

A harness missing any of these is a demo, not a tool. They are inside the budget, not exceptions
to it.

1. Cancel during streaming. `tokio::select!` over the response stream and key events.
2. Tool output cap. One `cat` of a build log must not consume the context window.
3. Retry on 429 and 5xx, with backoff and a visible indicator.
4. Context-limit detection. `rxa` reports tokens used and fails with a clear message. It does not
   compact.
5. Terminal restore on every exit path, including panic. A panic hook plus a `Drop` guard.

## The budget

3,000 lines in `src/`, excluding tests. `make budget` enforces it and `make check` includes it;
the `BUDGET` variable in the Makefile is the authoritative number.

A feature list ratchets, because adding one item to a list costs nothing. A line budget makes
every new feature compete with an existing one for the same 3,000 lines.

## Amending this file

The scope table is an objective, not a rule. A feature that makes `rxa` materially more usable is
worth more than an elegant tool nobody runs. Amendments are expected. They are also the only way
scope changes:

1. Edit the table or the absent list in the same commit as the code.
2. Say what the feature displaces, or raise the budget number explicitly.
3. Never grow `src/` past the budget and fix it later.

Two amendments are already anticipated and will arrive first: session resume, and a second wire
format. The model cache is where persistence starts, so resume will be cheap to add. That is the
argument for deciding it deliberately rather than discovering it.

Candidates that have been costed but not taken live in [TODO.md](TODO.md).

## Layout

1,799 lines across 18 files. Each module owns one boundary.

| Module | Holds |
| --- | --- |
| `agent.rs` | the continuation loop, retry policy, context check |
| `turn.rs` | stream events to one assistant message; no I/O, unit tested |
| `provider/` | the wire format, the network client, the scripted mock |
| `tools/` | the four tools, dispatched by enum rather than `dyn Tool` |
| `frontend/` | the `Frontend` trait and its two implementations |
| `cancel.rs` | the latched flag Esc and Ctrl-C both set |
| `term.rs` | raw mode, restored from `Drop` and from a panic hook |
| `config.rs`, `cache.rs` | flag and environment resolution, the model cache |
| `tests/` | unit tests sit in `src/`; `tests/` is only the network path |

## Build

```sh
make            # build
make check      # lint + test + budget; the full gate
make run        # one-shot against the mock provider
make repl       # interactive against the mock provider
make help       # every target
```

Plain `cargo build`, `cargo test` and `cargo clippy --all-targets -- -D warnings` work too; the
Makefile only adds the budget gate and the smoke targets.

`scripts/test_openrouter.sh`, `test_openai.sh` and `test_anthropic.sh` run four scenarios against
a real endpoint -- text, `read`, `bash`, then `write` with a read-back -- and exit non-zero if any
of them misses. They take the key from the provider's usual environment variable or from
`~/.config/rxa/<provider>.key`, and work from a throwaway sandbox rather than the repo, because
rxa's tools have no path jail. The Anthropic one uses that vendor's OpenAI-compatibility
endpoint; native `anthropic-messages` is the deferred amendment in [TODO.md](TODO.md).

Rust 1.88 or newer, for let-chains under edition 2024. `cargo test` also needs `python3`.

Unit tests live beside the module they exercise. `tests/live_path.rs` covers only what the
in-process mock cannot reach -- the request body rxa actually sends, and what it does against a
gateway that serves no model list -- by driving the built binary against
`tests/fixtures/fake_provider.py`. Behaviour that a unit test can already pin does not get a
second assertion there.

Run without a network or an API key by replaying a scripted stream:

```sh
rxa --mock mock/read-then-answer.json -p "what is this package?"
rxa --mock mock/say-hi.json            # interactive
```

A mock script is a JSON array of turns, each an array of steps: `{"text": ...}`,
`{"tool_call": {...}}`, `{"usage": {...}}`. One turn is consumed per provider round-trip.

Configuration, in precedence order: flags, then environment, then `$XDG_CONFIG_HOME/rxa/`,
which also holds the model cache and `history.txt`. rxa creates both owner-only, 0700 and 0600,
because prompts are written verbatim.

| Variable | Meaning |
| --- | --- |
| `RXA_BASE_URL` | API base, e.g. `https://api.openai.com/v1` |
| `RXA_API_KEY` | bearer token |
| `RXA_MODEL` | model id |
| `RUST_LOG` | `tracing` filter; `rxa=debug` logs requests with auth redacted |

## Name

`hx` is Helix's binary and `rx` is taken on crates.io, so the package and the binary are both
`rxa`.

## Licence

MIT.
