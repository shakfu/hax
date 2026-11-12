/* SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "harness.h"
#include "xalloc.h"
#include "system/clock.h"
#include "system/fs.h"
#include "terminal/herdr.h"

#define MAX_RECORDS 16

/* Recordings from the fake herdr binary: one file per call, one argument per line. */
struct records {
    char *text[MAX_RECORDS];
    unsigned long long seq[MAX_RECORDS];
    int n;
};

static char *g_dir;

/* Each call publishes its argv by rename, so a reader never sees a partial file. Absolute command
 * paths, since printf is not a builtin in every /bin/sh. */
static void write_fake_herdr(const char *path)
{
    FILE *script = fopen(path, "w");
    EXPECT(script != NULL);
    if (!script)
        return;
    fprintf(script,
            "#!/bin/sh\n/usr/bin/printf '%%s\\n' \"$@\" > '%s/tmp.'$$ && /bin/mv '%s/tmp.'$$ "
            "'%s/rec.'$$\n",
            g_dir, g_dir, g_dir);
    fclose(script);
    EXPECT(chmod(path, 0755) == 0);
}

static void enter_herdr(void)
{
    char *bin = xasprintf("%s/herdr", g_dir);
    setenv("HERDR_ENV", "1", 1);
    setenv("HERDR_PANE_ID", "w1:p1", 1);
    setenv("HERDR_BIN_PATH", bin, 1);
    setenv("HERDR_SOCKET_PATH", "/nonexistent/herdr.sock", 1);
    free(bin);
}

static unsigned long long seq_of(const char *text)
{
    const char *line = strstr(text, "\n--seq\n");
    return line ? strtoull(line + 7, NULL, 10) : 0;
}

static void read_records(struct records *out)
{
    out->n = 0;
    DIR *dir = opendir(g_dir);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)) && out->n < MAX_RECORDS) {
        if (strncmp(entry->d_name, "rec.", 4) != 0)
            continue;
        char *path = xasprintf("%s/%s", g_dir, entry->d_name);
        char *text = fs_read_file(path, NULL);
        free(path);
        if (!text)
            continue;
        int i = out->n++;
        /* Insertion sort by seq, which herdr_report assigns in call order. */
        unsigned long long seq = seq_of(text);
        while (i > 0 && out->seq[i - 1] > seq) {
            out->text[i] = out->text[i - 1];
            out->seq[i] = out->seq[i - 1];
            i--;
        }
        out->text[i] = text;
        out->seq[i] = seq;
    }
    closedir(dir);
}

static void free_records(struct records *records)
{
    for (int i = 0; i < records->n; i++)
        free(records->text[i]);
    records->n = 0;
}

/* Wait for `n` recordings. Bounded on the clock: detached spawns slow down under a parallel suite,
 * and 20s stays under meson's 30s timeout. */
static void wait_records(int n, struct records *out)
{
    for (long start = monotonic_ms();;) {
        read_records(out);
        if (out->n >= n || monotonic_ms() - start > 20000)
            break;
        free_records(out);
        struct timespec pause = {.tv_nsec = 10 * 1000 * 1000};
        nanosleep(&pause, NULL);
    }
    EXPECT(out->n == n);
}

/* The arguments after the seq value, which varies per run. The fake sees argv from argv[1]. */
static const char *after_seq(const char *text)
{
    const char *line = strstr(text, "\n--seq\n");
    if (!line)
        return "";
    const char *value_end = strchr(line + 7, '\n');
    return value_end ? value_end + 1 : "";
}

static void expect_record(const struct records *records, int i, const char *head, const char *tail)
{
    if (i >= records->n)
        return;
    size_t head_len = strlen(head);
    EXPECT(strncmp(records->text[i], head, head_len) == 0);
    EXPECT(strncmp(records->text[i] + head_len, "--seq\n", 6) == 0);
    EXPECT_STR_EQ(after_seq(records->text[i]), tail);
}

static void test_reports_lifecycle(void)
{
    g_dir = t_tempdir();
    char *bin = xasprintf("%s/herdr", g_dir);
    write_fake_herdr(bin);
    free(bin);

    /* Outside herdr every call is a no-op: HERDR_ENV unset, then set but incomplete. */
    herdr_report(HERDR_WORKING, NULL);
    setenv("HERDR_ENV", "1", 1);
    herdr_report(HERDR_WORKING, NULL);
    herdr_release();

    enter_herdr();
    herdr_report(HERDR_IDLE, NULL);
    herdr_report(HERDR_IDLE, NULL); /* unchanged: not re-sent */
    herdr_report(HERDR_WORKING, NULL);
    herdr_report(HERDR_IDLE, "20261105T120000-abc");
    herdr_report(HERDR_IDLE, "it's"); /* herdr would reject the resume command */
    herdr_release();
    herdr_release(); /* nothing held: not re-sent */

    struct records records;
    wait_records(5, &records);
#define REPORT "pane\nreport-agent\nw1:p1\n--source\nhax\n--agent\nhax\n--state\n"
    expect_record(&records, 0, REPORT "idle\n", "");
    expect_record(&records, 1, REPORT "working\n", "");
    expect_record(&records, 2, REPORT "idle\n",
                  "--agent-session-id\n20261105T120000-abc\n--\nhax\n"
                  "--resume=20261105T120000-abc\n");
    expect_record(&records, 3, REPORT "idle\n", "");
    expect_record(&records, 4, "pane\nrelease-agent\nw1:p1\n--source\nhax\n--agent\nhax\n", "");
#undef REPORT
    for (int i = 1; i < records.n; i++)
        EXPECT(records.seq[i] > records.seq[i - 1]);
    free_records(&records);
}

int main(void)
{
    test_reports_lifecycle();
    T_REPORT();
}
