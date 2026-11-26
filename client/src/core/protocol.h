/* Protocol core: modem dialing, the version 2 handshake and project list,
   up to six sessions over one call (prompts, permission requests, stopping,
   opening and closing), flow control, disconnecting, and file transfers
   (files.h).

   Portable C with no Toolbox calls. The Mac shell feeds it received bytes and
   the current time in ticks (60 per second); the core answers with events
   for the window and with bytes to send. Every event carries a session id:
   1 to 6 for a session, 0 for the link. All protocol timing lives here, so
   it can be tested on the host. */
#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "linebuf.h"

#define TICKS_PER_SEC 60L
#define MAX_SESS      6
#define MAX_XFER      8
#define MAX_PROJECTS  128
#define MAX_MODELS    16
#define MODEL_LEN     32   /* a model name, with its NUL */

typedef enum {
    ST_IDLE,        /* no call */
    ST_DIALING,     /* modem in command mode: AT, then the dial string */
    ST_HANDSHAKE,   /* connected; waiting for #OK and the project list */
    ST_ONLINE,      /* the bridge is talking v2; sessions may be open */
    ST_CLOSING,     /* disconnecting: stopping turns, then !QUIT */
    ST_HANGUP       /* sending +++ / ATH to put the modem on hook */
} LinkState;

typedef enum {
    SS_NONE,        /* no session with this id */
    SS_OPENING,     /* <n>O sent, waiting for <n>O and the first <n>> */
    SS_READY,       /* the bridge waits for a prompt */
    SS_BUSY,        /* a turn is running */
    SS_PERMISSION,  /* the bridge waits for an answer to a permission request */
    SS_CLOSING      /* <n>X sent, waiting for <n>X closed */
} SessState;

typedef enum {
    EV_STATE,        /* link (id 0) or session state changed; text is empty */
    EV_TEXT,         /* new prose paragraph (may be empty: a blank line) */
    EV_TEXT_CONT,    /* continuation of the last paragraph; join with one space */
    EV_CODE_BEGIN,
    EV_CODE,         /* one code line */
    EV_CODE_CONT,    /* continuation of the last code line; join with nothing */
    EV_CODE_END,
    EV_TOOL,         /* "Tool summary" */
    EV_ERROR,
    EV_INFO,         /* id 0: information about the link */
    EV_RESULT,       /* "status cost" */
    EV_INTERRUPTED,  /* the user stopped this turn */
    EV_READY,        /* turn finished; input may be sent */
    EV_PERM_BEGIN,   /* "Tool summary" of a new request */
    EV_PERM_LINE,    /* one body line */
    EV_PERM_ASK,     /* request complete: show the dialog */
    EV_PERM_CLOSE,   /* close the dialog without answering (link lost) */
    EV_PERM_ABANDONED, /* transcript note: the request was never answered */
    EV_OPENED,       /* the session is open; text is the project name */
    EV_OPEN_FAILED,  /* the bridge refused to open it; text is the reason */
    EV_SESSION_CLOSED, /* the session ended; text is "closed" or a reason */
    EV_CONNECTED,    /* id 0: handshake done, project list received */
    EV_PROJECTS,     /* id 0: the project list was received again */
    EV_DIAL_FAILED,  /* id 0: text says what went wrong, for an alert */
    EV_LOST,         /* id 0: the carrier dropped during a call */
    EV_CLOSED,       /* id 0: a disconnect the user asked for has finished */
    EV_SILENCE,      /* id 0: no input for the warning time during a turn */
    EV_RESUMED,      /* a session's conversation: text is "<new|resumed|cut> <token>" */
    EV_AGENT_START,  /* a subagent started: the J+ payload "<k> <type> <task>" */
    EV_AGENT_STEP,   /* progress: the J~ payload "<k> <steps> <tool> <summary>" */
    EV_AGENT_END,    /* ended: the J- payload "<k> <status> <steps> <summary>", or
                        "* stopped" when the turn ended with subagent lines open */
    EV_MODEL,        /* the model and effort a session runs with (or will from
                        its next turn): text is "<model|-> <effort|->" */
    EV_USAGE,        /* the conversation's context and cost: the $ payload
                        "<used> <window> <cost>" (usage.h parses it) */
    /* Files (files.h). During these, proto_xfer() names the transfer. */
    EV_LIST_BEGIN,   /* text is the folder's path */
    EV_LIST_ENTRY,   /* text is "d|f <size> <name>": see files_parse_entry */
    EV_LIST_END,     /* text is "cut" when the listing was cut short */
    EV_LIST_FAILED,  /* text is the reason */
    EV_PIECE,        /* a read piece passed its checks; text is its lines */
    EV_READ_FAILED,  /* text is the reason */
    EV_SAVE_PROGRESS,/* a block was accepted */
    EV_SAVED,        /* text is the file's new version token */
    EV_SAVE_CONFLICT,/* the file changed on the host: force or cancel */
    EV_SAVE_FAILED,  /* text is the reason */
    EV_COUNT
} EventKind;

typedef struct Proto Proto;
typedef void (*ProtoEventFn)(void *ctx, int sid, EventKind kind, const char *text, long len);
typedef void (*ProtoSendFn)(void *ctx, const char *data, long len);

typedef struct {
    SessState state;
    int last_kind;               /* 'T', 'C' or 'B': what a + line continues */
    int in_code;
    char perm_id[16];
    int perm_open;               /* Q seen, waiting for ? */
    int perm_asked;              /* ? seen, no answer sent yet */
    int agents_open;             /* subagent lines started (J+) and not yet ended (J-) */
    int deny_on_ask;             /* answer n as soon as ? arrives */
    int stopped;                 /* !STOP sent during this turn */
    int closing_turn;            /* disconnecting: waiting for this turn to end */
    unsigned long close_by;      /* SS_CLOSING: give up waiting at this tick */
    int rx;                      /* transfer whose piece is arriving, or -1 */
    int listing;                 /* a listing was asked for */
    unsigned long list_by;       /* ... and should have ended by this tick */
} Session;

typedef enum { XF_NONE, XF_READ, XF_SAVE } XferKind;

typedef struct {
    XferKind kind;
    int sid;
    long owner;                  /* the shell's own id for it */
    long num;                    /* transfer number on the wire */
    char path[256];
    int phase;
    int tries;
    unsigned long due;           /* tick by which the next answer is due */
    /* reads */
    long first, count, budget;   /* asked for */
    long h_first, h_count, h_total; /* as the bridge answered */
    char token[24];              /* version token (reads, and saves when done) */
    char mode[24];               /* "e" or "r:<reason>" */
    char *buf;                   /* the piece's lines joined with CR */
    long len, cap;
    long lines_got;
    unsigned long noise_mark;    /* serial errors before the piece */
    /* saves */
    char *text;                  /* a copy of the whole text */
    long tlen;
    long pos, end;               /* the block being sent: [pos, end) */
    int seq, nblocks;
    int force;
} FileXfer;

enum { FILES_OFF, FILES_RO, FILES_RW };

struct Proto {
    LinkState state;
    ProtoEventFn on_event;
    ProtoSendFn send;
    void *ctx;
    LineBuf lb;

    unsigned long now;           /* last tick seen */
    unsigned long timer;         /* deadline for the current link wait, 0 = none */
    unsigned long last_input;    /* tick of the last received byte */

    char dial[64];               /* dial string, sent verbatim */
    int dial_phase;              /* 0 waiting for OK to AT, 1 waiting for CONNECT */
    int at_retry;                /* AT got no answer once; hung up and retried */
    int got_ok;                  /* handshake: #OK seen, waiting for P] */
    int files;                   /* FILES_OFF, FILES_RO or FILES_RW */
    int resume_cap;              /* the bridge resumes conversations (<n>Y) */
    int ping_cap;                /* the bridge answers !PING and times out silent apps */
    int agents_cap;              /* the bridge sends J lines to an app that sent !AGENTS */
    int chat_cap;                /* a session can be opened as a chat with the name "*" */
    int models_cap;              /* the bridge sends a model list and takes <n>M */
    int stream_cap;              /* the bridge takes <n>V on|off and can stream answers */
    int md_cap;                  /* the bridge leaves ** and __ in prose for an app that sent !MD */
    int usage_cap;               /* the bridge sends $ lines to an app that sent !USAGE */

    /* The models a session may pick, from the handshake's M lines. */
    char models[MAX_MODELS][MODEL_LEN];
    int nmodels;
    char def_model[MODEL_LEN];   /* what "bridge default" means; empty: the CLI decides */
    char def_effort[MODEL_LEN];

    unsigned pending_acks;       /* W lines received but not yet acknowledged */
    int noise;                   /* serial errors since the last intact W */
    unsigned long noise_count;   /* serial error reports, ever */
    long dropped;                /* lines for no open session; only grows */
    int spare_ack_sent;
    int silence_warned;
    unsigned long last_heard;    /* tick of the last byte of any kind, #PONG included */
    unsigned long last_tx;       /* tick of the last line we sent */
    unsigned long last_ping;     /* tick of the last !PING */
    int ping_out;                /* a !PING has not been answered yet */
    unsigned long prev_input;    /* last_input before the bytes being fed */
    int feed_real;               /* the bytes being fed held more than #PONG */
    int feed_pong;               /* ... and a #PONG */

    Session sess[MAX_SESS + 1];  /* index 1..6 */

    /* The project list, names back to back with NUL after each. */
    char *proj;
    long proj_len, proj_cap;
    int nproj;
    long proj_off[MAX_PROJECTS];
    int proj_complete;

    FileXfer xfer[MAX_XFER];
    long next_num;
    int cur_xfer;                /* the transfer an event is about, or -1 */

    int quit_sent;               /* !QUIT sent: a drop now is normal */
    int hangup_step;             /* 0 guard, 1 after +++, 2 after ATH */
    int after_hangup;            /* what to do when the hangup finishes */
    char fail_msg[96];           /* alert text for a failed call */
};

/* Times, in ticks. */
#define T_AT          (3 * TICKS_PER_SEC)
#define T_DIAL        (60 * TICKS_PER_SEC)
#define T_HANDSHAKE   (60 * TICKS_PER_SEC)
#define T_QUIT        (3 * TICKS_PER_SEC)
#define T_STOP_TURN   (10 * TICKS_PER_SEC)
#define T_CLOSE       (10 * TICKS_PER_SEC)
#define T_GUARD       (72)          /* 1.2 s */
#define T_ATH         (3 * TICKS_PER_SEC)
#define T_SILENCE     (90 * TICKS_PER_SEC)
#define T_FILE        (20 * TICKS_PER_SEC)
#define T_PING        (30 * TICKS_PER_SEC)   /* heartbeat when nothing else was sent */
#define T_PING_WAIT   (10 * TICKS_PER_SEC)   /* a !PING unanswered this long counts */
#define T_DEAD        (90 * TICKS_PER_SEC)   /* nothing heard this long: the link is dead */

void proto_init(Proto *p, ProtoEventFn ev, ProtoSendFn send, void *ctx);
void proto_free(Proto *p);

/* Starts a call: AT, then the dial string. */
void proto_connect(Proto *p, const char *dial, unsigned long now);
/* Disconnect or Quit, from any state: stops every turn, then !QUIT. */
void proto_disconnect(Proto *p, unsigned long now);

void proto_feed(Proto *p, const char *data, long n, unsigned long now);
void proto_tick(Proto *p, unsigned long now);

/* Opens session sid (1..6) in a project from the list. Returns 0 if the
   link is not online or the id is in use. */
int proto_open(Proto *p, int sid, const char *project);
/* The same, continuing the conversation the resume token names (EV_RESUMED
   reports which one started). A bridge without resume, or an empty token,
   opens a new conversation. */
int proto_open_resume(Proto *p, int sid, const char *project, const char *token);
/* The models the bridge offers (none without the models capability), and
   what "bridge default" means: names, or "" when the CLI decides. */
/* The bridge offers chats: proto_open(p, sid, "*") opens one (PROTOCOL.md, Chats). */
int proto_chat_cap(const Proto *p);
int proto_models_cap(const Proto *p);
/* The bridge can stream a session's answers (PROTOCOL.md, Streaming answers). */
int proto_stream_cap(const Proto *p);
/* The bridge reports each session's context and cost (PROTOCOL.md, Context
   and cost). The core asks for it by itself after the handshake. */
int proto_usage_cap(const Proto *p);
/* Turns streaming on or off for session sid, open or not; the bridge keeps
   it for the next open of that number. Streamed prose arrives as the usual
   EV_TEXT and EV_TEXT_CONT, in more and smaller pieces. Returns 0 when it
   was not sent (not online, no stream capability, a bad id). */
int proto_set_stream(Proto *p, int sid, int on);
int proto_nmodels(const Proto *p);
const char *proto_model(const Proto *p, int i);
const char *proto_default_model(const Proto *p);
const char *proto_default_effort(const Proto *p);
/* Sets session sid's model and effort ("" or NULL: the bridge default),
   whether or not it is open yet: before it opens, the bridge keeps the
   values for the next open of that number. The bridge confirms with
   EV_MODEL. Returns 0 when it was not sent (not online, no models
   capability, a bad id or a value with a space). */
int proto_set_model(Proto *p, int sid, const char *model, const char *effort);
/* Closes a session: the bridge denies its open request, stops its turn and
   confirms. Its file transfers fail. */
int proto_close(Proto *p, int sid);
/* Asks the bridge for the project list again (EV_PROJECTS follows). */
void proto_list_projects(Proto *p);
int proto_nprojects(const Proto *p);
const char *proto_project(const Proto *p, int i);

/* True when the prompt is /quit, which closes the tab instead of being sent. */
int proto_is_quit(const char *text, long len);
/* Sends a prompt (text as typed, CR line breaks) to a ready session.
   Returns 0 if it was not sent. */
int proto_send_prompt(Proto *p, int sid, const char *text, long len);
/* Cmd-period or Stop. */
void proto_stop(Proto *p, int sid);
/* Answers the session's open permission request: 'y', 'n' or 'a'. */
int proto_answer(Proto *p, int sid, char answer);
/* The shell has drawn everything received so far: acknowledge each W. */
void proto_drawn(Proto *p);
/* The serial driver reported lost bytes. */
void proto_line_noise(Proto *p);

SessState proto_sess_state(const Proto *p, int sid);
/* True while any session has a turn running. */
int proto_any_busy(const Proto *p);
/* Seconds without input while a turn runs, for the status line (0 if none). */
long proto_silence_secs(const Proto *p);
int proto_online(const Proto *p);

#endif
