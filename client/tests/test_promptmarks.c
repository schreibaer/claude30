/* Prompt positions for Cmd-Up and Cmd-Down. */
#include "check.h"
#include "promptmarks.h"

static void test_find(void)
{
    PromptMarks m;
    pm_init(&m);
    CHECK_EQ_LONG(pm_prev(&m, 100), -1);
    CHECK_EQ_LONG(pm_next(&m, 0), -1);

    pm_add(&m, 10);
    pm_add(&m, 200);
    pm_add(&m, 900);
    pm_add(&m, 900);          /* not after the last: ignored */
    pm_add(&m, 50);
    CHECK_EQ_LONG(m.n, 3);

    CHECK_EQ_LONG(pm_prev(&m, 5), -1);
    CHECK_EQ_LONG(pm_next(&m, 5), 10);
    CHECK_EQ_LONG(pm_prev(&m, 300), 200);
    CHECK_EQ_LONG(pm_next(&m, 300), 900);
    CHECK_EQ_LONG(pm_prev(&m, 1000), 900);
    CHECK_EQ_LONG(pm_next(&m, 1000), -1);
    /* On a prompt: that one is current. */
    CHECK_EQ_LONG(pm_prev(&m, 200), 10);
    CHECK_EQ_LONG(pm_next(&m, 200), 900);
}

static void test_cut(void)
{
    PromptMarks m;
    pm_init(&m);
    pm_add(&m, 10);
    pm_add(&m, 200);
    pm_add(&m, 900);
    pm_cut(&m, 0);
    CHECK_EQ_LONG(m.n, 3);
    pm_cut(&m, 100);
    CHECK_EQ_LONG(m.n, 2);
    CHECK_EQ_LONG(m.off[0], 100);
    CHECK_EQ_LONG(m.off[1], 800);
    pm_cut(&m, 100);          /* a prompt at the cut stays, at 0 */
    CHECK_EQ_LONG(m.n, 2);
    CHECK_EQ_LONG(m.off[0], 0);
    pm_cut(&m, 5000);
    CHECK_EQ_LONG(m.n, 0);
}

static void test_full(void)
{
    PromptMarks m;
    long i;
    pm_init(&m);
    for (i = 0; i < PM_MAX + 10; i++)
        pm_add(&m, i * 10);
    CHECK_EQ_LONG(m.n, PM_MAX);
    CHECK_EQ_LONG(m.off[0], 100);
    CHECK_EQ_LONG(m.off[PM_MAX - 1], (PM_MAX + 9) * 10);
}

static void test_shift(void)
{
    PromptMarks m;
    pm_init(&m);
    pm_add(&m, 10);
    pm_add(&m, 200);
    pm_shift(&m, 50, 7);
    CHECK_EQ_LONG(m.off[0], 10);
    CHECK_EQ_LONG(m.off[1], 207);
    pm_shift(&m, 50, -20);
    CHECK_EQ_LONG(m.off[1], 187);
}

int main(void)
{
    test_shift();
    test_find();
    test_cut();
    test_full();
    CHECK_DONE();
}
