/* Session protocol core: modem dialing, the app-mode handshake, tag parsing,
   flow control, permission requests, stopping and disconnecting.

   Portable C with no Toolbox calls. The Mac shell feeds it received bytes and
   the current time in ticks (60 per second); the core answers with events
   for the window and with bytes to send. All protocol timing lives here, so
   it can be tested on the host. */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "linebuf.h"

#define TICKS_PER_SEC 60L

typedef enum {
    ST_IDLE,        /* no call */
    ST_DIALING,     /* modem in command mode: AT, then the dial string */
    ST_HANDSHAKE,   /* connected; waiting for the bridge to confirm app mode */
    ST_READY,       /* bridge waits for a prompt */
    ST_BUSY,        /* a turn is running */
    ST_PERMISSION,  /* the bridge waits for an answer to a permission request */
    ST_CLOSING,     /* disconnecting */
    ST_HANGUP       /* sending +++ / ATH to put the modem on hook */
} SessState;

typedef enum {
    EV_STATE,        /* state changed; text is empty */
    EV_TEXT,         /* new prose paragraph (may be empty: a blank line) */
    EV_TEXT_CONT,    /* continuation of the last paragraph; join with one space */
    EV_CODE_BEGIN,
    EV_CODE,         /* one code line */
    EV_CODE_CONT,    /* continuation of the last code line; join with nothing */
    EV_CODE_END,
    EV_TOOL,         /* "Tool summary" */
    EV_ERROR,
    EV_INFO,
    EV_RESULT,       /* "status cost" */
    EV_INTERRUPTED,  /* the user stopped this turn */
    EV_READY,        /* turn finished; input may be sent */
    EV_PERM_BEGIN,   /* "Tool summary" of a new request */
    EV_PERM_LINE,    /* one body line */
    EV_PERM_ASK,     /* request complete: show the dialog */
    EV_PERM_CLOSE,   /* close the dialog without answering (link lost) */
    EV_PERM_ABANDONED, /* transcript note: the request was never answered */
    EV_UNKNOWN,      /* a line the core does not recognise (maybe line noise) */
    EV_CONNECTED,    /* handshake done */
    EV_DIAL_FAILED,  /* text: what went wrong, for an alert */
    EV_LOST,         /* the carrier dropped during a session */
    EV_CLOSED,       /* a disconnect the user asked for has finished */
    EV_SILENCE       /* no input for the warning time during a turn */
} EventKind;

typedef struct Proto Proto;
typedef void (*ProtoEventFn)(void *ctx, EventKind kind, const char *text, long len);
typedef void (*ProtoSendFn)(void *ctx, const char *data, long len);

struct Proto {
    SessState state;
    ProtoEventFn on_event;
    ProtoSendFn send;
    void *ctx;
    LineBuf lb;

    unsigned long now;           /* last tick seen */
    unsigned long timer;         /* deadline for the current wait, 0 = none */
    unsigned long last_input;    /* tick of the last received byte */

    char dial[64];               /* dial string, sent verbatim */
    int dial_phase;              /* 0 waiting for OK to AT, 1 waiting for CONNECT */
    int at_retry;                /* AT got no answer once; hung up and retried */
    int got_ok;                  /* handshake: #OK seen, waiting for > */

    int swallow;                 /* ready markers still due for continuation lines */
    int last_kind;               /* 'T' or 'C': what a + line continues */
    int in_code;
    unsigned pending_acks;       /* W lines received but not yet acknowledged */

    char perm_id[16];
    int perm_open;               /* Q seen, waiting for ? */
    int perm_asked;              /* ? seen, no answer sent yet */
    int deny_on_ask;             /* answer n as soon as ? arrives */

    int stopped;                 /* !STOP sent during this turn */
    int noise;                   /* serial errors seen during this turn */
    int spare_ack_sent;
    int silence_warned;

    int closing_turn;            /* disconnecting: waiting for the turn to end */
    int quit_sent;               /* /quit sent: a drop now is normal */
    int hangup_step;             /* 0 guard, 1 after +++, 2 after ATH */
    int after_hangup;            /* what to do when the hangup finishes */
    char fail_msg[80];           /* alert text for a failed call */
};

/* Times, in ticks. */
#define T_AT          (3 * TICKS_PER_SEC)
#define T_DIAL        (60 * TICKS_PER_SEC)
#define T_HANDSHAKE   (60 * TICKS_PER_SEC)
#define T_QUIT        (3 * TICKS_PER_SEC)
#define T_STOP_TURN   (10 * TICKS_PER_SEC)
#define T_GUARD       (72)          /* 1.2 s */
#define T_ATH         (3 * TICKS_PER_SEC)
#define T_SILENCE     (90 * TICKS_PER_SEC)

void proto_init(Proto *p, ProtoEventFn ev, ProtoSendFn send, void *ctx);
void proto_free(Proto *p);

/* Starts a call: AT, then the dial string. */
void proto_connect(Proto *p, const char *dial, unsigned long now);
/* Disconnect or Quit, from any state. */
void proto_disconnect(Proto *p, unsigned long now);

void proto_feed(Proto *p, const char *data, long n, unsigned long now);
void proto_tick(Proto *p, unsigned long now);

/* Sends a prompt (text as typed, CR line breaks). Only in ST_READY.
   Returns 0 if it was not sent. */
int proto_send_prompt(Proto *p, const char *text, long len);
/* Cmd-period or Stop. */
void proto_stop(Proto *p);
/* Answers the open permission request: 'y', 'n' or 'a'. */
void proto_answer(Proto *p, char answer);
/* The shell has drawn everything received so far: acknowledge each W. */
void proto_drawn(Proto *p);
/* The serial driver reported lost bytes. */
void proto_line_noise(Proto *p);

/* Seconds without input during a turn, for the status line (0 if not busy). */
long proto_silence_secs(const Proto *p);
int proto_online(const Proto *p);

#endif
