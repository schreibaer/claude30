/* Case-insensitive Mac Roman search, in a buffer and in a chunked file. */
#include "check.h"
#include "find.h"

#define P(s) s, (long)strlen(s)

static void test_case(void)
{
    const char *t = "def Parse_Header(x): PARSE";
    CHECK_EQ_LONG(find_forward(t, (long)strlen(t), 0, P("parse")), 4);
    CHECK_EQ_LONG(find_forward(t, (long)strlen(t), 5, P("parse")), 21);
    CHECK_EQ_LONG(find_backward(t, (long)strlen(t), (long)strlen(t), P("PARSE")), 21);
    CHECK_EQ_LONG(find_backward(t, (long)strlen(t), 21, P("PARSE")), 4);
    CHECK_EQ_LONG(find_backward(t, (long)strlen(t), 4, P("PARSE")), -1);
    /* Mac Roman: e-acute 0x8E matches E-acute 0x83 */
    CHECK_EQ_LONG(find_forward("a\x8E" "b", 3, 0, "\x83", 1), 1);
    CHECK_EQ_LONG(find_forward("a\x83" "b", 3, 0, "\x8E", 1), 1);
    CHECK_EQ_LONG(find_forward("\x8A", 1, 0, "\x80", 1), 0);     /* a-umlaut */
    CHECK(find_fold(0xA0) == 0xA0);
}

/* Every Mac Roman capital folds to its small letter, and folding is stable. */
static void test_table(void)
{
    static const unsigned char pairs[][2] = {
        { 0xE7, 0x87 }, { 0xCB, 0x88 }, { 0xE5, 0x89 }, { 0x80, 0x8A }, { 0xCC, 0x8B },
        { 0x81, 0x8C }, { 0x82, 0x8D }, { 0x83, 0x8E }, { 0xE9, 0x8F }, { 0xE6, 0x90 },
        { 0xE8, 0x91 }, { 0xEA, 0x92 }, { 0xED, 0x93 }, { 0xEB, 0x94 }, { 0xEC, 0x95 },
        { 0x84, 0x96 }, { 0xEE, 0x97 }, { 0xF1, 0x98 }, { 0xEF, 0x99 }, { 0x85, 0x9A },
        { 0xCD, 0x9B }, { 0xF2, 0x9C }, { 0xF4, 0x9D }, { 0xF3, 0x9E }, { 0x86, 0x9F },
        { 0xAE, 0xBE }, { 0xAF, 0xBF }, { 0xCE, 0xCF },
    };
    unsigned i;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        CHECK(find_fold(pairs[i][0]) == pairs[i][1]);
        CHECK(find_fold(pairs[i][1]) == pairs[i][1]);
    }
    for (i = 0; i < 256; i++)
        CHECK(find_fold(find_fold((unsigned char)i)) == find_fold((unsigned char)i));
}

static void test_edges(void)
{
    CHECK_EQ_LONG(find_forward("abc", 3, 0, "abc", 3), 0);
    CHECK_EQ_LONG(find_forward("xabc", 4, 0, "abc", 3), 1);
    CHECK_EQ_LONG(find_forward("abcx", 4, 1, "abc", 3), -1);   /* forward from the end */
    CHECK_EQ_LONG(find_backward("abcx", 4, 0, "abc", 3), -1);  /* backward from offset 0 */
    CHECK_EQ_LONG(find_backward("abcx", 4, 3, "abc", 3), 0);
    CHECK_EQ_LONG(find_forward("abc", 3, 0, "", 0), -1);       /* empty never matches */
    CHECK_EQ_LONG(find_backward("abc", 3, 3, "", 0), -1);
    CHECK_EQ_LONG(find_forward("ab", 2, 0, "abc", 3), -1);     /* longer than the buffer */
    CHECK_EQ_LONG(find_backward("ab", 2, 2, "abc", 3), -1);
}

typedef struct { const char *d; long n; int fail; } Mem;

static int rd(void *ctx, long off, long len, char *out)
{
    Mem *m = ctx;
    if (m->fail || off < 0 || off + len > m->n)
        return 1;
    memcpy(out, m->d + off, len);
    return 0;
}

/* The match straddles a chunk edge at every position of a small chunk size. */
static void test_chunks(void)
{
    char text[64], buf[16];
    long at, cap;
    Mem m;
    const char *pat = "needl";
    m.fail = 0;
    for (cap = 11; cap <= 14; cap++)
        for (at = 0; at + 5 <= 60; at++) {
            memset(text, '.', sizeof(text));
            memcpy(text + at, "NEEDL", 5);
            m.d = text;
            m.n = 60;
            CHECK_EQ_LONG(find_file_backward(rd, &m, 60, 60, pat, 5, buf, cap), at);
            CHECK_EQ_LONG(find_file_forward(rd, &m, 60, 0, pat, 5, buf, cap), at);
            CHECK_EQ_LONG(find_file_backward(rd, &m, 60, at + 4, pat, 5, buf, cap), -1);
            CHECK_EQ_LONG(find_file_forward(rd, &m, 60, at + 1, pat, 5, buf, cap), -1);
        }
    m.fail = 1;
    CHECK_EQ_LONG(find_file_backward(rd, &m, 60, 60, pat, 5, buf, 14), -2);
    CHECK_EQ_LONG(find_file_forward(rd, &m, 60, 0, pat, 5, buf, 14), -2);
    m.fail = 0;
    CHECK_EQ_LONG(find_file_backward(rd, &m, 60, 60, pat, 5, buf, 8), -1);   /* buffer too small */
    CHECK_EQ_LONG(find_file_forward(rd, &m, 60, 0, pat, 5, buf, 8), -1);
    CHECK_EQ_LONG(find_forward("abc", 3, -5, "a", 1), 0);                    /* from < 0 */
    CHECK_EQ_LONG(find_backward("abc", 3, 99, "c", 1), 2);                   /* before > len */
}

/* Two matches: backward finds the later first, then the earlier. */
static void test_order(void)
{
    char text[40], buf[16];
    Mem m;
    long a, b;
    memset(text, '.', sizeof(text));
    memcpy(text + 3, "key", 3);
    memcpy(text + 30, "KEY", 3);
    m.d = text; m.n = 40; m.fail = 0;
    a = find_file_backward(rd, &m, 40, 40, "key", 3, buf, 12);
    b = find_file_backward(rd, &m, 40, a, "key", 3, buf, 12);
    CHECK_EQ_LONG(a, 30);
    CHECK_EQ_LONG(b, 3);
}

int main(void)
{
    test_case();
    test_table();
    test_edges();
    test_chunks();
    test_order();
    CHECK_DONE();
}
