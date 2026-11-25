/* The heartbeat light: which picture, and the art. */
#include "check.h"
#include "heartbeat.h"
#include <limits.h>

static HeartInput online(unsigned long heard)
{
    HeartInput in;
    memset(&in, 0, sizeof(in));
    in.link = HL_ONLINE;
    in.ping_cap = 1;
    in.last_heard = heard;
    return in;
}

static int diff(HeartPic a, HeartPic b)
{
    int r, bits = 0;
    for (r = 0; r < HB_H; r++) {
        unsigned short x = (unsigned short)(heartbeat_row(a, r) ^ heartbeat_row(b, r));
        while (x) {
            bits += x & 1;
            x >>= 1;
        }
    }
    return bits;
}

static void test_art(void)
{
    int p, r;
    for (p = 0; p < HP_COUNT; p++)
        for (r = 0; r < HB_H; r++)
            CHECK_EQ_LONG(strlen(heartbeat_art[p][r]), HB_W);
    CHECK_EQ_LONG(heartbeat_row(HP_BLANK, 0), 0x3FFC);   /* ..############.. */
    CHECK_EQ_LONG(heartbeat_row(HP_COUNT, 0), 0);
    CHECK_EQ_LONG(heartbeat_row(HP_BLANK, HB_H), 0);
    /* Every picture differs from every other, and the three hearts by 8+. */
    for (p = 0; p < HP_COUNT; p++)
        for (r = p + 1; r < HP_COUNT; r++)
            CHECK(diff((HeartPic)p, (HeartPic)r) > 0);
    CHECK(diff(HP_HEART_SMALL, HP_HEART_BIG) >= 8);
    CHECK(diff(HP_HEART_SMALL, HP_HEART_HOLLOW) >= 8);
    CHECK(diff(HP_HEART_BIG, HP_HEART_HOLLOW) >= 8);
}

static void test_states(void)
{
    Heartbeat hb;
    HeartInput in = online(100);
    memset(&hb, 0, sizeof(hb));

    in.link = HL_OFF;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 10), HP_BLANK);
    in.redial = 1;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 10), HP_SAD);
    in.link = HL_DIALING;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 10), HP_WATCH);   /* redialing */
    in.redial = 0;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 10), HP_WATCH);
}

static void test_beats(void)
{
    Heartbeat hb;
    HeartInput in = online(1000);
    unsigned long t, last_start = 0;
    int starts = 0;
    HeartPic prev = HP_HEART_SMALL;
    memset(&hb, 0, sizeof(hb));

    /* The first look after going online is a beat (the handshake arrived),
       then a small heart. */
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000), HP_HEART_BIG);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + HB_BEAT_TICKS - 1), HP_HEART_BIG);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + HB_BEAT_TICKS), HP_HEART_SMALL);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + 500), HP_HEART_SMALL);

    /* A line arriving: a beat for 12 ticks. */
    in.last_heard = 2000;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 2000), HP_HEART_BIG);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 2011), HP_HEART_BIG);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 2012), HP_HEART_SMALL);

    /* A line every 5 ticks: beats start no closer than 30 ticks apart. */
    for (t = 3000; t < 3600; t++) {
        HeartPic pic;
        if (t % 5 == 0)
            in.last_heard = t;
        pic = heartbeat_pic(&hb, &in, t);
        if (pic == HP_HEART_BIG && prev != HP_HEART_BIG) {
            if (starts)
                CHECK(t - last_start >= HB_GAP_TICKS);
            last_start = t;
            starts++;
        }
        prev = pic;
    }
    CHECK(starts >= 15 && starts <= 20);       /* about two a second */

    /* A line heard inside the gap beats as soon as the gap is over. */
    memset(&hb, 0, sizeof(hb));
    in = online(5000);
    heartbeat_pic(&hb, &in, 5000);             /* first beat */
    in.last_heard = 5005;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 5020), HP_HEART_SMALL);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 5030), HP_HEART_BIG);
}

static void test_hollow(void)
{
    Heartbeat hb;
    HeartInput in = online(1000);
    memset(&hb, 0, sizeof(hb));
    heartbeat_pic(&hb, &in, 1000);

    in.ping_out = 1;
    in.last_ping = 1000;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + 4 * 60), HP_HEART_SMALL);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + 5 * 60), HP_HEART_HOLLOW);
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + 60 * 60), HP_HEART_HOLLOW);

    /* Anything heard since the ping ends it, even before a #PONG. */
    in.last_heard = 1000 + 6 * 60;
    CHECK_EQ_LONG(heartbeat_pic(&hb, &in, 1000 + 6 * 60), HP_HEART_BIG);
    CHECK(heartbeat_pic(&hb, &in, 1000 + 7 * 60) != HP_HEART_HOLLOW);

    /* A bridge that cannot answer heartbeats never goes hollow. */
    memset(&hb, 0, sizeof(hb));
    in = online(1000);
    in.ping_cap = 0;
    heartbeat_pic(&hb, &in, 1000);
    CHECK(heartbeat_pic(&hb, &in, 1000 + 100000) != HP_HEART_HOLLOW);
    in.ping_cap = 1;
    in.ping_out = 0;
    CHECK(heartbeat_pic(&hb, &in, 1000 + 100000) != HP_HEART_HOLLOW);
}

static void test_wrap(void)
{
    /* The tick counter passing ULONG_MAX does not freeze or flash the heart. */
    Heartbeat hb;
    unsigned long t, base = ULONG_MAX - 100;
    HeartInput in = online(base);
    int bigs = 0, hollows = 0;
    memset(&hb, 0, sizeof(hb));
    heartbeat_pic(&hb, &in, base);
    in.ping_out = 1;
    in.last_ping = base + 50;           /* wraps to a small number */
    for (t = base + 51; t != base + 400; t++) {
        HeartPic pic;
        if (t == base + 150)
            in.last_heard = t;          /* heard just after the wrap */
        pic = heartbeat_pic(&hb, &in, t);
        if (pic == HP_HEART_BIG)
            bigs++;
        if (pic == HP_HEART_HOLLOW)
            hollows++;
    }
    CHECK_EQ_LONG(bigs, HB_BEAT_TICKS);   /* exactly the one beat after the first, which began earlier */
    CHECK(hollows == 0);                  /* heard after the ping: not hollow */
}

int main(void)
{
    test_art();
    test_states();
    test_beats();
    test_hollow();
    test_wrap();
    CHECK_DONE();
}
