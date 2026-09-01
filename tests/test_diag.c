/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "harness.h"
#include "xalloc.h"
#include "system/fs.h"

static void test_diag_sequence(void)
{
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    EXPECT(saved >= 0);
    FILE *tmp = tmpfile();
    EXPECT(tmp != NULL);
    EXPECT(dup2(fileno(tmp), STDERR_FILENO) >= 0);

    unsigned long before = hax_diag_sequence();
    hax_warn("sequence test");
    EXPECT(hax_diag_sequence() == before + 1);

    EXPECT(dup2(saved, STDERR_FILENO) >= 0);
    close(saved);
    fclose(tmp);
}

struct diag_capture {
    enum hax_diag_level level;
    char *message;
    int calls;
};

static void capture_diag(enum hax_diag_level level, const char *message, void *user)
{
    struct diag_capture *capture = user;
    capture->level = level;
    free(capture->message);
    capture->message = xstrdup(message);
    capture->calls++;
}

static void test_diag_sink_receives_level_and_message(void)
{
    struct diag_capture capture = {0};
    hax_set_diag_sink(capture_diag, &capture);

    hax_err("broke at %d", 7);
    EXPECT(capture.calls == 1);
    EXPECT(capture.level == HAX_DIAG_ERR);
    EXPECT_STR_EQ(capture.message, "broke at 7");

    hax_warn("stale");
    EXPECT(capture.calls == 2);
    EXPECT(capture.level == HAX_DIAG_WARN);
    EXPECT_STR_EQ(capture.message, "stale");

    hax_set_diag_sink(NULL, NULL);
    free(capture.message);
}

static void test_diag_sink_message_has_no_prefix_or_newline(void)
{
    struct diag_capture capture = {0};
    hax_set_diag_sink(capture_diag, &capture);
    hax_err("plain");
    hax_set_diag_sink(NULL, NULL);

    EXPECT(capture.message != NULL);
    if (capture.message) {
        EXPECT(strncmp(capture.message, "hax: ", 5) != 0);
        EXPECT(strchr(capture.message, '\n') == NULL);
    }
    free(capture.message);
}

static void test_diag_sink_still_advances_sequence(void)
{
    struct diag_capture capture = {0};
    hax_set_diag_sink(capture_diag, &capture);
    unsigned long before = hax_diag_sequence();
    hax_warn("counted");
    unsigned long after = hax_diag_sequence();
    hax_set_diag_sink(NULL, NULL);

    EXPECT(after == before + 1);
    free(capture.message);
}

static void test_clearing_diag_sink_restores_stderr(void)
{
    struct diag_capture capture = {0};
    hax_set_diag_sink(capture_diag, &capture);
    hax_set_diag_sink(NULL, NULL);

    char *path = xasprintf("%s/stderr", t_tempdir());
    int saved = dup(STDERR_FILENO);
    FILE *redirected = freopen(path, "w", stderr);
    EXPECT(redirected != NULL);
    hax_err("goes to stderr");
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    clearerr(stderr);

    char *written = fs_read_file(path, NULL);
    EXPECT(capture.calls == 0);
    EXPECT(written != NULL);
    if (written)
        EXPECT_STR_EQ(written, "hax: goes to stderr\n");
    free(written);
    free(path);
    free(capture.message);
}

int main(void)
{
    test_diag_sequence();

    test_diag_sink_receives_level_and_message();
    test_diag_sink_message_has_no_prefix_or_newline();
    test_diag_sink_still_advances_sequence();
    test_clearing_diag_sink_restores_stderr();

    T_REPORT();
}
