/* Subagent lines: payload parsing, composed text, slot reuse, fitting. */
#include "check.h"
#include "agentline.h"

#define RAQ "\xC8"
#define MID "\xE1"

static long measure(const char *s, long n)
{
    (void)s;
    return n * 5;            /* every byte is five pixels wide */
}

static void test_life(void)
{
    AgentLines a;
    char out[AL_TEXT];
    int i;
    al_init(&a);

    i = al_start(&a, "1 Explore Find tests", 20, out, sizeof(out));
    CHECK_EQ_LONG(i, 0);
    CHECK(strcmp(out, "  " RAQ " Explore: Find tests") == 0);
    CHECK_EQ_LONG(a.s[0].state, AL_OPEN);

    i = al_step(&a, "1 3 Grep foo", 12, out, sizeof(out));
    CHECK_EQ_LONG(i, 0);
    CHECK(strcmp(out, "  " RAQ " Explore: Find tests " MID " Grep " MID " 3 tools") == 0);
    i = al_step(&a, "1 1 Read x", 10, out, sizeof(out));   /* steps never go back */
    CHECK(strstr(out, "3 tools") != NULL);
    CHECK_EQ_LONG(al_step(&a, "7 1 Read x", 10, out, sizeof(out)), -1);

    i = al_end(&a, "1 done 4 Found two", 18, out, sizeof(out));
    CHECK_EQ_LONG(i, 0);
    CHECK(strcmp(out, "  " RAQ " Explore: done " MID " 4 tools " MID " Found two") == 0);
    CHECK_EQ_LONG(a.s[0].state, AL_ENDED);
    CHECK_EQ_LONG(al_end(&a, "1 done 4 again", 14, out, sizeof(out)), -1);
    CHECK_EQ_LONG(al_start(&a, "x Explore y", 11, out, sizeof(out)), -1);
}

static void test_slots(void)
{
    AgentLines a;
    char out[AL_TEXT], pay[40];
    int i, n;
    al_init(&a);
    for (i = 1; i <= AL_SLOTS; i++) {
        n = sprintf(pay, "%d T task%d", i, i);
        CHECK_EQ_LONG(al_start(&a, pay, n, out, sizeof(out)), i - 1);
    }
    /* All open: the next one is plain. */
    CHECK_EQ_LONG(al_start(&a, "20 Plan make a plan", 19, out, sizeof(out)), -2);
    CHECK(strcmp(out, "Agent Plan: make a plan") == 0);
    CHECK_EQ_LONG(al_end(&a, "20 done 2 ok", 12, out, sizeof(out)), -2);
    CHECK(strcmp(out, "Agent Plan done: ok") == 0);
    CHECK_EQ_LONG(al_end(&a, "20 done 2 ok", 12, out, sizeof(out)), -1);

    /* An ended slot is reused, the oldest first. */
    al_end(&a, "3 done 1 x", 10, out, sizeof(out));
    al_end(&a, "2 done 1 x", 10, out, sizeof(out));
    CHECK_EQ_LONG(al_start(&a, "21 U again", 10, out, sizeof(out)), 1);

    /* The turn ends with lines open: each closes as stopped, once. */
    n = 0;
    while (al_stop_next(&a, out, sizeof(out)) >= 0) {
        CHECK(strstr(out, ": stopped") != NULL);
        n++;
    }
    CHECK_EQ_LONG(n, AL_SLOTS - 1);   /* slot 2 (k=3) is still ended */
    CHECK_EQ_LONG(al_stop_next(&a, out, sizeof(out)), -1);
}

static void test_offsets(void)
{
    AgentLines a;
    char out[AL_TEXT];
    al_init(&a);
    al_start(&a, "1 A one", 7, out, sizeof(out));
    al_start(&a, "2 B two", 7, out, sizeof(out));
    a.s[0].drawn = a.s[1].drawn = 1;
    a.s[0].off = 100; a.s[1].off = 300;
    al_shift(&a, 100, 12);
    CHECK_EQ_LONG(a.s[0].off, 100);
    CHECK_EQ_LONG(a.s[1].off, 312);
    al_cut(&a, 200);
    CHECK_EQ_LONG(a.s[0].gone, 1);
    CHECK_EQ_LONG(a.s[1].off, 112);
    CHECK_EQ_LONG(a.s[1].gone, 0);
}

static void test_fit(void)
{
    char s[AL_TEXT] = "  " RAQ " Explore: a long task description";
    long n = (long)strlen(s), r;
    CHECK_EQ_LONG(al_fit(s, n, 1000, measure), n);
    r = al_fit(s, n, 100, measure);       /* 20 bytes */
    CHECK_EQ_LONG(r, 20);
    CHECK_EQ_LONG((unsigned char)s[19], 0xC9);
    CHECK(strncmp(s, "  " RAQ " Explore", 5) == 0);
}

int main(void)
{
    test_life();
    test_slots();
    test_offsets();
    test_fit();
    CHECK_DONE();
}
