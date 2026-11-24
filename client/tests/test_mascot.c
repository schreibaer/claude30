/* The mascot: frames, words and walk. */
#include "check.h"
#include "mascot.h"

static void test_art(void)
{
    int f, r;
    for (f = 0; f < MF_COUNT; f++)
        for (r = 0; r < MASCOT_H; r++)
            CHECK_EQ_LONG(strlen(mascot_art[f][r]), MASCOT_W);
    CHECK_EQ_LONG(mascot_row(MF_STAND, 7), 0xFFFF);      /* arms */
    CHECK_EQ_LONG(mascot_row(MF_STAND, 0), 0);
    CHECK_EQ_LONG(mascot_row(MF_STAND, 5), 0x3BDC);      /* ..###.####.###.. */
    CHECK_EQ_LONG(mascot_row(MF_COUNT, 0), 0);
    for (f = 0; f < PF_COUNT; f++)
        for (r = 0; r < MASCOT_H; r++)
            CHECK_EQ_LONG(strlen(mascot_prop_art[f][r]), MASCOT_W);
    CHECK_EQ_LONG(mascot_prop_row(PF_HAPPY, 13), 0x7FFC);
    CHECK_EQ_LONG(mascot_prop_row(PF_COUNT, 0), 0);
}

static void test_view(void)
{
    MascotView v;
    unsigned long t;

    mascot_view(1000, 0, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.place, MP_HIDDEN);

    mascot_view(0, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.place, MP_BOX);
    CHECK_EQ_LONG(v.frame, MF_STAND);
    CHECK(strcmp(v.word, "Pondering\xC9") == 0);

    mascot_view(0, 1, 0, 1, NULL, &v);
    CHECK_EQ_LONG(v.place, MP_SPOT);
    CHECK_EQ_LONG(v.word[0], 0);

    for (t = 0; t < 2000; t += 37) {
        mascot_view(t, 1, 1, 0, "Bash", &v);
        CHECK_EQ_LONG(v.frame, MF_WAIT);
        CHECK(strcmp(v.word, "Waiting for you") == 0);
    }

    mascot_view(5, 1, 0, 0, "Bash", &v);
    CHECK(strcmp(v.word, "Running Bash\xC9") == 0);
    mascot_view(5, 1, 0, 0, "", &v);
    CHECK(strcmp(v.word, "Pondering\xC9") == 0);
    mascot_view(5, 1, 0, 0, "mcp__a_very_long_tool_name_indeed", &v);
    CHECK(strlen(v.word) < MASCOT_WORD_MAX);
    CHECK(strcmp(v.word, "Running mcp__a_very_long_tool_n\xC9") == 0);

    /* A new frame every 8 ticks, a new word every 180, always the same. */
    mascot_view(7, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.frame, MF_STAND);
    mascot_view(8, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.frame, MF_STEP);
    mascot_view(23 * 8, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.frame, MF_BLINK);
    mascot_view(179, 1, 0, 0, NULL, &v);
    CHECK(strcmp(v.word, "Pondering\xC9") == 0);
    mascot_view(180, 1, 0, 0, NULL, &v);
    CHECK(strcmp(v.word, "Warming up the 68030\xC9") == 0);
    mascot_view(180 * 20, 1, 0, 0, NULL, &v);
    CHECK(strcmp(v.word, "Pondering\xC9") == 0);
}

static void test_prop(void)
{
    MascotView v;
    mascot_view(0, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.prop, PF_HAPPY);
    mascot_view(MASCOT_PROP_TICKS, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.prop, PF_WATCH1);
    mascot_view(2 * MASCOT_PROP_TICKS, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.prop, PF_WATCH2);
    mascot_view(4 * MASCOT_PROP_TICKS, 1, 0, 0, NULL, &v);
    CHECK_EQ_LONG(v.prop, PF_HAPPY);
    mascot_view(MASCOT_PROP_TICKS, 1, 1, 0, NULL, &v);
    CHECK_EQ_LONG(v.prop, PF_HAPPY);      /* waiting: the Mac smiles */
}

static void test_walk(void)
{
    unsigned long t;
    int x, last = 0, turned = 0;
    CHECK_EQ_LONG(mascot_walk_x(1000, 0), 0);
    CHECK_EQ_LONG(mascot_walk_x(1000, -5), 0);
    CHECK_EQ_LONG(mascot_walk_x(0, 100), 0);
    CHECK_EQ_LONG(mascot_walk_x(8, 100), 2);
    for (t = 0; t < 8 * 400; t += 8) {
        x = mascot_walk_x(t, 100);
        CHECK(x >= 0 && x <= 100);
        if (t && x < last)
            turned = 1;
        last = x;
    }
    CHECK(turned);
    CHECK_EQ_LONG(mascot_walk_x(8 * 50, 100), 100);   /* the far end */
    CHECK_EQ_LONG(mascot_walk_x(8 * 51, 100), 98);    /* and back */
    CHECK_EQ_LONG(mascot_walk_x(8 * 100, 100), 0);
}

int main(void)
{
    test_art();
    test_view();
    test_prop();
    test_walk();
    CHECK_DONE();
}
