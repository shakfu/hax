/* SPDX-License-Identifier: MIT */
#ifndef HAX_DIAG_H
#define HAX_DIAG_H

/* Emit one `hax: <message>` line to stderr, without changing control flow. Callers supply no prefix
 * or newline. hax_err uses the error color and hax_warn the warning color on terminals. */
__attribute__((format(printf, 1, 2))) void hax_err(const char *format, ...);
__attribute__((format(printf, 1, 2))) void hax_warn(const char *format, ...);

/* Monotonic count of completed hax_err() and hax_warn() writes. */
unsigned long hax_diag_sequence(void);

enum hax_diag_level {
    HAX_DIAG_ERR,
    HAX_DIAG_WARN,
};

/* Receive a formatted diagnostic instead of stderr: no `hax: ` prefix, no trailing newline, and
 * `message` borrowed for the call. Installed by an embedder that turns diagnostics into its own
 * errors; NULL restores the stderr default.
 *
 * hax_diag_sequence() still counts sink deliveries, so a caller can detect that something was
 * reported without inspecting the text. */
typedef void (*hax_diag_fn)(enum hax_diag_level level, const char *message, void *user);
void hax_set_diag_sink(hax_diag_fn fn, void *user);

#endif /* HAX_DIAG_H */
