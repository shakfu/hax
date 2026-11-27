/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOL_SCHEMA_H
#define HAX_TOOL_SCHEMA_H

#include <jansson.h>

#ifdef __cplusplus
extern "C" {
#endif

struct tool_def;

/* Build the JSON Schema object describing a tool's parameters. Returns a new reference the caller
 * owns; never NULL. A tool with no parameters yields a property-less object schema. A def's
 * `schema_json`, when it parses as an object, is returned as given. */
json_t *tool_schema_build(const struct tool_def *def);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TOOL_SCHEMA_H */
