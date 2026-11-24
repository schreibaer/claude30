/* Transcript batcher: styled runs and the trim plan. */
#include "check.h"
#include "batch.h"

static const char *run_text(Batch *b, int i, long *len)
{
    *len = b->runs[i].len;
    return b->text + b->runs[i].start;
}

static void test_runs(void)
{
    Batch b;
    long n;
    const char *t;
    batch_init(&b);

    /* Prose, a tool line and code: three runs, in order, with their styles. */
    batch_event(&b, EV_TEXT, "Hello", 5);
    batch_event(&b, EV_TOOL, "Read a.c", 8);
    batch_event(&b, EV_CODE_BEGIN, "", 0);
    batch_event(&b, EV_CODE, "int x;", 6);
    CHECK_EQ_LONG(b.nruns, 3);
    CHECK_EQ_LONG(b.runs[0].style, RS_PROSE);
    CHECK_EQ_LONG(b.runs[1].style, RS_TOOL);
    CHECK_EQ_LONG(b.runs[2].style, RS_CODE);
    t = run_text(&b, 0, &n);
    CHECK_MEM(t, n, "Hello");
    t = run_text(&b, 1, &n);
    CHECK_MEM(t, n, "\r\xA5 Read a.c");
    t = run_text(&b, 2, &n);
    CHECK_MEM(t, n, "\r  int x;");

    /* + joins prose with a space and code with nothing. */
    batch_clear(&b);
    batch_event(&b, EV_TEXT, "one", 3);
    batch_event(&b, EV_TEXT_CONT, "two", 3);
    batch_event(&b, EV_CODE, "ab", 2);
    batch_event(&b, EV_CODE_CONT, "cd", 2);
    t = run_text(&b, 0, &n);
    CHECK_MEM(t, n, "\rone two");
    t = run_text(&b, 1, &n);
    CHECK_MEM(t, n, "\r  abcd");

    /* A sent prompt comes before Claude's output for that turn. */
    batch_clear(&b);
    batch_prompt(&b, "fix it", 6);
    batch_event(&b, EV_TEXT, "Done", 4);
    CHECK_EQ_LONG(b.runs[0].style, RS_PROMPT);
    t = run_text(&b, 0, &n);
    CHECK_MEM(t, n, "\r> fix it");
    CHECK_EQ_LONG(b.runs[1].style, RS_PROSE);
    batch_free(&b);
}

static void test_styles_distinct(void)
{
    /* Every kind of line gets its own style and marker. */
    Batch b;
    long n;
    const char *t;
    batch_init(&b);
    batch_event(&b, EV_ERROR, "boom", 4);
    batch_event(&b, EV_INFO, "Bye.", 4);
    batch_event(&b, EV_RESULT, "success $0.01", 13);
    batch_event(&b, EV_INTERRUPTED, "", 0);
    batch_marker(&b, RS_NOISE, "Line noise: output above may be damaged");
    batch_marker(&b, RS_DIVIDER, "New conversation");
    CHECK_EQ_LONG(b.nruns, 6);
    CHECK_EQ_LONG(b.runs[0].style, RS_ERROR);
    t = run_text(&b, 0, &n);
    CHECK_MEM(t, n, "Error: boom");
    CHECK_EQ_LONG(b.runs[1].style, RS_INFO);
    CHECK_EQ_LONG(b.runs[2].style, RS_RESULT);
    t = run_text(&b, 2, &n);
    CHECK_MEM(t, n, "\r\xD1 success $0.01");
    CHECK_EQ_LONG(b.runs[3].style, RS_INTERRUPTED);
    CHECK_EQ_LONG(b.runs[4].style, RS_NOISE);
    CHECK_EQ_LONG(b.runs[5].style, RS_DIVIDER);
    batch_free(&b);
}

static void test_long_line_split(void)
{
    /* A 40 KB code line never yields a run or line TextEdit cannot take. */
    static char big[40960];
    Batch b;
    int i;
    long line = 0, j;
    memset(big, 'x', sizeof(big));
    batch_init(&b);
    batch_event(&b, EV_CODE, big, sizeof(big));
    for (i = 0; i < b.nruns; i++)
        CHECK(b.runs[i].len <= BATCH_MAX_RUN);
    for (j = 0; j < b.len; j++) {
        if (b.text[j] == '\r')
            line = 0;
        else
            line++;
        if (line > BATCH_MAX_LINE + 2) {
            CHECK(0);
            break;
        }
    }
    CHECK(b.len >= (long)sizeof(big));
    batch_free(&b);
}

static void test_trim(void)
{
    static char text[40000];
    long i, cut;

    /* Under both limits: nothing to cut. */
    memset(text, 'a', 20000);
    CHECK_EQ_LONG(batch_trim_cut(text, 20000, 0, 0), 0);

    /* 28.5 KB of 50-byte lines: cut to about 20 KB, at a line break. */
    for (i = 0; i < 29184; i++)
        text[i] = (i % 50 == 49) ? '\r' : 'a';
    cut = batch_trim_cut(text, 29184, 0, 0);
    CHECK(cut > 0);
    CHECK(text[cut - 1] == '\r');
    CHECK(29184 - cut <= TRIM_KEEP_BYTES);
    CHECK(29184 - cut > TRIM_KEEP_BYTES - 60);

    /* 6,500 short lines under 28 KB still trim on the line count. */
    for (i = 0; i < 26000; i++)
        text[i] = (i % 4 == 3) ? '\r' : 'b';
    cut = batch_trim_cut(text, 26000, 0, 0);
    CHECK(cut > 0);
    CHECK(text[cut - 1] == '\r');
    {
        long left = 1;
        for (i = cut; i < 26000; i++)
            if (text[i] == '\r')
                left++;
        CHECK(left <= TRIM_KEEP_LINES + 1);
    }

    /* Soft-wrapped display lines count too. */
    memset(text, 'c', 10000);
    text[5000] = '\r';
    cut = batch_trim_cut(text, 10000, 7000, 0);
    CHECK_EQ_LONG(cut, 5001);

    /* Text about to arrive counts toward the byte limit. */
    for (i = 0; i < 20000; i++)
        text[i] = (i % 100 == 99) ? '\r' : 'd';
    cut = batch_trim_cut(text, 20000, 0, 10000);
    CHECK(cut > 0);
    CHECK(text[cut - 1] == '\r');
    CHECK(20000 - cut + 10000 <= TRIM_KEEP_BYTES);
}

int main(void)
{
    test_runs();
    test_styles_distinct();
    test_long_line_split();
    test_trim();
    CHECK_DONE();
}
