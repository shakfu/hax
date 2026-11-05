/* SPDX-License-Identifier: MIT */
#include "text/trim.h"

#include <string.h>

/* ASCII only: under a UTF-8 locale, isspace() may match a byte inside a multibyte sequence. */
static int is_space(char c)
{
    return c && strchr(" \t\n\v\f\r", c);
}

size_t trim_prompt(char *text)
{
    size_t end = strlen(text);
    while (end > 0 && is_space(text[end - 1]))
        end--;

    size_t start = 0;
    for (size_t i = 0; i < end && is_space(text[i]); i++) {
        if (text[i] == '\n')
            start = i + 1;
    }

    size_t len = end - start;
    memmove(text, text + start, len);
    text[len] = '\0';
    return len;
}
