/* The heartbeat light in the status line: a 16x16 compact Mac whose screen
   shows how the link to the host is doing. Online, a heart beats whenever
   something arrives from the bridge and turns hollow when a heartbeat goes
   unanswered; while dialing the screen shows the wristwatch; while a lost
   link waits to be redialed, a Sad Mac; otherwise the screen is blank.
   Portable C with no Toolbox calls, host-tested; window.c draws it. */
#ifndef HEARTBEAT_H_INCLUDED
#define HEARTBEAT_H_INCLUDED

#define HB_W 16
#define HB_H 16
#define HB_BEAT_TICKS 12          /* the big heart shows this long */
#define HB_GAP_TICKS  30          /* beats start at least this far apart (2 a second) */
#define HB_WAIT_TICKS (5 * 60)    /* a heartbeat unanswered this long: hollow */

typedef enum {
    HP_BLANK, HP_WATCH, HP_HEART_SMALL, HP_HEART_BIG, HP_HEART_HOLLOW, HP_SAD,
    HP_COUNT
} HeartPic;

typedef enum {
    HL_OFF,       /* no call (hanging up counts as off) */
    HL_DIALING,   /* dialing, or waiting for the bridge's handshake */
    HL_ONLINE
} HeartLink;

/* What the shell reads from the protocol core and the modem. */
typedef struct {
    HeartLink link;
    int redial;                   /* a lost link is waiting to be redialed */
    unsigned long last_heard;     /* Proto.last_heard */
    int ping_cap;                 /* Proto.ping_cap */
    int ping_out;                 /* Proto.ping_out */
    unsigned long last_ping;      /* Proto.last_ping */
} HeartInput;

/* The caller's state between calls; zero it to start. */
typedef struct {
    unsigned long beat_at;        /* tick the latest beat began */
    unsigned long seen;           /* last_heard that beat started */
    int have_beat;
} Heartbeat;

/* The picture to show at tick now. Call it often (a few times a second). */
HeartPic heartbeat_pic(Heartbeat *hb, const HeartInput *in, unsigned long now);

/* Picture art, HB_H rows of HB_W characters: '#' is black. */
extern const char *const heartbeat_art[HP_COUNT][HB_H];
/* Row r of a picture as 16 bits, leftmost pixel in the high bit. */
unsigned short heartbeat_row(HeartPic pic, int r);

#endif
