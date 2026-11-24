/* Line assembler and prompt encoder. */
#include "check.h"
#include "linebuf.h"
#include "prompt.h"

static char lines[8][6000];
static long lens[8];
static int nlines;

static void got_line(void *ctx, const char *line, long n)
{
    (void)ctx;
    if (nlines < 8 && n < 6000) {
        memcpy(lines[nlines], line, n);
        lens[nlines] = n;
    }
    nlines++;
}

static void test_linebuf(void)
{
    LineBuf lb;
    char big[5001];
    linebuf_init(&lb);

    /* CR LF ends a line; a stray LF is ignored; a line may span feeds. */
    nlines = 0;
    linebuf_feed(&lb, "T hel", 5, got_line, NULL);
    linebuf_feed(&lb, "lo\r\nW\r", 6, got_line, NULL);
    linebuf_feed(&lb, "\n\nU x\r\n", 7, got_line, NULL);
    CHECK_EQ_LONG(nlines, 3);
    CHECK_MEM(lines[0], lens[0], "T hello");
    CHECK_MEM(lines[1], lens[1], "W");
    CHECK_MEM(lines[2], lens[2], "U x");

    /* Lines of any length arrive whole. */
    nlines = 0;
    memset(big, 'x', 5000);
    big[0] = 'D';
    big[1] = ' ';
    big[5000] = '\r';
    linebuf_feed(&lb, big, 5001, got_line, NULL);
    CHECK_EQ_LONG(nlines, 1);
    CHECK_EQ_LONG(lens[0], 5000);
    linebuf_free(&lb);
}

static void encode(const char *text, const char *expect, int expect_lines)
{
    char out[512];
    int n = 0;
    long len = prompt_encode(text, (long)strlen(text), "3P ", out, sizeof(out), &n);
    CHECK_MEM(out, len, expect);
    CHECK_EQ_LONG(n, expect_lines);
}

static void test_prompt(void)
{
    char small[8];
    int n;
    encode("fix the parser", "3P fix the parser\r", 1);
    encode("line one\rline two\rthree", "3P line one\\\r3P line two\\\r3P three\r", 3);
    encode("crlf\r\nnext", "3P crlf\\\r3P next\r", 2);
    encode("lf\nnext", "3P lf\\\r3P next\r", 2);
    /* A last line ending in a backslash gets a space, or the bridge waits. */
    encode("see C:\\", "3P see C:\\ \r", 1);
    /* A middle line ending in a backslash keeps it; the bridge strips one. */
    encode("a\\\rb", "3P a\\\\\r3P b\r", 2);
    /* Prefixed, a flow-control word is an ordinary prompt line. */
    encode("!ACK", "3P !ACK\r", 1);
    encode("/new", "3P /new\r", 1);
    /* An empty middle line still gets its prefix. */
    encode("a\r\rb", "3P a\\\r3P \\\r3P b\r", 3);
    /* Mac Roman bytes pass through unchanged. */
    encode("caf\x8E", "3P caf\x8E\r", 1);
    CHECK_EQ_LONG(prompt_encode("abc", 3, "3P ", small, sizeof(small), &n), -1);
}

int main(void)
{
    test_linebuf();
    test_prompt();
    CHECK_DONE();
}
