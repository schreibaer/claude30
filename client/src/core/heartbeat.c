#include "heartbeat.h"

/* One compact Mac, with the screen contents changing: a heart (small, big
   for the beat, and hollow), the wristwatch, a Sad Mac, or nothing. */
const char *const heartbeat_art[HP_COUNT][HB_H] = {
    {   /* blank screen */
        "..############..",
        ".#.##########.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." },
    {   /* wristwatch */
        "..############..",
        ".#.##########.#.",
        ".#.#.######.#.#.",
        ".#.##......##.#.",
        ".#.##...#..##.#.",
        ".#.##...#..##.#.",
        ".#.##...###.#.#.",
        ".#.##......##.#.",
        ".#.##......##.#.",
        ".#.#.######.#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." },
    {   /* small heart */
        "..############..",
        ".#.##########.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#.##..##.#.#.",
        ".#.#.######.#.#.",
        ".#.#.######.#.#.",
        ".#.#..####..#.#.",
        ".#.#...##...#.#.",
        ".#.#........#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." },
    {   /* big heart (a beat) */
        "..############..",
        ".#.##########.#.",
        ".#.#........#.#.",
        ".#.#.##..##.#.#.",
        ".#.##########.#.",
        ".#.##########.#.",
        ".#.##########.#.",
        ".#.#.######.#.#.",
        ".#.#..####..#.#.",
        ".#.#...##...#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." },
    {   /* hollow heart */
        "..############..",
        ".#.##########.#.",
        ".#.#........#.#.",
        ".#.#........#.#.",
        ".#.#.##..##.#.#.",
        ".#.#.#....#.#.#.",
        ".#.#.#....#.#.#.",
        ".#.#..#..#..#.#.",
        ".#.#...##...#.#.",
        ".#.#........#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." },
    {   /* Sad Mac */
        "..############..",
        ".#.##########.#.",
        ".#.#........#.#.",
        ".#.##.#..#.##.#.",
        ".#.#.#....#.#.#.",
        ".#.##.#..#.##.#.",
        ".#.#........#.#.",
        ".#.#..####..#.#.",
        ".#.#.#....#.#.#.",
        ".#.#........#.#.",
        ".#.##########.#.",
        ".#............#.",
        ".#...######...#.",
        ".#............#.",
        "..############..",
        "....########...." }
};

unsigned short heartbeat_row(HeartPic pic, int r)
{
    unsigned short bits = 0;
    const char *s;
    int i;
    if ((int)pic < 0 || pic >= HP_COUNT || r < 0 || r >= HB_H)
        return 0;
    s = heartbeat_art[pic][r];
    for (i = 0; i < HB_W && s[i]; i++)
        if (s[i] == '#')
            bits |= (unsigned short)(0x8000u >> i);
    return bits;
}

/* a is after b, in ticks that may have wrapped. */
static int after(unsigned long a, unsigned long b)
{
    return (long)(a - b) > 0;
}

HeartPic heartbeat_pic(Heartbeat *hb, const HeartInput *in, unsigned long now)
{
    if (in->link == HL_DIALING)
        return HP_WATCH;
    if (in->link == HL_OFF) {
        hb->have_beat = 0;
        return in->redial ? HP_SAD : HP_BLANK;
    }
    /* Online. A line heard since the last beat starts a new one, but not
       sooner than HB_GAP_TICKS after the last: a busy stream pulses. */
    if (!hb->have_beat) {
        hb->have_beat = 1;
        hb->beat_at = now;
        hb->seen = in->last_heard;
    } else if (in->last_heard != hb->seen
               && now - hb->beat_at >= (unsigned long)HB_GAP_TICKS) {
        hb->beat_at = now;
        hb->seen = in->last_heard;
    }
    /* Hollow: a heartbeat has waited too long and nothing at all arrived
       since it was sent. A bridge that cannot answer one never goes hollow. */
    if (in->ping_cap && in->ping_out && now - in->last_ping >= (unsigned long)HB_WAIT_TICKS
            && !after(in->last_heard, in->last_ping))
        return HP_HEART_HOLLOW;
    return now - hb->beat_at < (unsigned long)HB_BEAT_TICKS ? HP_HEART_BIG : HP_HEART_SMALL;
}
