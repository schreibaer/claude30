#include "permsum.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MR_ELLIPSIS "\xC9"      /* Mac Roman ellipsis */
#define HEAD_MAX 255

static long width_of(PermsumMeasure m, const char *s, long n)
{
    return m ? m(s, n) : n;
}

static int is_word(const char *s, long n, const char *w)
{
    return n == (long)strlen(w) && memcmp(s, w, n) == 0;
}

static int is_file_tool(const char *t, long n)
{
    return is_word(t, n, "Edit") || is_word(t, n, "MultiEdit") || is_word(t, n, "Write")
        || is_word(t, n, "Read") || is_word(t, n, "NotebookEdit") || is_word(t, n, "NotebookRead");
}

/* Writes head + target + tail into out, cutting the target (with an
   ellipsis) until the whole line is no wider than max_width. */
static long compose(const char *head, const char *target, long tlen, const char *tail,
                    PermsumMeasure measure, long max_width, char *out, long cap)
{
    char line[PERMSUM_MAX + 8];
    long limit = cap - 1 < PERMSUM_MAX - 1 ? cap - 1 : PERMSUM_MAX - 1;
    long hl = (long)strlen(head), tl = (long)strlen(tail), k, n;
    if (hl > limit)
        hl = limit;
    for (k = tlen; ; k--) {
        int cut = k < tlen;
        int fits = hl + k + cut + tl <= limit;
        long t = tl;
        long kk = k < limit - hl ? k : limit - hl;
        memcpy(line, head, hl);
        n = hl;
        memcpy(line + n, target, kk);
        n += kk;
        if (cut && n < limit)
            line[n++] = MR_ELLIPSIS[0];
        if (t > limit - n)
            t = limit - n;
        memcpy(line + n, tail, t);
        n += t;
        if ((fits && width_of(measure, line, n) <= max_width) || k <= 0)
            break;
    }
    memcpy(out, line, n);
    out[n] = 0;
    return n;
}

/* "src/foo.py (40 lines)" -> "src/foo.py": the bridge adds the count to a
   Write header, and the headline gives its own. */
static long strip_line_count(const char *s, long n)
{
    long r, m;
    if (n < 2 || s[n - 1] != ')')
        return n;
    r = n - 2;
    while (r >= 0 && s[r] != '(')
        r--;
    if (r < 1 || s[r - 1] != ' ')
        return n;
    m = r + 1;
    while (m < n - 1 && s[m] >= '0' && s[m] <= '9')
        m++;
    if (m == r + 1 || n - 1 - m != 6 || memcmp(s + m, " lines", 6) != 0)
        return n;
    n = r - 1;
    while (n > 0 && s[n - 1] == ' ')
        n--;
    return n;
}

static long plain(const char *s, char *out, long cap)
{
    long n = (long)strlen(s);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    return n;
}

/* Added by the bridge to an Edit whose replace_all is set. */
#define ALL_MARK " (all occurrences)"

long permsum_build(const char *header, long hlen,
                   const char *body, const long *line_ends, long nlines,
                   int incomplete,
                   PermsumMeasure measure, long max_width,
                   char *out, long cap)
{
    char h[HEAD_MAX + 1], head[64], tail[64];
    const char *tool, *label = NULL, *target;
    long toollen, labellen = 0, tlen, i, start = 0;
    long added = 0, removed = 0, hunks = 0, p = 0;
    int every = 0;
    int bash, file_tool;

    if (cap < 2)
        return 0;
    if (incomplete)
        return plain("Incomplete request: deny it", out, cap);

    if (hlen < 0)
        hlen = 0;
    if (hlen > HEAD_MAX)
        hlen = HEAD_MAX;
    for (i = 0; i < hlen; i++)
        h[i] = (unsigned char)header[i] < 0x20 ? ' ' : header[i];
    h[hlen] = 0;

    while (p < hlen && h[p] == ' ')
        p++;
    tool = h + p;
    while (p < hlen && h[p] != ' ')
        p++;
    toollen = (h + p) - tool;
    if (toollen == 0)
        return plain("Permission request", out, cap);
    while (p < hlen && h[p] == ' ')
        p++;
    /* A subagent's type, "(general-purpose)": one word in parentheses. */
    if (p < hlen && h[p] == '(') {
        long q = p + 1;
        /* A real type is short and plain: letters, digits, "-", "_", ":".
           Anything else is the start of a command such as "(a;b) && ls". */
        while (q < hlen && q - p <= 20 && (isalnum((unsigned char)h[q]) || h[q] == '-'
                                           || h[q] == '_' || h[q] == ':'))
            q++;
        if (q > p + 1 && q < hlen && h[q] == ')' && (q + 1 == hlen || h[q + 1] == ' ')) {
            label = h + p;
            labellen = q + 1 - p;
            p = q + 1;
            while (p < hlen && h[p] == ' ')
                p++;
        }
    }
    target = h + p;
    tlen = hlen - p;
    while (tlen > 0 && target[tlen - 1] == ' ')
        tlen--;
    if (tlen == 0 && label && nlines == 0) {
        /* a command such as "(ls)" is not a label */
        target = label;
        tlen = labellen;
        label = NULL;
        labellen = 0;
    }

    bash = is_word(tool, toollen, "Bash");
    file_tool = is_file_tool(tool, toollen);

    for (i = 0; i < nlines; i++) {
        long end = line_ends[i];
        if (end > start) {
            char c = body[start];
            if (c == '+')
                added++;
            else if (c == '-')
                removed++;
            else if (c == '@' && end - start >= 2 && body[start + 1] == '@')
                hunks++;
        }
        start = end;
    }

    /* head: "Tool" or "Tool (type)" */
    snprintf(head, sizeof(head), "%.*s%s%.*s", (int)(toollen > 40 ? 40 : toollen), tool,
             label ? " " : "", (int)labellen, label ? label : "");

    if (bash) {
        const char *cmd = target;
        long clen = tlen;
        char buf[PERMSUM_MAX + 8];
        long n;
        if (nlines > 1) {
            snprintf(tail, sizeof(tail), ": %ld-line command, read it below", nlines);
            return compose(head, "", 0, tail, measure, max_width, out, cap);
        }
        if (nlines == 1) {
            cmd = body;
            clen = line_ends[0];
        }
        if (clen == 0)
            return compose(head, "", 0, "", measure, max_width, out, cap);
        /* The whole command or nothing: a cut command could hide the part
           that matters. */
        n = snprintf(buf, sizeof(buf), "%s: ", head);
        if (n + clen < (long)sizeof(buf) && n + clen < cap && n + clen < PERMSUM_MAX) {
            memcpy(buf + n, cmd, clen);
            buf[n + clen] = 0;
            if (width_of(measure, buf, n + clen) <= max_width) {
                memcpy(out, buf, n + clen + 1);
                return n + clen;
            }
        }
        snprintf(tail, sizeof(tail), ": long command (%ld characters), read it below", clen);
        return compose(head, "", 0, tail, measure, max_width, out, cap);
    }

    if (file_tool) {
        long k;
        /* The bridge adds this when Edit replaces every match in the file:
           the diff shows one match, so the count must not claim a place. */
        if (tlen > (long)sizeof(ALL_MARK) - 1
                && memcmp(target + tlen - (sizeof(ALL_MARK) - 1), ALL_MARK, sizeof(ALL_MARK) - 1) == 0) {
            every = 1;
            tlen -= (long)sizeof(ALL_MARK) - 1;
        }
        if (is_word(tool, toollen, "Write"))
            tlen = strip_line_count(target, tlen);
        for (k = tlen; k > 0; k--)
            if (target[k - 1] == '/')
                break;
        if (k < tlen) {
            target += k;
            tlen -= k;
        }
    }

    tail[0] = 0;
    if (nlines > 0) {
        if (is_word(tool, toollen, "Edit") || is_word(tool, toollen, "MultiEdit")) {
            long n = snprintf(tail, sizeof(tail), ": +%ld -%ld lines", added, removed);
            if (every && hunks > 0)
                snprintf(tail + n, sizeof(tail) - n, " at every match");
            else if (hunks > 0)
                snprintf(tail + n, sizeof(tail) - n, " in %ld place%s", hunks, hunks == 1 ? "" : "s");
        } else if (is_word(tool, toollen, "Write")) {
            snprintf(tail, sizeof(tail), ": +%ld lines", added);
        } else {
            snprintf(tail, sizeof(tail), ": %ld lines below", nlines);
        }
    }
    {
        char hd[80];
        snprintf(hd, sizeof(hd), "%s%s", head, tlen > 0 ? " " : "");
        return compose(hd, target, tlen, tail, measure, max_width, out, cap);
    }
}
