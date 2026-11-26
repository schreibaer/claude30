#include "batch.h"
#include <stdlib.h>
#include <string.h>

/* Mac Roman bytes used as markers. */
#define MR_BULLET "\xA5"   /* bullet */
#define MR_EMDASH "\xD1"   /* em dash */

void batch_init(Batch *b)
{
    memset(b, 0, sizeof(*b));
}

void batch_free(Batch *b)
{
    free(b->text);
    free(b->runs);
    batch_init(b);
}

void batch_clear(Batch *b)
{
    b->len = 0;
    b->nruns = 0;
    b->oom = 0;
}

static int grow_text(Batch *b, long extra)
{
    if (b->len + extra > b->cap) {
        long ncap = b->cap ? b->cap : 1024;
        char *nt;
        while (ncap < b->len + extra)
            ncap *= 2;
        nt = (char *)realloc(b->text, ncap);
        if (!nt) {
            b->oom = 1;
            return 0;
        }
        b->text = nt;
        b->cap = ncap;
    }
    return 1;
}

static int new_run(Batch *b, RunStyle s)
{
    if (b->nruns >= b->rcap) {
        int ncap = b->rcap ? b->rcap * 2 : 32;
        Run *nr = (Run *)realloc(b->runs, ncap * sizeof(Run));
        if (!nr) {
            b->oom = 1;
            return 0;
        }
        b->runs = nr;
        b->rcap = ncap;
    }
    b->runs[b->nruns].style = s;
    b->runs[b->nruns].start = b->len;
    b->runs[b->nruns].len = 0;
    b->nruns++;
    return 1;
}

static void put_byte(Batch *b, RunStyle s, char c)
{
    Run *r = b->nruns ? &b->runs[b->nruns - 1] : NULL;
    if (!r || r->style != s || r->len >= BATCH_MAX_RUN) {
        if (!new_run(b, s))
            return;
        r = &b->runs[b->nruns - 1];
    }
    if (!grow_text(b, 1))
        return;
    b->text[b->len++] = c;
    r->len++;
}

/* Appends text in a style, turning LF into CR and breaking over-long lines. */
static void put(Batch *b, RunStyle s, const char *t, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        char c = t[i];
        if (c == '\n')
            c = '\r';
        if (c == '\r') {
            put_byte(b, s, '\r');
            b->line_len = 0;
            continue;
        }
        if (b->line_len >= BATCH_MAX_LINE) {
            put_byte(b, s, '\r');
            b->line_len = 0;
        }
        put_byte(b, s, c);
        b->line_len++;
    }
}

static void puts_(Batch *b, RunStyle s, const char *t)
{
    put(b, s, t, (long)strlen(t));
}

/* Writes v right-aligned in 3 columns and a space to out; returns the length. */
static long line_number(char *out, long v)
{
    char d[12];
    long n = 0, k = 0;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && n < 11);
    while (n + k < 3)
        out[k++] = ' ';
    while (n > 0)
        out[k++] = d[--n];
    out[k++] = ' ';
    return k;
}

/* Starts a new block (paragraph, code line, marker) on its own line. */
static void block(Batch *b, RunStyle s)
{
    if (b->need_break)
        put(b, s, "\r", 1);
    b->need_break = 1;
    b->prose_open = 0;
}

void batch_event(Batch *b, EventKind kind, const char *t, long n)
{
    switch (kind) {
    case EV_TEXT:
        block(b, RS_PROSE);
        put(b, RS_PROSE, t, n);
        b->prose_open = 1;
        break;
    case EV_TEXT_CONT:
        /* A streamed paragraph arrives over seconds, so a line the app drew
           itself (line noise, lost output) can come between two pieces. */
        if (b->prose_open)
            put(b, RS_PROSE, " ", 1);
        else
            block(b, RS_PROSE);
        put(b, RS_PROSE, t, n);
        b->prose_open = 1;
        break;
    case EV_CODE_BEGIN:
    case EV_CODE_END:
        b->code_line = 0;
        break;
    case EV_CODE: {
        char num[16];
        block(b, RS_CODE);
        put(b, RS_CODE, num, line_number(num, ++b->code_line));
        put(b, RS_CODE, t, n);
        break;
    }
    case EV_CODE_CONT:
        put(b, RS_CODE, t, n);
        break;
    case EV_TOOL:
        block(b, RS_TOOL);
        puts_(b, RS_TOOL, MR_BULLET " ");
        put(b, RS_TOOL, t, n);
        break;
    case EV_INFO:
        block(b, RS_INFO);
        puts_(b, RS_INFO, "i ");
        put(b, RS_INFO, t, n);
        break;
    case EV_ERROR:
        block(b, RS_ERROR);
        puts_(b, RS_ERROR, "Error: ");
        put(b, RS_ERROR, t, n);
        break;
    case EV_RESULT:
        block(b, RS_RESULT);
        puts_(b, RS_RESULT, MR_EMDASH " ");
        put(b, RS_RESULT, t, n);
        break;
    case EV_INTERRUPTED:
        batch_marker(b, RS_INTERRUPTED, MR_EMDASH " Interrupted");
        break;
    case EV_PERM_BEGIN:
        block(b, RS_TOOL);
        puts_(b, RS_TOOL, "? ");
        put(b, RS_TOOL, t, n);
        break;
    case EV_PERM_ABANDONED:
        batch_marker(b, RS_ERROR, "Permission request abandoned: the link was lost.");
        break;
    default:
        break;     /* state changes and dialog events draw nothing here */
    }
}

void batch_prompt(Batch *b, const char *t, long n)
{
    block(b, RS_PROMPT);
    puts_(b, RS_PROMPT, "> ");
    put(b, RS_PROMPT, t, n);
}

void batch_marker(Batch *b, RunStyle s, const char *t)
{
    block(b, s);
    puts_(b, s, t);
}

/* Position just after the first line break at or after pos, or len. */
static long next_line(const char *text, long len, long pos)
{
    while (pos < len && text[pos] != '\r')
        pos++;
    return pos < len ? pos + 1 : len;
}

long batch_trim_cut(const char *text, long len, long display_lines, long incoming)
{
    long lines = display_lines, cut = 0, i;
    if (lines <= 0) {
        lines = 1;
        for (i = 0; i < len; i++)
            if (text[i] == '\r')
                lines++;
    }
    if (len + incoming <= TRIM_AT_BYTES && lines <= TRIM_AT_LINES)
        return 0;
    if (len + incoming > TRIM_KEEP_BYTES)
        cut = len + incoming - TRIM_KEEP_BYTES;
    if (lines > TRIM_KEEP_LINES) {
        /* Cut a share of the text proportional to the surplus lines. */
        long by_lines = len * (lines - TRIM_KEEP_LINES) / lines;
        if (by_lines > cut)
            cut = by_lines;
    }
    return next_line(text, len, cut);
}
