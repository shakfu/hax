/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "xalloc.h"
#include "text/trim.h"

static void expect_trimmed(const char *prompt, const char *want)
{
    char *text = xstrdup(prompt);
    size_t len = trim_prompt(text);
    EXPECT_STR_EQ(text, want);
    EXPECT(len == strlen(want));
    free(text);
}

static void test_trailing_whitespace(void)
{
    expect_trimmed("hello", "hello");
    expect_trimmed("hello\n", "hello");
    expect_trimmed("hello \t\r\n\n  \n", "hello");
}

static void test_leading_blank_lines_keep_indentation(void)
{
    expect_trimmed("\n\n  def f():\n      pass\n", "  def f():\n      pass");
    expect_trimmed(" \t\r\n    - item", "    - item");
    expect_trimmed("  hello", "  hello");
}

static void test_interior_untouched(void)
{
    expect_trimmed("first\n\n\n  second", "first\n\n\n  second");
}

static void test_whitespace_only_becomes_empty(void)
{
    expect_trimmed("", "");
    expect_trimmed("\n", "");
    expect_trimmed(" \n\t\n ", "");
}

static void test_utf8_edges_survive(void)
{
    /* U+00E0 and U+0145 end in 0xA0 and 0x85, which Latin-1 classifies as spaces. */
    expect_trimmed("\xc3\xa0", "\xc3\xa0");
    expect_trimmed("\xc5\x85\n", "\xc5\x85");
}

int main(void)
{
    test_trailing_whitespace();
    test_leading_blank_lines_keep_indentation();
    test_interior_untouched();
    test_whitespace_only_becomes_empty();
    test_utf8_edges_survive();
    T_REPORT();
}
