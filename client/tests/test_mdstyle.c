/* The Markdown styler: what prose becomes as styled runs, and how a paragraph
   that arrives in pieces ends up the same as one that arrives whole. */
#include "check.h"
#include "batch.h"
#include "mdstyle.h"

/* The runs as text: "faces:text" per run, runs joined with "|", with the
   faces B(old) I(talic) U(nderline) E(xtend) M(ono) before the colon when
   there are any, and CR shown as "/". */
static void dump(const Batch *b, char *out, size_t cap)
{
    static const char L[] = "BIUEM";
    size_t o = 0;
    int i, k;
    long j;
    for (i = 0; i < b->nruns && o + 40 < cap; i++) {
        const Run *r = &b->runs[i];
        if (i)
            out[o++] = '|';
        if (r->flags) {
            for (k = 0; k < 5; k++)
                if (r->flags & (1 << k))
                    out[o++] = L[k];
            out[o++] = ':';
        }
        for (j = 0; j < r->len && o + 4 < cap; j++) {
            char ch = b->text[r->start + j];
            out[o++] = ch == '\r' ? '/' : ch;
        }
    }
    out[o] = 0;
}

static void same(const char *what, const char *got, const char *want)
{
    check_count++;
    if (strcmp(got, want) != 0) {
        check_failures++;
        fprintf(stderr, "%s\n   got  \"%s\"\n   want \"%s\"\n", what, got, want);
    }
}

static void event(Batch *b, EventKind k, const char *s)
{
    batch_event(b, k, s, (long)strlen(s));
}

/* One line of prose in one piece. */
static void expect(const char *in, const char *want)
{
    Batch b;
    char out[8192];
    batch_init(&b);
    batch_set_rule(&b, 5);
    event(&b, EV_TEXT, in);
    batch_flush(&b);
    dump(&b, out, sizeof out);
    same(in, out, want);
    batch_free(&b);
}

static void test_inline(void)
{
    /* the acceptance example: code, bold, and marks that stay as typed */
    expect("Call `parse_line()` on **each** row; snake_case and 2 * 3 stay",
           "Call |M:parse_line()| on |B:each| row; snake_case and 2 * 3 stay");
    expect("***both***", "BI:both");
    expect("**bold *and italic***", "B:bold |BI:and italic");
    expect("*a **b** c*", "I:a |BI:b|I: c");
    expect("__x__ and _y_", "B:x| and |I:y");
    expect("(_x_) and (**y**)", "(|I:x|) and (|B:y|)");
    expect("``a ` b``", "M:a ` b");
    expect("```c```", "M:c");
    expect("`**x**` and `a_b_c`", "M:**x**| and |M:a_b_c");
    expect("a `b", "a `b");

    /* text that is not Markdown shows as typed */
    expect("*Note: this asterisk never closes", "*Note: this asterisk never closes");
    expect("snake_case_name", "snake_case_name");
    expect("2 * 3 * 4", "2 * 3 * 4");
    expect("a*b*c", "a*b*c");
    expect("~~gone~~", "~~gone~~");
    expect("<br> and <b>x</b>", "<br> and <b>x</b>");
    expect("**not closed and __neither", "**not closed and __neither");
    expect("#hashtag", "#hashtag");
    expect("", "");

    /* escapes */
    expect("\\*not italic\\*", "*not italic*");
    expect("a \\_b\\_ c \\# d \\` e", "a _b_ c # d ` e");
    expect("\\\\ stays", "\\ stays");

    /* links and addresses */
    expect("[docs](https://example.com/x)", "U:docs");
    expect("see [the **docs**](https://e.com) now", "see |U:the |BU:docs| now");
    expect("<https://a.b/c>", "U:https://a.b/c");
    expect("see https://a.b/c.", "see |U:https://a.b/c|.");
    expect("(https://a.b/c) and https://x.y/(z)", "(|U:https://a.b/c|) and |U:https://x.y/(z)");
    expect("https://a.b/c_d_e and more", "U:https://a.b/c_d_e| and more");
    expect("[x] done and [1] [WARN] too", "[x] done and [1] [WARN] too");
    expect("array[i] and (a)(b)", "array[i] and (a)(b)");
    expect("[not a link] (x)", "[not a link] (x)");
}

static void test_lines(void)
{
    expect("# Title", "BUE:Title");
    expect("## Sub", "BE:Sub");
    expect("### Deeper", "B:Deeper");
    expect("#### Deep", "B:Deep");
    expect("###### Six", "B:Six");
    expect("####### Seven", "####### Seven");
    expect("## A **b** c", "BE:A b c");

    expect("- a", "\xA5 a");
    expect("* b", "\xA5 b");
    expect("+ c", "\xA5 c");
    expect("  - nested", "  \xD0 nested");
    expect("- first with **bold**", "\xA5 first with |B:bold");
    expect("1. first **one**", "1. first |B:one");
    expect("-nope and *nope*", "-nope and |I:nope");
    expect("**bold** first", "B:bold| first");

    expect("> quoted *word*", "I:| quoted word");
    expect(">no space", ">no space");

    expect("---", "\xD1\xD1\xD1\xD1\xD1");
    expect("***", "\xD1\xD1\xD1\xD1\xD1");
    expect("___", "\xD1\xD1\xD1\xD1\xD1");
    expect("- - -", "\xD1\xD1\xD1\xD1\xD1");
    expect("-----", "\xD1\xD1\xD1\xD1\xD1");
    expect("--- text", "--- text");
    expect("--", "--");

    expect("| a | **b** |", "M:| a | **b** |");
    expect("|---|:---:|", "M:|---+-----|");
    expect("| --- | --- |", "M:|-----+-----|");
    expect("|", "M:|");
}

static void test_pieces(void)
{
    Batch b;
    char out[4096];

    /* a bold phrase that closes in the next piece */
    batch_init(&b);
    event(&b, EV_TEXT, "The **quick brown");
    dump(&b, out, sizeof out);
    same("before the closing mark", out, "The ");
    event(&b, EV_TEXT_CONT, "fox** jumps");
    dump(&b, out, sizeof out);
    same("after it", out, "The |B:quick brown fox| jumps");
    batch_free(&b);

    /* the same across two draws: the held text survives batch_clear */
    batch_init(&b);
    event(&b, EV_TEXT, "The **quick brown");
    batch_clear(&b);
    event(&b, EV_TEXT_CONT, "fox** jumps");
    dump(&b, out, sizeof out);
    same("after a draw", out, "B:quick brown fox| jumps");
    batch_free(&b);

    /* an unclosed span is typed when another block comes */
    batch_init(&b);
    event(&b, EV_TEXT, "a **b");
    dump(&b, out, sizeof out);
    same("held", out, "a ");
    event(&b, EV_TOOL, "Read x");
    dump(&b, out, sizeof out);
    same("flushed by a tool line", out, "a **b|/\xA5 Read x");
    CHECK_EQ_LONG(b.runs[0].style, RS_PROSE);
    CHECK_EQ_LONG(b.runs[1].style, RS_TOOL);
    CHECK_EQ_LONG(b.runs[1].flags, 0);
    batch_free(&b);

    /* a held span grows past the limit: the mark is text */
    batch_init(&b);
    {
        char x[201];
        memset(x, 'x', 200);
        x[200] = 0;
        event(&b, EV_TEXT, "**a");
        event(&b, EV_TEXT_CONT, x);
        event(&b, EV_TEXT_CONT, x);
        CHECK_EQ_LONG(b.nruns, 0);
        event(&b, EV_TEXT_CONT, x);
        CHECK(b.nruns >= 1);
        CHECK(b.len > 600);
        CHECK_MEM(b.text, 5L, "**a x");
        event(&b, EV_TEXT_CONT, "more**");
        batch_flush(&b);
        dump(&b, out, sizeof out);
        CHECK(strchr(out, ':') == NULL);      /* nothing styled */
    }
    batch_free(&b);

    /* a lone marker is decided when the rest arrives */
    batch_init(&b);
    event(&b, EV_TEXT, "##");
    CHECK_EQ_LONG(b.nruns, 0);
    event(&b, EV_TEXT_CONT, "Setup");
    dump(&b, out, sizeof out);
    same("## then Setup", out, "BE:Setup");
    batch_free(&b);

    batch_init(&b);
    event(&b, EV_TEXT, "  -");
    event(&b, EV_TEXT_CONT, "then run it");
    dump(&b, out, sizeof out);
    same("indented dash then text", out, "  \xD0 then run it");
    batch_free(&b);

    batch_init(&b);
    event(&b, EV_TEXT, "-");
    event(&b, EV_TOOL, "Read x");
    dump(&b, out, sizeof out);
    same("a lone dash and then a tool", out, "-|/\xA5 Read x");
    batch_free(&b);

    /* [x] is text at once; the span after it waits */
    batch_init(&b);
    event(&b, EV_TEXT, "[x] done");
    dump(&b, out, sizeof out);
    same("[x] released", out, "[x] done");
    event(&b, EV_TEXT_CONT, "**now** ok");
    dump(&b, out, sizeof out);
    same("then bold", out, "[x] done |B:now| ok");
    batch_free(&b);

    /* a bracket and a code span that need the next piece */
    batch_init(&b);
    event(&b, EV_TEXT, "see [the");
    dump(&b, out, sizeof out);
    same("[ waits", out, "see ");
    event(&b, EV_TEXT_CONT, "docs](https://x.y/z) now");
    dump(&b, out, sizeof out);
    same("link", out, "see |U:the docs| now");
    batch_free(&b);

    batch_init(&b);
    event(&b, EV_TEXT, "use `foo");
    event(&b, EV_TEXT_CONT, "bar` now");
    dump(&b, out, sizeof out);
    same("code across pieces", out, "use |M:foo bar| now");
    batch_free(&b);

    /* a table row streams as it comes; a separator row waits for its end */
    batch_init(&b);
    event(&b, EV_TEXT, "| a");
    dump(&b, out, sizeof out);
    same("row", out, "M:| a");
    event(&b, EV_TEXT_CONT, "| b |");
    dump(&b, out, sizeof out);
    same("row, more", out, "M:| a | b |");
    batch_free(&b);

    batch_init(&b);
    event(&b, EV_TEXT, "|---");
    CHECK_EQ_LONG(b.nruns, 0);
    event(&b, EV_TEXT_CONT, "|:--:|");
    batch_flush(&b);
    dump(&b, out, sizeof out);
    same("separator row", out, "M:|----+----|");
    batch_free(&b);

    /* a line the app drew itself (noise) comes between two pieces: the
       continuation starts a line of its own, and a held span is flushed first */
    batch_init(&b);
    event(&b, EV_TEXT, "abc *d");
    batch_marker(&b, RS_NOISE, "Line noise");
    event(&b, EV_TEXT_CONT, "e* f");
    dump(&b, out, sizeof out);
    same("noise between pieces", out, "abc *d|/Line noise|/e* f");
    batch_free(&b);

    /* a marker line, a prompt and code draw with no faces */
    batch_init(&b);
    batch_prompt(&b, "fix **it**", 10);
    batch_marker(&b, RS_ERROR, "Error: *x*");
    event(&b, EV_CODE, "a **b** c");
    {
        int i;
        for (i = 0; i < b.nruns; i++)
            CHECK_EQ_LONG(b.runs[i].flags, 0);
    }
    batch_free(&b);
}

/* A paragraph split at any space and joined the way the app joins pieces
   draws exactly like the whole line. */
static void test_split_anywhere(void)
{
    static const char *lines[] = {
        "Call `parse_line()` on **each** row; snake_case and 2 * 3 stay",
        "A **bold phrase that runs on** and *then italic words* and ***both of them*** here",
        "**bold *and italic*** and *a **b** c* and __x__ and _y_",
        "## A heading with `code` and **bold**",
        "# Title words",
        "- a bullet with **bold text** inside",
        "  - a nested bullet with [a link](https://e.com/x) in it",
        "- - -",
        "> a quote with *emphasis here* and `code`",
        "see https://a.b/c_d_e. and [docs](https://x.y/z) now",
        "use `a b` and ``c d e`` here",
        "[x] done [1] and array[i] and [WARN] left",
        "| a b | c d |",
        "| --- | --- |",
        "1. first **one** and 2 * 3 * 4",
        "*Note: this asterisk never closes at all",
        "a \\*b c\\* d",
        0
    };
    int i;
    long p;
    for (i = 0; lines[i]; i++) {
        const char *s = lines[i];
        long n = (long)strlen(s);
        Batch whole;
        char want[8192], got[8192];
        batch_init(&whole);
        batch_set_rule(&whole, 5);
        batch_event(&whole, EV_TEXT, s, n);
        batch_flush(&whole);
        dump(&whole, want, sizeof want);
        batch_free(&whole);
        for (p = 0; p < n; p++) {
            Batch b;
            if (s[p] != ' ')
                continue;
            batch_init(&b);
            batch_set_rule(&b, 5);
            batch_event(&b, EV_TEXT, s, p);
            batch_event(&b, EV_TEXT_CONT, s + p + 1, n - p - 1);
            batch_flush(&b);
            dump(&b, got, sizeof got);
            if (strcmp(got, want) != 0) {
                char what[200];
                snprintf(what, sizeof what, "split of \"%s\" at %ld", s, p);
                same(what, got, want);
            } else {
                check_count++;
            }
            batch_free(&b);
        }
    }
}

int main(void)
{
    test_inline();
    test_lines();
    test_pieces();
    test_split_anywhere();
    CHECK_DONE();
}
