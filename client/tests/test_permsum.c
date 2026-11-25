/* The permission dialog's bold headline. */
#include "check.h"
#include "permsum.h"

#define WIDE 1000L

/* Builds the headline for a header and body lines. */
static const char *head_for(const char *hdr, const char **lines, long n, int incomplete, long width)
{
    static char out[PERMSUM_MAX];
    char body[4096];
    long ends[64], len = 0, i;
    for (i = 0; i < n; i++) {
        long l = (long)strlen(lines[i]);
        memcpy(body + len, lines[i], l);
        len += l;
        ends[i] = len;
    }
    permsum_build(hdr, (long)strlen(hdr), body, ends, n, incomplete, NULL, width, out, sizeof(out));
    return out;
}

#define EXPECT(got, want) do { \
    const char *g_ = (got); \
    CHECK_MEM(g_, (long)strlen(g_), want); \
} while (0)

static void test_edit(void)
{
    /* two hunks: +12 -3, as the bridge's diff body (no file headers) */
    const char *two[18];
    long n = 0, i;
    char added[12][8];
    two[n++] = "@@ -1,3 +1,2 @@";
    two[n++] = "-old one";
    two[n++] = " context";
    two[n++] = "-old two";
    two[n++] = "@@ -20,2 +19,12 @@";
    two[n++] = "-old three";
    for (i = 0; i < 12 && n < 18; i++) {
        snprintf(added[i], sizeof(added[i]), "+new %ld", i);
        two[n++] = added[i];
    }
    EXPECT(head_for("Edit src/parser.py", two, n, 0, WIDE), "Edit parser.py: +12 -3 lines in 2 places");

    {
        const char *one[] = { "@@ -1,1 +1,1 @@", "-a", "+b" };
        EXPECT(head_for("Edit src/x.c", one, 3, 0, WIDE), "Edit x.c: +1 -1 lines in 1 place");
    }
    {   /* a removed line whose own text starts with "--" and an added one with "++" */
        const char *dash[] = { "@@ -1,2 +1,2 @@", "---x", "+++y" };
        EXPECT(head_for("Edit a/b.txt", dash, 3, 0, WIDE), "Edit b.txt: +1 -1 lines in 1 place");
    }
    {
        const char *multi[] = { "@@ -1 +1 @@", "-a", "+b", "@@ -9 +9 @@", "-c", "+d", "@@ -30 +30 @@", "+e" };
        EXPECT(head_for("MultiEdit lib/m.py", multi, 8, 0, WIDE), "MultiEdit m.py: +3 -2 lines in 3 places");
    }
    EXPECT(head_for("Edit src/parser.py", NULL, 0, 0, WIDE), "Edit parser.py");
}

static void test_write(void)
{
    const char *w[5] = { "+a", "+b", "+c", "+d", "+e" };
    /* the bridge's real Write header carries its own count */
    EXPECT(head_for("Write src/foo.py (5 lines)", w, 5, 0, WIDE), "Write foo.py: +5 lines");
    EXPECT(head_for("Write src/foo.py (0 lines)", NULL, 0, 0, WIDE), "Write foo.py");
    /* a name that only looks like a count stays */
    EXPECT(head_for("Write notes (draft)", NULL, 0, 0, WIDE), "Write notes (draft)");
}

static void test_bash(void)
{
    const char *one[] = { "git status" };
    const char *wide[] = { "echo start && echo a-very-long-command-that-will-not-fit-in-the-headline" };
    const char *four[] = { "set -e", "cd /tmp", "make", "make install" };
    EXPECT(head_for("Bash git status", NULL, 0, 0, WIDE), "Bash: git status");
    EXPECT(head_for("Bash git status", one, 1, 0, WIDE), "Bash: git status");
    EXPECT(head_for("Bash echo start", wide, 1, 0, 40),
           "Bash: long command (72 characters), read it below");
    EXPECT(head_for("Bash set -e", four, 4, 0, WIDE), "Bash: 4-line command, read it below");
    /* the subagent's type stays */
    EXPECT(head_for("Bash (general-purpose) git log -5", NULL, 0, 0, WIDE),
           "Bash (general-purpose): git log -5");
    /* a command in parentheses is a command, not a type */
    EXPECT(head_for("Bash (ls)", NULL, 0, 0, WIDE), "Bash: (ls)");
    EXPECT(head_for("Bash", NULL, 0, 0, WIDE), "Bash");
}

static void test_other(void)
{
    const char *seven[] = { "a", "b", "c", "d", "e", "f", "g" };
    EXPECT(head_for("Read src/parser.py", NULL, 0, 0, WIDE), "Read parser.py");
    EXPECT(head_for("Foo target", seven, 7, 0, WIDE), "Foo target: 7 lines below");
    EXPECT(head_for("Foo", seven, 7, 0, WIDE), "Foo: 7 lines below");
    EXPECT(head_for("WebFetch https://example.com/a/b", NULL, 0, 0, WIDE), "WebFetch https://example.com/a/b");
}

static void test_incomplete_and_empty(void)
{
    const char *w[] = { "+a" };
    EXPECT(head_for("Write foo.py (1 lines)", w, 1, 1, WIDE), "Incomplete request: deny it");
    EXPECT(head_for("Bash rm -rf /", NULL, 0, 1, WIDE), "Incomplete request: deny it");
    EXPECT(head_for("", NULL, 0, 0, WIDE), "Permission request");
    EXPECT(head_for("   ", NULL, 0, 0, WIDE), "Permission request");
    /* control bytes in a header become spaces */
    EXPECT(head_for("Read a\tb", NULL, 0, 0, WIDE), "Read a b");
}

static long measure3(const char *s, long n)
{
    (void)s;
    return n * 3;
}

static void test_fit(void)
{
    static char out[PERMSUM_MAX];
    long n;
    const char *hdr = "Read some/dir/averyveryverylongfilename.c";
    /* a file name that does not fit is cut with the Mac Roman ellipsis */
    n = permsum_build(hdr, (long)strlen(hdr), NULL, NULL, 0, 0, NULL, 20, out, sizeof(out));
    CHECK(n <= 20);
    CHECK(out[n - 1] == '\xC9');
    CHECK(memcmp(out, "Read averyv", 11) == 0);
    /* the width comes from the caller's measure */
    n = permsum_build(hdr, (long)strlen(hdr), NULL, NULL, 0, 0, measure3, 60, out, sizeof(out));
    CHECK(n * 3 <= 60);
    CHECK(out[n - 1] == '\xC9');
    /* a tiny cap never overruns */
    n = permsum_build(hdr, (long)strlen(hdr), NULL, NULL, 0, 0, NULL, WIDE, out, 8);
    CHECK(n <= 7);
    CHECK(out[n] == 0);
    /* a header much longer than the buffer is still bounded */
    {
        char big[1000];
        memset(big, 'x', sizeof(big));
        memcpy(big, "Read ", 5);
        n = permsum_build(big, (long)sizeof(big), NULL, NULL, 0, 0, NULL, WIDE, out, sizeof(out));
        CHECK(n < PERMSUM_MAX);
        CHECK(out[n] == 0);
    }
}

int main(void)
{
    test_edit();
    test_write();
    test_bash();
    test_other();
    test_incomplete_and_empty();
    test_fit();
    CHECK_DONE();
}
