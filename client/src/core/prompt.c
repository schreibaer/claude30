#include "prompt.h"
#include <string.h>

long prompt_encoded_max(long len, long prefix_len)
{
    /* Worst case: every byte is a line break, so len + 1 lines, each with
       the prefix, a backslash or guard space, and CR. */
    return (len + 1) * (prefix_len + 2) + len + 1;
}

long prompt_encode(const char *text, long len, const char *prefix,
                   char *out, long outcap, int *nlines)
{
    long o = 0, i = 0, start, pl = (long)strlen(prefix);
    int lines = 0;

    if (outcap < prompt_encoded_max(len, pl))
        return -1;
    for (;;) {
        start = i;
        while (i < len && text[i] != '\r' && text[i] != '\n')
            i++;
        memcpy(out + o, prefix, pl);
        o += pl;
        memcpy(out + o, text + start, i - start);
        o += i - start;
        lines++;
        if (i >= len) {
            /* A last line ending in a backslash would make the bridge wait
               for another line; a trailing space avoids that, and the bridge
               strips it from the prompt. */
            if (i > start && text[i - 1] == '\\')
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
