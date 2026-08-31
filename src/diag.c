/* SPDX-License-Identifier: MIT */
#include "diag.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "terminal/ansi.h"
#include "terminal/theme.h"

/* Lets stdout presentation detect diagnostics emitted directly by lower layers. */
static _Atomic unsigned long diagnostic_sequence;

unsigned long hax_diag_sequence(void)
{
    return atomic_load_explicit(&diagnostic_sequence, memory_order_relaxed);
}

static hax_diag_fn diag_sink;
static void *diag_sink_user;

void hax_set_diag_sink(hax_diag_fn fn, void *user)
{
    diag_sink = fn;
    diag_sink_user = user;
}

static void emit_diagnostic(enum hax_diag_level level, const char *format, va_list args)
{
    if (diag_sink) {
        char *message = xvasprintf(format, args);
        if (message) {
            diag_sink(level, message, diag_sink_user);
            free(message);
        }
        atomic_fetch_add_explicit(&diagnostic_sequence, 1, memory_order_relaxed);
        return;
    }

    const char *color = theme_open(level == HAX_DIAG_WARN ? THEME_WARN : THEME_ERROR);
    int styled = isatty(fileno(stderr)) && *color;
    if (styled)
        fputs(color, stderr);
    fputs("hax: ", stderr);
    vfprintf(stderr, format, args);
    if (styled)
        fputs(ANSI_RESET, stderr);
    fputc('\n', stderr);
    fflush(stderr);
    atomic_fetch_add_explicit(&diagnostic_sequence, 1, memory_order_relaxed);
}

void hax_err(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(HAX_DIAG_ERR, format, args);
    va_end(args);
}

void hax_warn(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    emit_diagnostic(HAX_DIAG_WARN, format, args);
    va_end(args);
}
