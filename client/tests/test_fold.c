/* Folds, run against a plain string standing in for the transcript. */
#include "check.h"
#include "fold.h"

#define DOCMAX 200000

typedef struct {
    char t[DOCMAX];
    long n;
    FoldSet fs;
    FoldOps ops;
    long shifted_total;          /* what the replace hook told other trackers */
    long measure_calls;
} Doc;

static void d_read(void *c, long off, long len, char *out)
{
    Doc *d = (Doc *)c;
    memcpy(out, d->t + off, len);
}

static void d_replace(void *c, long off, long oldlen, const char *text, long tlen, int kind)
{
    Doc *d = (Doc *)c;
    (void)kind;
    memmove(d->t + off + tlen, d->t + off + oldlen, d->n - off - oldlen);
    memcpy(d->t + off, text, tlen);
    d->n += tlen - oldlen;
    d->shifted_total += tlen - oldlen;
}

static void doc_init(Doc *d, long width)
{
    memset(d, 0, sizeof(*d));
    fold_init(&d->fs);
    d->ops.ctx = d;
    d->ops.width = width;
    d->ops.read = d_read;
    d->ops.replace = d_replace;
    d->ops.measure = NULL;
}

/* One run, as window_append inserts it; fold false is the plain reference. */
static void add(Doc *d, int fold, RunStyle s, const char *text)
{
    long len = (long)strlen(text), base = d->n;
    memcpy(d->t + d->n, text, len);
    d->n += len;
    if (fold)
        fold_append(&d->fs, &d->ops, s, text, len, base);
}

static int same(const Doc *d, const char *want)
{
    return d->n == (long)strlen(want) && memcmp(d->t, want, d->n) == 0;
}

#define CHECK_DOC(d, want) do { \
    check_count++; \
    if (!same((d), (want))) { \
        check_failures++; \
        fprintf(stderr, "%s:%d: doc is \"%.*s\"\n  expected \"%s\"\n", __FILE__, __LINE__, \
                (int)((d)->n > 300 ? 300 : (d)->n), (d)->t, (want)); \
    } \
} while (0)

#define BULLET "\xA5 "

/* n code lines "  c<i>", one run each or all in one run. */
static void code_lines(Doc *d, int fold, int from, int to, int one_run)
{
    char line[64], run[8000];
    int i;
    run[0] = 0;
    for (i = from; i <= to; i++) {
        snprintf(line, sizeof(line), "\r  c%d", i);
        if (one_run)
            strcat(run, line);
        else
            add(d, fold, RS_CODE, line);
    }
    if (one_run)
        add(d, fold, RS_CODE, run);
}

static void test_code(void)
{
    Doc a, b;
    doc_init(&a, 0);
    doc_init(&b, 0);
    /* 12 lines stay as they are */
    add(&a, 1, RS_PROSE, "hello");
    code_lines(&a, 1, 1, 12, 0);
    add(&b, 0, RS_PROSE, "hello");
    code_lines(&b, 0, 1, 12, 0);
    CHECK(same(&a, b.t) || (a.n == b.n && memcmp(a.t, b.t, a.n) == 0));
    CHECK_EQ_LONG(a.fs.n, 0);
    /* the 13th line collapses all of them */
    code_lines(&a, 1, 13, 13, 0);
    CHECK_EQ_LONG(a.fs.n, 1);
    CHECK_DOC(&a, "hello\r[+] Code, 13 lines: c1");
    /* the count keeps climbing as the block streams in */
    code_lines(&a, 1, 14, 47, 0);
    CHECK_EQ_LONG(a.fs.n, 1);
    CHECK_DOC(&a, "hello\r[+] Code, 47 lines: c1");
    CHECK(a.fs.f[0].growing);
    /* prose ends the block */
    add(&a, 1, RS_PROSE, "\rdone");
    CHECK(!a.fs.f[0].growing);
    CHECK_DOC(&a, "hello\r[+] Code, 47 lines: c1\rdone");
    /* open: every line is back, in place */
    code_lines(&b, 0, 13, 47, 0);
    add(&b, 0, RS_PROSE, "\rdone");
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_OPENED);
    CHECK(a.n > 0 && memcmp(a.t, "hello\r[-] Code, 47 lines: c1\r  c1\r  c2", 36) == 0);
    {
        char want[4000];
        long k = 0;
        memcpy(want, "hello\r[-] Code, 47 lines: c1", 28);
        k = 28;
        memcpy(want + k, b.t + 5, b.n - 5);       /* the lines and "done" */
        want[k + b.n - 5] = 0;
        CHECK_DOC(&a, want);
    }
    /* closing again gives the marker back */
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_CLOSED);
    CHECK_DOC(&a, "hello\r[+] Code, 47 lines: c1\rdone");
}

static void test_code_one_run_and_continuation(void)
{
    Doc a, b;
    doc_init(&a, 0);
    doc_init(&b, 0);
    /* the whole block in one run, then a continuation of its last line */
    code_lines(&a, 1, 1, 20, 1);
    code_lines(&b, 0, 1, 20, 1);
    CHECK_DOC(&a, "\r[+] Code, 20 lines: c1");
    add(&a, 1, RS_CODE, "tail");      /* EV_CODE_CONT: no line break */
    add(&b, 0, RS_CODE, "tail");
    CHECK_DOC(&a, "\r[+] Code, 20 lines: c1");
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_BUSY);
    add(&a, 1, RS_PROSE, "\rx");
    add(&b, 0, RS_PROSE, "\rx");
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_OPENED);
    {
        char want[4000];
        memcpy(want, "\r[-] Code, 20 lines: c1", 23);
        memcpy(want + 23, b.t, b.n);
        want[23 + b.n] = 0;
        CHECK_DOC(&a, want);
    }
}

static void test_tools(void)
{
    Doc a;
    doc_init(&a, 0);
    add(&a, 1, RS_PROSE, "go");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read a.c");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read b.c");
    CHECK_EQ_LONG(a.fs.n, 0);
    add(&a, 1, RS_TOOL, "\r" BULLET "Grep x");
    CHECK_EQ_LONG(a.fs.n, 1);
    CHECK_DOC(&a, "go\r[+] 3 tools: Read 2, Grep");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read c.c");
    add(&a, 1, RS_TOOL, "\r" BULLET "Bash ls");
    CHECK_DOC(&a, "go\r[+] 5 tools: Read 3, Grep, Bash");
    add(&a, 1, RS_PROSE, "\rthen");
    /* a later run is a new one */
    add(&a, 1, RS_TOOL, "\r" BULLET "Glob *");
    add(&a, 1, RS_TOOL, "\r" BULLET "Glob *.h");
    CHECK_EQ_LONG(a.fs.n, 1);
    add(&a, 1, RS_PROSE, "\rmore");
    CHECK_EQ_LONG(a.fs.n, 1);
}

static void test_tool_runs_end(void)
{
    Doc a, b;
    /* a permission line ends the run, even in the same run of text */
    doc_init(&a, 0);
    doc_init(&b, 0);
    add(&a, 1, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b\r" BULLET "Read c\r? Write x\r" BULLET "Read d");
    add(&b, 0, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b\r" BULLET "Read c\r? Write x\r" BULLET "Read d");
    CHECK_EQ_LONG(a.fs.n, 1);
    CHECK_DOC(&a, "\r[+] 3 tools: Read 3\r? Write x\r" BULLET "Read d");
    CHECK_EQ_LONG(a.fs.f[0].growing, 0);
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_OPENED);
    {
        char want[400];
        memcpy(want, "\r[-] 3 tools: Read 3", 20);
        memcpy(want + 20, b.t, b.n);
        want[20 + b.n] = 0;
        CHECK_DOC(&a, want);
    }

    /* a subagent line, another style, ends the run */
    doc_init(&a, 0);
    add(&a, 1, RS_TOOL, "\r" BULLET "Read a");
    add(&a, 1, RS_AGENT, "\r  \xC8 Explore: find it");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read b");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read c");
    CHECK_EQ_LONG(a.fs.n, 0);
    /* the run is three lines when nothing is in between */
    doc_init(&a, 0);
    add(&a, 1, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b");
    add(&a, 1, RS_INFO, "\ri note");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read c");
    CHECK_EQ_LONG(a.fs.n, 0);
}

static void test_width(void)
{
    Doc a;
    doc_init(&a, 30);
    add(&a, 1, RS_CODE, "\r    this is a long first line of code that does not fit");
    code_lines(&a, 1, 2, 13, 0);
    CHECK_EQ_LONG(a.fs.n, 1);
    /* "[+] Code, 13 lines: " takes 20 of the 30; the first line is cut to fit */
    CHECK(a.fs.f[0].len <= 30);
    CHECK(memcmp(a.t + 1, "[+] Code, 13 lines: this", 24) == 0);
    CHECK(a.t[a.fs.f[0].off + a.fs.f[0].len - 1] == '\xC9');
    fold_free(&a.fs);
}

/* ---- the log and trimming ----------------------------------------------- */

typedef struct {
    char out[DOCMAX];
    long n;
    const Doc *d;
    int fail_at;                 /* fail the nth call; 0: never */
    int calls;
} Sink;

static int s_range(void *c, long off, long len)
{
    Sink *s = (Sink *)c;
    if (s->fail_at && ++s->calls == s->fail_at)
        return 7;
    memcpy(s->out + s->n, s->d->t + off, len);
    s->n += len;
    return 0;
}

static int s_bytes(void *c, const char *t, long len)
{
    Sink *s = (Sink *)c;
    if (s->fail_at && ++s->calls == s->fail_at)
        return 7;
    memcpy(s->out + s->n, t, len);
    s->n += len;
    return 0;
}

static void test_log_and_cut(void)
{
    Doc a, b;
    Sink s;
    FoldLog lg;
    long cut;
    doc_init(&a, 0);
    doc_init(&b, 0);
    add(&a, 1, RS_PROSE, "intro");
    add(&b, 0, RS_PROSE, "intro");
    code_lines(&a, 1, 1, 30, 0);
    code_lines(&b, 0, 1, 30, 0);
    add(&a, 1, RS_PROSE, "\rmiddle");
    add(&b, 0, RS_PROSE, "\rmiddle");
    add(&a, 1, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b\r" BULLET "Read c");
    add(&b, 0, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b\r" BULLET "Read c");
    add(&a, 1, RS_PROSE, "\rend");
    add(&b, 0, RS_PROSE, "\rend");
    CHECK_EQ_LONG(a.fs.n, 2);

    memset(&s, 0, sizeof(s));
    s.d = &a;
    lg.ctx = &s;
    lg.range = s_range;
    lg.bytes = s_bytes;
    /* cut after "middle\r": the log holds the intro and the full 30 lines */
    cut = (long)(strstr(a.t, "\r[+] 3 tools") - a.t) + 1;
    CHECK_EQ_LONG(fold_log(&a.fs, cut, &lg), 0);
    {
        long want = (long)(strstr(b.t, "\r" BULLET "Read a") - b.t) + 1;
        CHECK_EQ_LONG(s.n, want);
        CHECK(memcmp(s.out, b.t, want) == 0);
    }
    /* a cut through everything, tool fold included */
    s.n = 0;
    cut = a.n;
    CHECK_EQ_LONG(fold_log(&a.fs, cut, &lg), 0);
    CHECK_EQ_LONG(s.n, b.n);
    CHECK(memcmp(s.out, b.t, b.n) == 0);
    /* a failing write is returned and the folds are untouched */
    s.n = 0;
    s.fail_at = 2;
    s.calls = 0;
    CHECK_EQ_LONG(fold_log(&a.fs, cut, &lg), 7);
    CHECK_EQ_LONG(a.fs.n, 2);

    /* cut: the first fold goes, the second moves up */
    cut = (long)(strstr(a.t, "\rmiddle") - a.t) + 1;
    {
        long second = a.fs.f[1].off;
        fold_cut(&a.fs, cut);
        CHECK_EQ_LONG(a.fs.n, 1);
        CHECK_EQ_LONG(a.fs.f[0].off, second - cut);
        CHECK_EQ_LONG(a.fs.bytes, a.fs.f[0].body_len);
    }
}

static void test_open_fold_log_and_cut(void)
{
    Doc a;
    Sink s;
    FoldLog lg;
    doc_init(&a, 0);
    add(&a, 1, RS_PROSE, "a");
    code_lines(&a, 1, 1, 14, 0);
    add(&a, 1, RS_PROSE, "\rb");
    CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_OPENED);
    memset(&s, 0, sizeof(s));
    s.d = &a;
    lg.ctx = &s;
    lg.range = s_range;
    lg.bytes = s_bytes;
    CHECK_EQ_LONG(fold_log(&a.fs, a.n, &lg), 0);
    CHECK_EQ_LONG(s.n, a.n);                 /* text as it stands */
    CHECK(memcmp(s.out, a.t, a.n) == 0);
    /* a cut through the open lines drops the fold: its marker went */
    fold_cut(&a.fs, (long)(strstr(a.t, "\r  c5") - a.t) + 1);
    CHECK_EQ_LONG(a.fs.n, 0);
    CHECK_EQ_LONG(a.fs.bytes, 0);
}

static void test_shift(void)
{
    Doc a;
    doc_init(&a, 0);
    add(&a, 1, RS_PROSE, "x");
    code_lines(&a, 1, 1, 13, 0);
    add(&a, 1, RS_PROSE, "\ry");
    CHECK_EQ_LONG(a.fs.f[0].off, 2);
    fold_shift(&a.fs, 0, 3);              /* a line above grew by 3 */
    CHECK_EQ_LONG(a.fs.f[0].off, 5);
    fold_shift(&a.fs, 40, 3);             /* a line below changed */
    CHECK_EQ_LONG(a.fs.f[0].off, 5);
    CHECK_EQ_LONG(fold_marker_at(&a.fs, 5), 0);
    CHECK_EQ_LONG(fold_marker_at(&a.fs, 5 + a.fs.f[0].len), 0);
    CHECK_EQ_LONG(fold_marker_at(&a.fs, 4), -1);
    CHECK_EQ_LONG(fold_containing(&a.fs, 6), 0);
    CHECK_EQ_LONG(fold_last_in(&a.fs, 0, 100), 0);
    CHECK_EQ_LONG(fold_last_in(&a.fs, 6, 100), -1);
    CHECK_EQ_LONG(fold_by_id(&a.fs, a.fs.f[0].id), 0);
    CHECK_EQ_LONG(fold_by_id(&a.fs, 999), -1);
}

static void test_cut_while_growing(void)
{
    Doc a;
    doc_init(&a, 0);
    add(&a, 1, RS_PROSE, "intro");
    code_lines(&a, 1, 1, 20, 0);
    CHECK(a.fs.f[0].growing);
    /* old text above the block goes; the block's fold moves and keeps growing */
    fold_cut(&a.fs, 6);
    memmove(a.t, a.t + 6, a.n - 6);
    a.n -= 6;
    CHECK_EQ_LONG(a.fs.f[0].off, 0);
    CHECK_EQ_LONG(a.fs.start, 0);
    code_lines(&a, 1, 21, 25, 0);
    CHECK_DOC(&a, "[+] Code, 25 lines: c1");
    /* a cut through the marker drops the fold; the block is not folded again */
    fold_cut(&a.fs, 1);
    memmove(a.t, a.t + 1, a.n - 1);
    a.n -= 1;
    CHECK_EQ_LONG(a.fs.n, 0);
    CHECK_EQ_LONG(a.fs.kind, FK_NONE);
    code_lines(&a, 1, 26, 30, 0);
    CHECK_EQ_LONG(a.fs.n, 0);
    fold_free(&a.fs);
}

/* ---- limits ------------------------------------------------------------- */

static void test_body_cap(void)
{
    Doc a, b;
    int i;
    char line[100];
    doc_init(&a, 0);
    doc_init(&b, 0);
    /* a block whose lines pass the 12 KB a fold may keep: the fold stops
       growing and the rest of the block stays as plain lines */
    for (i = 1; i <= 400; i++) {
        snprintf(line, sizeof(line), "\r  %0*d", 50, i);
        add(&a, 1, RS_CODE, line);
        add(&b, 0, RS_CODE, line);
    }
    add(&a, 1, RS_PROSE, "\rend");
    add(&b, 0, RS_PROSE, "\rend");
    CHECK_EQ_LONG(a.fs.n, 1);
    CHECK(a.fs.f[0].body_len <= FOLD_MAX_BODY);
    CHECK(a.n < b.n);
    /* nothing lost: opening gives the plain transcript back, byte for byte */
    {
        CHECK_EQ_LONG(fold_toggle(&a.fs, &a.ops, 0), FT_OPENED);
        CHECK(memcmp(a.t, "\r[-] Code, ", 11) == 0);
        {
            char *p = strchr(a.t + 1, '\r');   /* the end of the marker line */
            long k = (long)(p - a.t);
            CHECK_EQ_LONG(a.n - k, b.n);
            CHECK(memcmp(a.t + k, b.t, b.n) == 0);
        }
    }
}

static void test_fold_count_cap(void)
{
    Doc a;
    int i;
    doc_init(&a, 0);
    for (i = 0; i < FOLD_MAX + 5; i++) {
        add(&a, 1, RS_TOOL, "\r" BULLET "Read a\r" BULLET "Read b\r" BULLET "Read c");
        add(&a, 1, RS_PROSE, "\rp");
    }
    CHECK_EQ_LONG(a.fs.n, FOLD_MAX);
    /* the ones past the cap stay plain tool lines */
    CHECK(strstr(a.t + a.n - 80, "Read c") != NULL);
    fold_free(&a.fs);
}

/* ---- the same text however the lines arrive ----------------------------- */

static unsigned long rng = 12345;
static int rnd(int n)
{
    rng = rng * 1103515245UL + 12345UL;
    return (int)((rng >> 16) % (unsigned long)n);
}

static void random_stream(unsigned long seed, long width)
{
    Doc a, b;
    int step;
    char *snapshot = (char *)malloc(DOCMAX);
    long snap_n;
    int i;
    rng = seed;
    doc_init(&a, width);
    doc_init(&b, 0);
    for (step = 0; step < 120; step++) {
        int kind = rnd(5), n, k;
        char text[6000];
        text[0] = 0;
        if (kind == 0) {
            snprintf(text, sizeof(text), "\rprose %d", step);
            add(&a, 1, RS_PROSE, text);
            add(&b, 0, RS_PROSE, text);
        } else if (kind == 1) {
            n = 1 + rnd(30);
            /* the lines of one block, in runs of 1 to 6 lines */
            for (k = 1; k <= n;) {
                int take = 1 + rnd(6), q;
                text[0] = 0;
                for (q = 0; q < take && k <= n; q++, k++) {
                    char line[40];
                    snprintf(line, sizeof(line), "\r  x%d_%d", step, k);
                    strcat(text, line);
                }
                add(&a, 1, RS_CODE, text);
                add(&b, 0, RS_CODE, text);
                if (rnd(5) == 0) {          /* a continuation of the last line */
                    add(&a, 1, RS_CODE, "+");
                    add(&b, 0, RS_CODE, "+");
                }
            }
        } else if (kind == 2) {
            n = 1 + rnd(7);
            for (k = 0; k < n;) {
                int take = 1 + rnd(3), q;
                text[0] = 0;
                for (q = 0; q < take && k < n; q++, k++) {
                    char line[40];
                    snprintf(line, sizeof(line), "\r" BULLET "%s f%d", (k % 3) ? "Read" : "Grep", k);
                    strcat(text, line);
                }
                add(&a, 1, RS_TOOL, text);
                add(&b, 0, RS_TOOL, text);
            }
        } else if (kind == 3) {
            snprintf(text, sizeof(text), "\r? Write w%d", step);
            add(&a, 1, RS_TOOL, text);
            add(&b, 0, RS_TOOL, text);
        } else {
            snprintf(text, sizeof(text), "\r  \xC8 agent %d", step);
            add(&a, 1, RS_AGENT, text);
            add(&b, 0, RS_AGENT, text);
        }
        if (rnd(7) == 0 && a.fs.n > 0) {
            /* open or close one at random, as the person would */
            int idx = rnd(a.fs.n);
            fold_toggle(&a.fs, &a.ops, idx);
        }
    }
    add(&a, 1, RS_PROSE, "\rend");
    add(&b, 0, RS_PROSE, "\rend");
    /* close everything, remember how it looks, then open everything */
    for (i = 0; i < a.fs.n; i++)
        if (a.fs.f[i].open)
            CHECK(fold_toggle(&a.fs, &a.ops, i) == FT_CLOSED);
    snap_n = a.n;
    memcpy(snapshot, a.t, snap_n);
    {
        Sink *s = (Sink *)malloc(sizeof(Sink));
        FoldLog lg;
        memset(s, 0, sizeof(*s));
        s->d = &a;
        lg.ctx = s;
        lg.range = s_range;
        lg.bytes = s_bytes;
        /* the log of the whole transcript is the plain transcript */
        CHECK_EQ_LONG(fold_log(&a.fs, a.n, &lg), 0);
        CHECK_EQ_LONG(s->n, b.n);
        CHECK(memcmp(s->out, b.t, b.n) == 0);
        free(s);
    }
    for (i = 0; i < a.fs.n; i++)
        CHECK(fold_toggle(&a.fs, &a.ops, i) == FT_OPENED);
    /* with every fold open, the only difference from the plain transcript is
       each fold's marker line, which sits in front of its lines */
    {
        char *rest = (char *)malloc(DOCMAX);
        long k = 0, pos = 0;
        int f;
        for (f = 0; f < a.fs.n; f++) {
            memcpy(rest + k, a.t + pos, a.fs.f[f].off - pos);
            k += a.fs.f[f].off - pos;
            pos = a.fs.f[f].off + a.fs.f[f].len + 1;      /* the marker and its CR */
        }
        memcpy(rest + k, a.t + pos, a.n - pos);
        k += a.n - pos;
        CHECK_EQ_LONG(k, b.n);
        CHECK(memcmp(rest, b.t, b.n) == 0);
        free(rest);
    }
    for (i = 0; i < a.fs.n; i++)
        CHECK(fold_toggle(&a.fs, &a.ops, i) == FT_CLOSED);
    CHECK_EQ_LONG(a.n, snap_n);
    CHECK(memcmp(a.t, snapshot, snap_n) == 0);
    free(snapshot);
    fold_free(&a.fs);
}

/* Numbered lines from batch.c: the marker shows the first line's code, not
   its number, and a blank first line is skipped. */
static void test_numbered_from_batch(void)
{
    Doc a;
    Batch b;
    int i;
    doc_init(&a, 0);
    batch_init(&b);
    batch_event(&b, EV_TEXT, "hi", 2);
    batch_event(&b, EV_CODE_BEGIN, "", 0);
    batch_event(&b, EV_CODE, "", 0);
    batch_event(&b, EV_CODE, "  def parse(text):", 18);
    for (i = 0; i < 12; i++)
        batch_event(&b, EV_CODE, "    pass", 8);
    batch_event(&b, EV_CODE_END, "", 0);
    for (i = 0; i < b.nruns; i++) {
        char run[2000];
        memcpy(run, b.text + b.runs[i].start, b.runs[i].len);
        run[b.runs[i].len] = 0;
        add(&a, 1, b.runs[i].style, run);
    }
    CHECK_DOC(&a, "hi\r[+] Code, 14 lines: def parse(text):");
    batch_free(&b);
    fold_free(&a.fs);
}

int main(void)
{
    test_numbered_from_batch();
    test_code();
    test_code_one_run_and_continuation();
    test_tools();
    test_tool_runs_end();
    test_width();
    test_log_and_cut();
    test_open_fold_log_and_cut();
    test_shift();
    test_cut_while_growing();
    test_body_cap();
    test_fold_count_cap();
    {
        unsigned long seed;
        for (seed = 1; seed <= 40; seed++)
            random_stream(seed, seed % 3 == 0 ? 40 : 0);
    }
    CHECK_DONE();
}
