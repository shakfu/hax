# Candidate amendments

Options, not a roadmap. Each one costs lines against the 3,000 in README.md, and nothing here is
committed to. Record the decision when one is taken or dropped.

## Second wire format: anthropic-messages

Status: deferred, not started, 2026-09-16.

Reaches Anthropic direct and anthropic-compatible endpoints, which Chat Completions cannot reach
at all. Prefer it over `openai-responses`, which buys only a vendor rxa already reaches.

Estimated 250-350 lines, plus 120-180 for the translation layer rxa does not currently have:
`Message` in `src/provider/mod.rs:54` is the Chat wire shape, serialized straight into the request
body. A second dialect has to decouple those.

### Verified against hax

Read from the fork's C implementation, not from memory.

- Body: `model`, `max_tokens` (required), `stream`, `messages` --
  `src/providers/anthropic_body.c:264`
- System prompt: top-level `system` as a `[{type:"text",...}]` array, not a message --
  `anthropic_body.c:270-277`
- Tools: flat `{name, description, input_schema}`, not nested under `function` --
  `anthropic_body.c:199-201`
- Tool call: a `tool_use` block inside assistant content -- `anthropic_body.c:60-69`
- Tool result: a `tool_result` block carrying `tool_use_id`, on a user message --
  `anthropic_body.c:116-137`
- SSE events: `message_start`, `content_block_start`, `content_block_delta`,
  `content_block_stop`, `message_delta`, `message_stop`, `error` --
  `anthropic_events.c:360-372`
- Deltas: `text_delta.text`, `input_json_delta.partial_json`, `thinking_delta`,
  `signature_delta` -- `anthropic_events.c:141-167`
- Usage: `input_tokens` and `output_tokens`. Cached input is reported in addition to
  `input_tokens`, not as a subset -- `anthropic_events.c:230-241`

Unlike Chat and Responses, the parser needs the SSE `event:` name: `wire.c:125` passes it through,
while `wire.c:41` and `wire.c:86` discard it. `eventsource-stream` already supplies it.

Model listing is paged with `after_id` cursors (`anthropic_models.c:117-190`). `src/cache.rs`
assumes the flat OpenAI list.

### Open decisions

- How the dialect is selected: an `--api` flag, or inferred from the base URL, or both.
- `max_tokens` is required by the wire and has no Chat equivalent. It needs a default and probably
  a flag.
- Thinking blocks and signatures must be replayed on the next request
  (`anthropic_events.c:177`). Carrying them means the internal message type grows a variant;
  skipping them means rxa cannot use extended thinking. Decide before writing the converter.

## Session resume

Status: deferred, not started, 2026-09-16.

The strongest pull on the freeze, and the cheapest to add, because `src/cache.rs` already carries
the parts that are fiddly: an XDG path, a schema version, an endpoint-keyed filename, and a
tmp-plus-rename atomic write. Persistence is already open; this widens what is stored, not
whether anything is.

Estimated 120-180 lines.

### Open decisions

- What a session is addressed by. `--continue` for the most recent and `--resume <id>` for a
  named one keeps it to two flags. A picker is in the README's absent list, so resume stays
  flag-driven or the absent list changes too.
- What gets written. The full `Vec<Message>` is the whole context, tool results included, so a
  session file is roughly the size of the conversation. Decide whether tool results are stored
  verbatim or re-truncated on load.
- Secrets. Tool output lands on disk verbatim: `bash` output can contain keys, tokens, and `env`
  dumps. At minimum the session directory is created 0700 and files 0600. Decide this before
  writing, not after.
- Pruning. Without a cap the directory grows without bound. A count or age limit is a few lines;
  an interactive pruner is not, and is absent by default.
- Schema drift. Reuse `cache.rs`'s version field and drop unreadable sessions rather than
  migrating them.

### What it displaces

The README scope table's `Persistence` row changes from "model list cache only". Update it in the
same commit, per the amendment rules.
