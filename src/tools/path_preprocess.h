/* SPDX-License-Identifier: MIT */
#ifndef HAX_TOOLS_PATH_PREPROCESS_H
#define HAX_TOOLS_PATH_PREPROCESS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Rewrite an absolute `path` under the process working directory as relative JSON arguments.
 * Returns allocated JSON, or NULL when the original arguments should be used. */
char *tool_relativize_path_args(const char *args_json);

#ifdef __cplusplus
}
#endif

#endif /* HAX_TOOLS_PATH_PREPROCESS_H */
