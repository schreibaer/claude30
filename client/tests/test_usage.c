/* The context and cost reading: parsing the $ payload and every number the
   tab gauge and the status line show. */
#include "check.h"
#include "usage.h"

static Usage parsed(const char *s)
{
    Usage u;
    usage_clear(&u);
    if (!usage_parse(&u, s, (long)strlen(s)))
        fprintf(stderr, "did not parse: \"%s\"\n", s);
    CHECK(usage_parse(&u, s, (long)strlen(s)));
    return u;
}

static void status_is(const char *payload, int hide, const char *want)
{
    Usage u = parsed(payload);
    char out[USAGE_STATUS_MAX];
    usage_status(&u, hide, out, sizeof(out));
    CHECK_MEM(out, strlen(out), want);
}

static void test_parse_and_status(void)
{
    Usage u = parsed("84112 200000 0.4414");
    CHECK_EQ_LONG(u.used, 84112);
    CHECK_EQ_LONG(u.window, 200000);
    CHECK_EQ_LONG(u.cost, 4414);
    CHECK_EQ_LONG(usage_percent(&u), 42);
    CHECK(!usage_near_full(&u));
    CHECK(usage_known(&u));
    status_is("84112 200000 0.4414", 0, "ctx 42% $0.44");
    status_is("84112 200000 0.4414", 1, "ctx 42%");
    status_is("- - 0.0123", 0, "$0.01");
    status_is("- - 0.0123", 1, "");
    status_is("5000 - 0.0123", 0, "$0.01");     /* no window: no percentage */
    status_is("5000 200000 -", 0, "ctx 2%");
    status_is("- - -", 0, "");
    status_is("0 200000 0.0000", 0, "ctx 0% $0.00");
}

static void test_unknown_reading(void)
{
    Usage u = parsed("- - -");
    CHECK(!usage_known(&u));
    CHECK_EQ_LONG(usage_percent(&u), -1);
    CHECK_EQ_LONG(usage_fill(&u, 8), 0);
    CHECK(!usage_near_full(&u));
    usage_clear(&u);
    CHECK(!usage_known(&u));
    u = parsed("100 0 0.1000");                  /* window 0 is unknown */
    CHECK_EQ_LONG(u.window, -1);
    CHECK_EQ_LONG(usage_percent(&u), -1);
}

static void test_near_full_edges(void)
{
    Usage u = parsed("160000 200000 1.2000");
    CHECK_EQ_LONG(usage_percent(&u), 80);
    CHECK(usage_near_full(&u));
    u = parsed("159999 200000 1.2000");
    CHECK_EQ_LONG(usage_percent(&u), 79);
    CHECK(!usage_near_full(&u));
    u = parsed("170000 200000 1.2000");
    CHECK_EQ_LONG(usage_percent(&u), 85);
    CHECK(usage_near_full(&u));
    u = parsed("250000 200000 1.2000");          /* past the window clamps */
    CHECK_EQ_LONG(usage_percent(&u), 100);
    CHECK_EQ_LONG(usage_fill(&u, 8), 8);
    u = parsed("800000 1000000 0.5000");           /* a 1M window */
    CHECK_EQ_LONG(usage_percent(&u), 80);
    u = parsed("999999000 999999999 0.5000");      /* nothing overflows */
    CHECK(usage_percent(&u) >= 99);
}

static void test_costs(void)
{
    status_is("- - 9.9949", 0, "$9.99");
    status_is("- - 9.9950", 0, "$10.00");
    status_is("- - 12.5000", 0, "$12.50");
    status_is("- - 99.9990", 0, "$100.00");
    status_is("- - 123.4567", 0, "$123");
    status_is("- - 7", 0, "$7.00");              /* no decimals */
    status_is("- - 3.5", 0, "$3.50");
    status_is("- - 0.00019", 0, "$0.00");        /* extra decimals are cut */
}

static void test_fill(void)
{
    Usage u = parsed("84112 200000 0.0100");     /* 42% of 8 */
    CHECK_EQ_LONG(usage_fill(&u, 8), 3);
    u = parsed("2000 200000 0.0100");            /* 1%: still shows */
    CHECK_EQ_LONG(usage_fill(&u, 8), 1);
    u = parsed("10 200000 0.0100");              /* under 1%: still shows */
    CHECK_EQ_LONG(usage_fill(&u, 8), 1);
    u = parsed("0 200000 0.0100");
    CHECK_EQ_LONG(usage_fill(&u, 8), 0);
    u = parsed("200000 200000 0.0100");
    CHECK_EQ_LONG(usage_fill(&u, 8), 8);
    CHECK_EQ_LONG(usage_fill(&u, 0), 0);
}

static void test_malformed(void)
{
    const char *bad[] = { "", "abc", "1", "1 2", "1 2 x", "1 2 3 4", "1x 2 3", "1 2 -3",
                          "99999999999 2 3", "1 2 3.", "1 2 .5", "-- - -", "1 2 99999999.0" };
    int i;
    for (i = 0; i < (int)(sizeof(bad) / sizeof(bad[0])); i++) {
        Usage u;
        usage_clear(&u);
        u.used = 7;                               /* untouched on failure */
        CHECK(!usage_parse(&u, bad[i], (long)strlen(bad[i])));
        CHECK_EQ_LONG(u.used, 7);
    }
    {
        Usage u = parsed("1 2 3");               /* a spare space is fine */
        CHECK_EQ_LONG(u.cost, 30000);
    }
}

static void test_status_buffer(void)
{
    Usage u = parsed("84112 200000 12345.0");
    char out[8];
    usage_status(&u, 0, out, sizeof(out));
    CHECK_EQ_LONG(strlen(out), 7);                 /* cut to the buffer, ended */
}

int main(void)
{
    test_parse_and_status();
    test_unknown_reading();
    test_near_full_edges();
    test_costs();
    test_fill();
    test_malformed();
    test_status_buffer();
    CHECK_DONE();
}
