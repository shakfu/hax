/* SPDX-License-Identifier: MIT */
#ifndef HAX_SYSTEM_BROWSER_H
#define HAX_SYSTEM_BROWSER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Best-effort: hand `url` to the platform's URL opener (open/xdg-open) with stdio silenced,
 * detached so a slow handoff never blocks or outlives the caller's interest. Success only means
 * the opener was spawned — callers must keep showing the URL itself. Never invokes a shell. */
void browser_open_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* HAX_SYSTEM_BROWSER_H */
