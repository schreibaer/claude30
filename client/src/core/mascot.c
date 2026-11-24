#include "mascot.h"
#include <stdio.h>
#include <string.h>

/* A wide body with two eye holes, stubby arms out the sides and four short
   legs. Step bobs the body up a pixel; wait raises the arms. */
const char *const mascot_art[MF_COUNT][MASCOT_H] = {
    {   /* stand */
        "................",
        "................",
        "................",
        "..############..",
        "..############..",
        "..###.####.###..",
        "..###.####.###..",
        "################",
        "################",
        "..############..",
        "..############..",
        "..############..",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "................" },
    {   /* step */
        "................",
        "................",
        "..############..",
        "..############..",
        "..###.####.###..",
        "..###.####.###..",
        "################",
        "################",
        "..############..",
        "..############..",
        "..############..",
        "...#.#....#.#...",
        "..#...#..#...#..",
        "..#...#..#...#..",
        "................",
        "................" },
    {   /* blink */
        "................",
        "................",
        "................",
        "..############..",
        "..############..",
        "..############..",
        "..###.####.###..",
        "################",
        "################",
        "..############..",
        "..############..",
        "..############..",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "................" },
    {   /* wait: arms up */
        "................",
        "##............##",
        "##............##",
        "###############.",
        ".##############.",
        "..###.####.###..",
        "..###.####.###..",
        "..############..",
        "..############..",
        "..############..",
        "..############..",
        "..############..",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "...#.#....#.#...",
        "................" }
};

unsigned short mascot_row(MascotFrame f, int r)
{
    unsigned short bits = 0;
    const char *s;
    int i;
    if (f < 0 || f >= MF_COUNT || r < 0 || r >= MASCOT_H)
        return 0;
    s = mascot_art[f][r];
    for (i = 0; i < MASCOT_W && s[i]; i++)
        if (s[i] == '#')
            bits |= (unsigned short)(0x8000u >> i);
    return bits;
}

static const char *const kWords[] = {
    "Pondering", "Noodling", "Tinkering", "Scheming", "Musing",
    "Percolating", "Clauding", "Puttering", "Mulling", "Brewing"
};
#define NWORDS ((unsigned long)(sizeof(kWords) / sizeof(kWords[0])))
#define ELLIPSIS "\xC9"            /* Mac Roman */
#define TOOL_MAX 24                /* longer tool names are cut */

void mascot_view(unsigned long ticks, int busy, int waiting, int has_text,
                 const char *tool, MascotView *out)
{
    unsigned long n = ticks / MASCOT_FRAME_TICKS;
    out->word[0] = 0;
    if (!busy && !waiting) {
        out->place = MP_HIDDEN;
        out->frame = MF_STAND;
        return;
    }
    out->place = has_text ? MP_SPOT : MP_BOX;
    if (waiting)
        out->frame = MF_WAIT;
    else if (n % 24 == 23)
        out->frame = MF_BLINK;
    else
        out->frame = (n & 1) ? MF_STEP : MF_STAND;
    if (out->place == MP_SPOT)
        return;
    if (waiting) {
        strcpy(out->word, "Waiting for you");
    } else if (tool && tool[0]) {
        size_t k = strlen(tool);
        if (k > TOOL_MAX)
            snprintf(out->word, sizeof(out->word), "Running %.*s" ELLIPSIS, TOOL_MAX - 1, tool);
        else
            snprintf(out->word, sizeof(out->word), "Running %s" ELLIPSIS, tool);
    } else {
        snprintf(out->word, sizeof(out->word), "%s" ELLIPSIS,
                 kWords[(ticks / MASCOT_WORD_TICKS) % NWORDS]);
    }
}

int mascot_walk_x(unsigned long ticks, int room)
{
    unsigned long pos, span;
    if (room <= 0)
        return 0;
    span = (unsigned long)room;
    pos = (ticks / MASCOT_FRAME_TICKS * 2) % (2 * span);
    return (int)(pos <= span ? pos : 2 * span - pos);
}
