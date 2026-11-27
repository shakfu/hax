# Host-defined tools over ACP

Decision record. Goal: an ACP client (here a C/C++ host) advertises tools the model can call,
and answers those calls itself, without linking `libhax`. User-facing behavior is documented
under "ACP mode" in `docs/usage.md`; `examples/acp_host.cpp` is a minimal C++ host.

## Why an extension

ACP has no tool-registration method. Its standard route is `session/new.mcpServers`, which needs
an MCP client in hax; `note_mcp_servers()` in `src/acp.c` currently drops them with a warning.
ACP reserves `_`-prefixed methods and `_meta` fields for extensions, so this sketch uses those.

The wire shapes copy MCP's `Tool` and `CallToolResult`. A later MCP client (stdio servers named
in `mcpServers`) can then reuse the same schema intake and result parsing.

## Protocol

### Capability

`initialize` result gains:

```json
{"agentCapabilities": {"_meta": {"hax": {"hostTools": true}}}}
```

A client that does not see it must not send host tools.

### Advertisement

`session/new` and `session/load` params accept:

```json
{"_meta": {"hax": {"tools": [
  {"name": "lookup_order",
   "description": "Return the contents of an order.",
   "inputSchema": {"type": "object",
                   "properties": {"order_id": {"type": "string"}},
                   "required": ["order_id"]}}
]}}}
```

- `inputSchema` must be a JSON object with `"type": "object"`. Otherwise the request fails with
  `INVALID_PARAMS`, naming the tool.
- Tools last for the session. Session files do not store them, so the client resends them on
  `session/load`.
- A name equal to a built-in (`read`, `edit`, `write`, `bash`, `task_wait`) replaces it, both
  advertisement and dispatch. This lets a host route `bash` to its own sandbox. It also drops
  the `bash` permission prompt, since the host decides whether its tools run.
- A session already open keeps its original tools when `session/load` names it again.
- Names must match `[A-Za-z0-9_-]{1,64}`, which provider APIs require.

### Call

When the model calls a host tool, hax sends a request to the client:

```json
{"id": 7, "method": "_hax/tool_call",
 "params": {"sessionId": "...", "toolCallId": "toolu_...", "name": "lookup_order",
            "arguments": {"order_id": "4417"}}}
```

The client answers:

```json
{"id": 7, "result": {"content": [{"type": "text", "text": "2x widget"}], "isError": false}}
```

| Client answer | Model sees | `origin` |
| --- | --- | --- |
| `result`, `isError` false or absent | concatenated `text` parts | `ITEM_ORIGIN_NONE` |
| `result`, `isError` true | `error: ` + text | `ITEM_ORIGIN_NONE` |
| JSON-RPC `error` | `error: ` + `error.message` | `ITEM_ORIGIN_NONE` |
| a line that is not JSON, or is over 16 MiB | `error: the client's answer ...` | `ITEM_ORIGIN_NONE` |
| the call's `id` with neither `result` nor `error` | `error: the client's answer has neither ...` | `ITEM_ORIGIN_NONE` |
| none within `acp.tool_timeout` | `error: the client did not answer within ...` | `ITEM_ORIGIN_NONE` |
| none; `session/cancel` arrives | `INTERRUPT_MARKER` | `ITEM_ORIGIN_SKIPPED` |
| none; input ends | `INTERRUPT_MARKER` | `ITEM_ORIGIN_SKIPPED` |

After a timeout or a cancel, hax sends the client a `_hax/tool_call_abandoned` notification with
`sessionId`, `toolCallId` and `reason` (`timeout` or `cancelled`). The client may still be running
a call the model now reads as failed, and the model may call it again. A notification lets the
client stop; the alternative, a deadline in the request params, would leave the client guessing
at a cancel. A call the client never received gets no notice, and end of input gets none, since
nobody is left to tell.

An unreadable line is taken as the waiting call's answer. Only one request is outstanding, so an
unrelated broken message is the only thing misattributed, and the call would otherwise wait out
the timeout and report the wrong cause. An answer with neither `result` nor `error` carries an
id, so it fails the call only when that id matches, and gets no `-32600` reply: JSON-RPC answers
no responses, and an error under the client's own id could be read as an answer to one of its
requests. Under any other id it is still refused as an invalid request.

Non-text content parts are ignored in v1. The existing `tool_call` and `tool_call_update` session
updates still go out, with `kind: "other"`. hax does not send `session/request_permission` for a
host tool: the host already decides whether the call runs.

A call the client has not answered within `acp.tool_timeout` (default `10m`) fails with an error
the model reads. The first version had no timeout. A client that hung without closing stdin then
froze the process, because the one dispatcher thread waits on the call, and every other request,
other sessions' included, queues behind it. A deadline bounds that wait; concurrent dispatch
would remove it but changes the server's threading model.

Response ids are matched by value, not type. Only one agent-to-client request is outstanding at a
time, so an answer carrying the same number as a string, or a null id on an error (JSON-RPC's
reply to a request it could not parse), can only answer that request. These are taken with a
warning. Any other id is dropped with a warning; a late answer to an abandoned request is dropped
quietly. Before this, any mismatch was dropped silently and the call waited forever.

The client's text is held to `tool_output_cap` by `output_cap_clip()`, with the same head and tail
split as the built-ins. It is applied where host text enters, not in `agent_tool_result_make()`:
the built-ins spend the whole budget on output and add their own markers, so a second cap there
would cut those.

## Output

Every message is queued for one writer thread, and only that thread writes stdout. Before, each
writer took `out_lock` and wrote with a blocking `fflush`. A client that stopped reading filled
the pipe, and the writer blocked holding the lock. If the reader thread then had to reply (a parse
error, or a `session/cancel` sent as a request), it waited on the lock and stopped reading stdin.
A client blocked writing to hax could then never finish, and neither side moved.

The dispatcher waits once about 1 MiB is queued (`ACP_OUTPUT_QUEUE_BYTES`). That is back-pressure
on a client that is not reading, and it bounds memory. The reader thread never waits, so it always
keeps reading stdin. Queueing without a bound was the alternative; it trades the pause for memory
that grows as long as the client is stuck. A message larger than the cap goes out alone. After a
failed write, output is discarded so that nothing waits on a client that is gone.

stderr is outside this. Warnings are written to it directly, so a client that captures stderr
and never reads it can still stop hax; `docs/usage.md` tells clients to read or inherit it.

## Schema passthrough

`struct tool_def` gained `schema_json`, a complete schema sent in place of one built from
`params`. Mapping `inputSchema` onto `struct tool_param` was the alternative. `tool_param` holds
only flat primitives and arrays of primitives, so nested objects, `enum`, and `oneOf` would be
rejected or silently dropped. Every adapter and the transcript build schemas through
`tool_schema_build()`, so the passthrough is one branch there.

`hax_abi` now reports `sizeof(struct tool_def)`, and `HAX_ABI_VERSION` is 3. A binding built
against the older layout would pass `agent_session_add_tool()` a def without the new field.

## Deferred

Decided 2026-11-27.

1. **Withholding built-ins.** A host can override a built-in but not remove it. Only
   `task_wait` can be withdrawn (`no_tasks`, process-wide). A per-session withhold list on
   `agent_session` would serve both this and the Python binding's `hermetic.py`.
2. **Subagents.** A subagent runs as a child `hax -p` process and cannot reach the ACP client, so
   it sees built-ins only.
3. **Images in results.** Non-text content parts are ignored. MCP `image` parts could map onto
   `tool_run_ctx.result_images`.
4. **MCP client.** `session/new.mcpServers` is still ignored. The tool and result shapes here
   copy MCP's, so an MCP client could reuse `acp_host_tools_parse()` and
   `acp_host_tool_output()`.
