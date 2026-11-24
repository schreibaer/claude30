/* The Claude mascot that walks in the input area while a turn runs: which
   frame, which word and where. In the empty input box it paces between its
   word and a tiny compact Mac, whose screen flips between a Happy Mac and the
   wristwatch cursor. Portable C, host-tested; window.c draws it. */
#ifndef MASCOT_H_INCLUDED
#define MASCOT_H_INCLUDED

#define MASCOT_W 16
#define MASCOT_H 16
#define MASCOT_FRAME_TICKS 8      /* a new frame about 7.5 times a second */
#define MASCOT_WORD_TICKS  180    /* a new word every 3 seconds */
#define MASCOT_WORD_MAX    40

typedef enum { MF_STAND, MF_STEP, MF_BLINK, MF_WAIT, MF_COUNT } MascotFrame;
/* The little Mac's screen. */
typedef enum { PF_HAPPY, PF_WATCH1, PF_WATCH2, PF_COUNT } PropFrame;
#define MASCOT_PROP_TICKS 30      /* the Mac's screen changes twice a second */
typedef enum { MP_HIDDEN, MP_BOX, MP_SPOT } MascotPlace;

/* Frame pictures, MASCOT_H rows of MASCOT_W characters: '#' is black. */
extern const char *const mascot_art[MF_COUNT][MASCOT_H];
extern const char *const mascot_prop_art[PF_COUNT][MASCOT_H];

/* Row r of frame f as 16 bits, leftmost pixel in the high bit. */
unsigned short mascot_row(MascotFrame f, int r);
unsigned short mascot_prop_row(PropFrame f, int r);

typedef struct {
    MascotPlace place;
    MascotFrame frame;
    PropFrame prop;               /* in the box only */
    char word[MASCOT_WORD_MAX];   /* Mac Roman, NUL-terminated; empty in the spot */
} MascotView;

/* What to show at ticks. busy: the front tab's turn runs. waiting: it waits
   for a permission answer. has_text: the input box holds text. tool: the
   tool running now, or NULL or "". */
void mascot_view(unsigned long ticks, int busy, int waiting, int has_text,
                 const char *tool, MascotView *out);

/* Left offset of the walking mascot inside room pixels of free width: it
   walks two pixels per frame and turns at both ends. 0 when room <= 0. */
int mascot_walk_x(unsigned long ticks, int room);

#endif
