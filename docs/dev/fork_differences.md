# Fork differences

Deliberate source differences from upstream hax. Each entry says what differs, why, and when
it can be dropped. Check this file when merging upstream.

## `utf8_encode_codepoint` (upstream: `utf8_encode`)

`src/text/utf8.{c,h}`, `src/text/frontmatter.c`, `tests/text/test_utf8.c`.

jansson 2.14.1 defines a global `int utf8_encode(int32_t, char *, size_t *)` in `src/utf.c`.
Upstream's `size_t utf8_encode(uint32_t, char[4])` has the same name:

- Static jansson (`make wheel`, `scripts/build_static.sh`): the link fails with
  `multiple definition of 'utf8_encode'`.
- Shared jansson from `subprojects/jansson.wrap`: the build exports every symbol, so hax's
  definition replaces jansson's. jansson reads the returned length as an error, and
  `json_loads` aborts at `load.c:417` on any `\uXXXX` escape.

Packaged shared jansson probably exports only `json_*` and `jansson_*`, so upstream's usual
builds do not show the clash. Hiding symbols in the wrap would fix only the shared case.

Drop this when upstream renames the function. On an upstream merge, rename any new
`utf8_encode(` call sites.
