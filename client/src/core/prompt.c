#include "prompt.h"
#include <string.h>

long prompt_encoded_max(long len)
{
    /* Worst case: every byte is a line break, each becoming "\\\r",
       plus a trailing space and CR. */
    return len * 2 + 4;
}

/* A last line the bridge would not treat as a plain prompt line: one ending
   in a backslash (it would wait for another line) or exactly a flow-control
   command (it would be swallowed). A trailing space avoids both, and the
   bridge strips it from the prompt. */
static int needs_guard(const char *line, long n)
{
    if (n > 0 && line[n - 1] == '\\')
        return 1;
    if (n == 4 && memcmp(line, "!ACK", 4) == 0)
        return 1;
    if (n == 5 && memcmp(line, "!STOP", 5) == 0)
        return 1;
    return 0;
}

long prompt_encode(const char *text, long len, char *out, long outcap, int *nlines)
{
    long o = 0, i = 0, start;
    int lines = 0;

    if (outcap < prompt_encoded_max(len))
        return -1;
    for (;;) {
        start = i;
        while (i < len && text[i] != '\r' && text[i] != '\n')
            i++;
        memcpy(out + o, text + start, i - start);
        o += i - start;
        lines++;
        if (i >= len) {
            if (needs_guard(text + start, i - start))
                out[o++] = ' ';
            out[o++] = '\r';
            break;
        }
        /* A line break: CR LF counts as one. */
        if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n')
            i++;
        i++;
        out[o++] = '\\';
        out[o++] = '\r';
    }
    if (nlines)
        *nlines = lines;
    return o;
}
