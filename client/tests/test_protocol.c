/* Protocol core: dialing, the v2 handshake and project list, sessions,
   flow control, permissions, stopping, closing, disconnecting and link loss.
   The demo-session test replays the bytes bridge.py --demo really sent
   (fixtures/demo_session.bin). */
#include "check.h"
#include "protocol.h"

#define MAXEV 1024

typedef struct {
    EventKind kind[MAXEV];
    int sid[MAXEV];
    char text[MAXEV][128];
    long len[MAXEV];
    int n;
    char sent[16384];
    long nsent;
    int answer_on_ask;  /* answer this when a permission dialog opens */
    Proto *p;
} Rec;

static Rec R;
static Proto P;
static unsigned long now;

static void on_event(void *ctx, int sid, EventKind k, const char *t, long n)
{
    Rec *r = (Rec *)ctx;
    if (r->n < MAXEV) {
        r->kind[r->n] = k;
        r->sid[r->n] = sid;
        r->len[r->n] = n;
        memcpy(r->text[r->n], t, n < 127 ? n : 127);
        r->text[r->n][n < 127 ? n : 127] = 0;
        r->n++;
    }
    if (k == EV_PERM_ASK && r->answer_on_ask)
        proto_answer(r->p, sid, (char)r->answer_on_ask);
}

static void on_send(void *ctx, const char *d, long n)
{
    Rec *r = (Rec *)ctx;
    if (r->nsent + n < (long)sizeof(r->sent)) {
        memcpy(r->sent + r->nsent, d, n);
        r->nsent += n;
    }
}

static void reset(void)
{
    proto_free(&P);
    memset(&R, 0, sizeof(R));
    R.p = &P;
    proto_init(&P, on_event, on_send, &R);
    now = 1000;
}

static void clear_rec(void)
{
    R.n = 0;
    R.nsent = 0;
}

static void feed(const char *s)
{
    proto_feed(&P, s, (long)strlen(s), now);
}

static void tick(unsigned long ticks)
{
    unsigned long end = now + ticks;
    while (now < end) {
        now++;
        proto_tick(&P, now);
    }
}

static int count(EventKind k)
{
    int i, c = 0;
    for (i = 0; i < R.n; i++)
        if (R.kind[i] == k)
            c++;
    return c;
}

static int count_sid(EventKind k, int sid)
{
    int i, c = 0;
    for (i = 0; i < R.n; i++)
        if (R.kind[i] == k && R.sid[i] == sid)
            c++;
    return c;
}

static int find(EventKind k)
{
    int i;
    for (i = 0; i < R.n; i++)
        if (R.kind[i] == k)
            return i;
    return -1;
}

static int count_sent(const char *s)
{
    long k = (long)strlen(s), i;
    int c = 0;
    for (i = 0; i + k <= R.nsent; i++)
        if (memcmp(R.sent + i, s, k) == 0)
            c++;
    return c;
}

/* Dials and completes the handshake with a two-project list. */
static void connect_online(void)
{
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\n");
    feed("CONNECT 57600\r\n");
    feed("> \r\n> #HELLO SE30 2\r\n#OK se30bridge 2 files=rw\r\nP[\r\nP alpha\r\nP beta\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    clear_rec();
}

/* Opens a session and waits for it to be ready. */
static void open_ready(int sid, const char *name)
{
    char buf[64];
    CHECK(proto_open(&P, sid, name));
    snprintf(buf, sizeof(buf), "%dO %s\r\n%d>\r\n", sid, name, sid);
    feed(buf);
    CHECK_EQ_LONG(proto_sess_state(&P, sid), SS_READY);
    clear_rec();
}

static void test_dial_and_handshake(void)
{
    reset();
    proto_connect(&P, "ATDTbridge", now);
    CHECK_MEM(R.sent, R.nsent, "AT\r");
    CHECK_EQ_LONG(P.state, ST_DIALING);
    feed("AT\r\n");               /* modem echo is ignored */
    feed("OK\r\n");
    CHECK_MEM(R.sent, R.nsent, "AT\rATDTbridge\r");
    clear_rec();
    feed("ATDTbridge\r\nCONNECT 57600\r\n");
    CHECK_EQ_LONG(P.state, ST_HANDSHAKE);
    CHECK_MEM(R.sent, R.nsent, "\r#HELLO SE30 2\r");
    /* tty banner and echo are ignored until #OK; P] completes the handshake. */
    feed("se30bridge (demo)\r\nReady.\r\n> \r\n> #HELLO SE30 2\r\nP stray\r\n");
    CHECK_EQ_LONG(P.state, ST_HANDSHAKE);
    feed("#OK se30bridge 2 files=rw\r\nP[\r\nP alpha\r\nP beta\r\n");
    CHECK_EQ_LONG(P.state, ST_HANDSHAKE);
    feed("P]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(count(EV_CONNECTED), 1);
    CHECK_EQ_LONG(proto_nprojects(&P), 2);
    CHECK(strcmp(proto_project(&P, 0), "alpha") == 0);
    CHECK(strcmp(proto_project(&P, 1), "beta") == 0);
    CHECK_EQ_LONG(P.files, FILES_RW);
}

static void test_handshake_words_and_old_bridge(void)
{
    /* No files= word means off; unknown words are ignored. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n#OK se30bridge 2 colour=no\r\nP[\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(P.files, FILES_OFF);
    CHECK_EQ_LONG(proto_nprojects(&P), 0);

    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n#OK se30bridge 2 files=ro\r\nP[\r\nP]\r\n");
    CHECK_EQ_LONG(P.files, FILES_RO);

    /* A v1 bridge answers #OK se30bridge 1 (or treats HELLO as a prompt):
       the handshake times out with a message naming the likely cause. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n#OK se30bridge 1\r\n>\r\n#OK se30bridge 20\r\n");
    CHECK_EQ_LONG(P.state, ST_HANDSHAKE);
    tick(T_HANDSHAKE);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
    tick(T_GUARD * 2);
    feed("OK\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_DIAL_FAILED) >= 0);
    CHECK(strstr(R.text[find(EV_DIAL_FAILED)], "older than this app") != NULL);
}

static void test_handshake_window(void)
{
    /* A W inside a long project list is acknowledged, and the list completes. */
    int i;
    char line[32];
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n#OK se30bridge 2 files=rw\r\nP[\r\n");
    for (i = 0; i < 7; i++) {
        snprintf(line, sizeof(line), "P proj%d\r\n", i);
        feed(line);
    }
    feed("W\r\n");
    clear_rec();
    proto_drawn(&P);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    feed("P proj7\r\nP proj8\r\nP proj9\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(proto_nprojects(&P), 10);
    /* !LIST refreshes it. */
    clear_rec();
    proto_list_projects(&P);
    CHECK_MEM(R.sent, R.nsent, "!LIST\r");
    feed("P[\r\nP only\r\nP]\r\n");
    CHECK_EQ_LONG(count(EV_PROJECTS), 1);
    CHECK_EQ_LONG(proto_nprojects(&P), 1);
}

static void test_dial_failures(void)
{
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\n");
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_DIAL_FAILED) >= 0);
    CHECK(strstr(R.text[find(EV_DIAL_FAILED)], "NO CARRIER") != NULL);

    /* No OK to AT: hang up (the modem may still be online), retry once. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    tick(T_AT);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
    clear_rec();
    tick(T_GUARD);
    CHECK_MEM(R.sent, R.nsent, "+++");
    tick(T_GUARD);
    CHECK_MEM(R.sent, R.nsent, "+++ATH\r");
    clear_rec();
    tick(T_ATH);
    CHECK_EQ_LONG(P.state, ST_DIALING);
    CHECK_MEM(R.sent, R.nsent, "AT\r");
    tick(T_AT);
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_DIAL_FAILED) >= 0);
}

static void test_open_and_close(void)
{
    reset();
    connect_online();
    CHECK(proto_open(&P, 2, "beta"));
    CHECK_MEM(R.sent, R.nsent, "2O beta\r");
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_OPENING);
    CHECK_EQ_LONG(proto_open(&P, 2, "alpha"), 0);       /* id in use */
    /* Prompts wait for the first >, which must not end a turn. */
    feed("2O beta\r\n");
    CHECK_EQ_LONG(count_sid(EV_OPENED, 2), 1);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_OPENING);
    CHECK_EQ_LONG(proto_send_prompt(&P, 2, "hi", 2), 0);
    feed("2>\r\n");
    CHECK_EQ_LONG(count_sid(EV_READY, 2), 1);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_READY);

    /* A refused open reports the reason and frees the id. */
    clear_rec();
    proto_open(&P, 3, "gone");
    feed("3X not available\r\n");
    CHECK_EQ_LONG(count_sid(EV_OPEN_FAILED, 3), 1);
    CHECK_MEM(R.text[find(EV_OPEN_FAILED)], R.len[find(EV_OPEN_FAILED)], "not available");
    CHECK_EQ_LONG(proto_sess_state(&P, 3), SS_NONE);

    /* Close: <n>X, then <n>X closed frees it. */
    clear_rec();
    CHECK(proto_close(&P, 2));
    CHECK_MEM(R.sent, R.nsent, "2X\r");
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_CLOSING);
    CHECK_EQ_LONG(proto_send_prompt(&P, 2, "hi", 2), 0);
    feed("2T late text\r\n2X closed\r\n");
    CHECK_EQ_LONG(count_sid(EV_SESSION_CLOSED, 2), 1);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_NONE);

    /* No confirmation: the id is freed after the wait. */
    open_ready(4, "alpha");
    clear_rec();
    proto_close(&P, 4);
    tick(T_CLOSE);
    CHECK_EQ_LONG(proto_sess_state(&P, 4), SS_NONE);
    CHECK_EQ_LONG(count_sid(EV_SESSION_CLOSED, 4), 1);
}

static void test_routing_and_unknown_sessions(void)
{
    reset();
    connect_online();
    open_ready(1, "alpha");
    open_ready(2, "beta");
    proto_send_prompt(&P, 1, "a", 1);
    proto_send_prompt(&P, 2, "b", 1);
    clear_rec();
    feed("1T one\r\n2T two\r\n2+ more\r\n1U Read x\r\n");
    CHECK_EQ_LONG(R.n, 4);
    CHECK(R.kind[0] == EV_TEXT && R.sid[0] == 1);
    CHECK(R.kind[1] == EV_TEXT && R.sid[1] == 2);
    CHECK(R.kind[2] == EV_TEXT_CONT && R.sid[2] == 2);
    CHECK(R.kind[3] == EV_TOOL && R.sid[3] == 1);
    /* A digit naming no open session: nothing for anyone, counted as noise. */
    clear_rec();
    feed("5T garbled\r\nQq junk\r\n");
    CHECK_EQ_LONG(R.n, 0);
    CHECK_EQ_LONG(P.dropped, 2);
    /* Tags this version does not know are ignored. */
    feed("1Y future\r\n");
    CHECK_EQ_LONG(R.n, 0);
    /* An empty T is a blank line; an exception ends the turn with > only. */
    feed("1T\r\n1C[\r\n1C code\r\n1+ more\r\n1I Error: boom\r\n1>\r\n");
    CHECK_EQ_LONG(R.len[0], 0);
    CHECK_EQ_LONG(count_sid(EV_CODE_CONT, 1), 1);
    CHECK(find(EV_CODE_END) >= 0 && find(EV_CODE_END) < find(EV_READY));
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_BUSY);
}

static void test_prompts(void)
{
    reset();
    connect_online();
    open_ready(3, "alpha");
    CHECK(proto_send_prompt(&P, 3, "one\rtwo", 7));
    CHECK_MEM(R.sent, R.nsent, "3P one\\\r3P two\r");
    CHECK_EQ_LONG(proto_sess_state(&P, 3), SS_BUSY);
    CHECK_EQ_LONG(proto_send_prompt(&P, 3, "again", 5), 0);   /* busy */
    feed("3>\r\n");                                           /* one > ends it */
    CHECK_EQ_LONG(count_sid(EV_READY, 3), 1);
    CHECK_EQ_LONG(proto_sess_state(&P, 3), SS_READY);
    /* /quit is never sent: the shell closes the tab instead. */
    clear_rec();
    CHECK(proto_is_quit("  /QUIT", 7));
    CHECK(!proto_is_quit("/quitter", 8));
    CHECK_EQ_LONG(proto_send_prompt(&P, 3, "/quit", 5), 0);
    CHECK_EQ_LONG(R.nsent, 0);
    /* Not while opening, nor on a closed id. */
    proto_open(&P, 4, "beta");
    CHECK_EQ_LONG(proto_send_prompt(&P, 4, "hi", 2), 0);
    CHECK_EQ_LONG(proto_send_prompt(&P, 5, "hi", 2), 0);
}

static void test_permissions_per_session(void)
{
    reset();
    connect_online();
    open_ready(1, "alpha");
    open_ready(2, "beta");
    proto_send_prompt(&P, 1, "a", 1);
    proto_send_prompt(&P, 2, "b", 1);
    feed("1Q 4 Bash ls\r\n1D[\r\n1D ls -la\r\n1D]\r\n2Q 4 Write x\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_BUSY);      /* not until ? */
    feed("1? 4\r\n2? 4\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_PERMISSION);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_PERMISSION);
    clear_rec();
    CHECK(proto_answer(&P, 2, 'y'));
    CHECK_MEM(R.sent, R.nsent, "2A 4 y\r");
    CHECK_EQ_LONG(proto_answer(&P, 2, 'y'), 0);            /* never twice */
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_PERMISSION);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_BUSY);
    /* A ? for another request id is not the one we hold. */
    feed("2? 9\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_BUSY);
    /* Stop on session 2 sends 2!STOP only. */
    clear_rec();
    proto_stop(&P, 2);
    proto_stop(&P, 2);
    CHECK_MEM(R.sent, R.nsent, "2!STOP\r");
    feed("2R interrupted $0.01\r\n2>\r\n");
    CHECK(count_sid(EV_INTERRUPTED, 2) == 1);
    /* Stop with session 1's dialog open: deny, then stop. */
    clear_rec();
    proto_stop(&P, 1);
    CHECK_MEM(R.sent, R.nsent, "1A 4 n\r1!STOP\r");
    /* Stop after Q but before ?: deny when ? arrives. */
    feed("1>\r\n");
    proto_send_prompt(&P, 1, "a", 1);
    feed("1Q 5 Bash rm\r\n");
    clear_rec();
    proto_stop(&P, 1);
    CHECK_EQ_LONG(R.nsent, 0);
    feed("1? 5\r\n");
    CHECK_MEM(R.sent, R.nsent, "1A 5 n\r1!STOP\r");
    CHECK_EQ_LONG(count(EV_PERM_ASK), 0);
}

static void test_acks(void)
{
    reset();
    connect_online();
    open_ready(1, "alpha");
    proto_send_prompt(&P, 1, "go", 2);
    feed("1Q 8 Write a.txt\r\nW\r\n1D[\r\n1D x\r\nW\r\n1D]\r\n");
    clear_rec();
    proto_drawn(&P);
    CHECK_EQ_LONG(count_sent("!ACK\r"), 2);
    clear_rec();
    proto_drawn(&P);
    CHECK_EQ_LONG(R.nsent, 0);
}

static void test_link_lost(void)
{
    reset();
    connect_online();
    open_ready(1, "alpha");
    open_ready(2, "beta");
    proto_send_prompt(&P, 1, "a", 1);
    proto_send_prompt(&P, 2, "b", 1);
    feed("1Q 1 Edit x\r\n1? 1\r\n2Q 1 Edit y\r\n2? 1\r\n");
    clear_rec();
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(count_sid(EV_PERM_CLOSE, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_PERM_CLOSE, 2), 1);
    CHECK_EQ_LONG(count(EV_PERM_ABANDONED), 2);
    CHECK_EQ_LONG(count(EV_LOST), 1);
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_NONE);
    CHECK_EQ_LONG(R.nsent, 0);             /* nothing answered */
}

static void test_disconnect(void)
{
    /* Session 1 busy, session 2 in Permission: deny and stop both, keep
       acknowledging, wait for both >, then !QUIT and hang up. */
    reset();
    connect_online();
    open_ready(1, "alpha");
    open_ready(2, "beta");
    open_ready(3, "alpha2");
    proto_send_prompt(&P, 1, "a", 1);
    proto_send_prompt(&P, 2, "b", 1);
    feed("2Q 3 Bash ls\r\n2? 3\r\n");
    clear_rec();
    proto_disconnect(&P, now);
    CHECK_EQ_LONG(P.state, ST_CLOSING);
    CHECK_EQ_LONG(count_sent("2A 3 n\r"), 1);
    CHECK_EQ_LONG(count_sent("2!STOP\r"), 1);
    CHECK_EQ_LONG(count_sent("1!STOP\r"), 1);
    CHECK_EQ_LONG(count_sent("3!STOP\r"), 0);
    CHECK_EQ_LONG(count_sent("!QUIT\r"), 0);
    clear_rec();
    feed("1T more\r\nW\r\n");
    proto_drawn(&P);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    feed("2R interrupted $0.01\r\n2>\r\n");
    CHECK_EQ_LONG(count_sent("!QUIT\r"), 0);
    feed("1R interrupted $0.01\r\n1>\r\n");
    CHECK_EQ_LONG(count_sent("!QUIT\r"), 1);
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK_EQ_LONG(count(EV_CLOSED), 1);
    CHECK_EQ_LONG(count(EV_LOST), 0);

    /* Idle sessions: !QUIT at once; no drop: hang up after the quit wait. */
    reset();
    connect_online();
    open_ready(1, "alpha");
    proto_disconnect(&P, now);
    CHECK_MEM(R.sent, R.nsent, "!QUIT\r");
    clear_rec();
    tick(T_QUIT + T_GUARD * 2);
    CHECK_MEM(R.sent, R.nsent, "+++ATH\r");

    /* A turn that never ends: !QUIT after 10 s, then the hangup. */
    reset();
    connect_online();
    open_ready(1, "alpha");
    proto_send_prompt(&P, 1, "a", 1);
    proto_disconnect(&P, now);
    clear_rec();
    tick(T_STOP_TURN);
    CHECK_MEM(R.sent, R.nsent, "!QUIT\r");
    tick(T_QUIT + T_GUARD * 2 + T_ATH);
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_CLOSED) >= 0);

    /* During the handshake: hang up straight away. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n");
    proto_disconnect(&P, now);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
}

static void test_silence(void)
{
    /* A background session's quiet turn counts, not just the front one. */
    reset();
    connect_online();
    open_ready(1, "alpha");
    open_ready(2, "beta");
    proto_send_prompt(&P, 2, "go", 2);
    clear_rec();
    tick(T_SILENCE - 1);
    CHECK_EQ_LONG(count(EV_SILENCE), 0);
    tick(1);
    CHECK_EQ_LONG(count(EV_SILENCE), 1);
    CHECK_EQ_LONG(R.nsent, 0);              /* no serial errors: no spare ACK */
    CHECK_EQ_LONG(proto_silence_secs(&P), 90);
    feed("2T back\r\n");
    CHECK_EQ_LONG(proto_silence_secs(&P), 0);
    /* After line noise, one spare ACK. */
    proto_line_noise(&P);
    clear_rec();
    tick(T_SILENCE);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    /* Nothing running: no warning. */
    feed("2>\r\n");
    clear_rec();
    tick(T_SILENCE * 2);
    CHECK_EQ_LONG(count(EV_SILENCE), 0);
}

static char *load_fixture(long *len)
{
    FILE *f = fopen(FIXTURE_DIR "/demo_session.bin", "rb");
    char *buf;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(*len);
    if (fread(buf, 1, *len, f) != (size_t)*len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static long find_bytes(const char *buf, long len, long from, const char *s)
{
    long k = (long)strlen(s), i;
    for (i = from; i + k <= len; i++)
        if (memcmp(buf + i, s, k) == 0)
            return i;
    return -1;
}

/* Connects to a bridge that advertises resume and heartbeat. */
static void connect_caps(void)
{
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\n");
    feed("CONNECT 57600\r\n");
    feed("> \r\n> #HELLO SE30 2\r\n#OK se30bridge 2 files=rw resume ping\r\n"
         "P[\r\nP alpha\r\nP beta\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    clear_rec();
}

static void test_capabilities_and_resume_open(void)
{
    /* Capability words are read; without them, resume opens are plain opens
       and no heartbeat is ever sent. */
    reset();
    connect_caps();
    CHECK(P.resume_cap && P.ping_cap);
    CHECK(proto_open_resume(&P, 1, "alpha", "0123456789abcdef"));
    CHECK_MEM(R.sent, R.nsent, "1Y 0123456789abcdef alpha\r");
    clear_rec();
    feed("1O alpha\r\n1Y 0123456789abcdef resumed\r\n1>\r\n");
    CHECK_EQ_LONG(count(EV_OPENED), 1);
    CHECK_EQ_LONG(count(EV_RESUMED), 1);
    CHECK(strcmp(R.text[find(EV_RESUMED)], "resumed 0123456789abcdef") == 0);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);

    /* An empty token, or no token, is a plain open. */
    clear_rec();
    CHECK(proto_open_resume(&P, 2, "beta", ""));
    CHECK_MEM(R.sent, R.nsent, "2O beta\r");
    clear_rec();
    feed("2O beta\r\n2Y aaaaaaaaaaaaaaaa new\r\n2>\r\n");
    CHECK(strcmp(R.text[find(EV_RESUMED)], "new aaaaaaaaaaaaaaaa") == 0);

    /* A bridge without the word gets "O" even when the app has a token. */
    reset();
    connect_online();
    CHECK(!P.resume_cap && !P.ping_cap);
    CHECK(proto_open_resume(&P, 1, "alpha", "0123456789abcdef"));
    CHECK_MEM(R.sent, R.nsent, "1O alpha\r");
    tick(T_DEAD * 2);
    CHECK_EQ_LONG(count_sent("!PING"), 0);
    CHECK_EQ_LONG(P.state, ST_ONLINE);

    /* A new token after /new or /edits, and a malformed Y, are handled. */
    reset();
    connect_caps();
    open_ready(1, "alpha");
    feed("1Y bbbbbbbbbbbbbbbb new\r\n1Y\r\n1Y x\r\n1Y toolongtokenxxxxxxx new\r\n");
    CHECK_EQ_LONG(count(EV_RESUMED), 1);
    CHECK(strcmp(R.text[find(EV_RESUMED)], "new bbbbbbbbbbbbbbbb") == 0);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);
    /* A Y for a session that is not open is dropped like any other line. */
    clear_rec();
    feed("3Y cccccccccccccccc new\r\n");
    CHECK_EQ_LONG(count(EV_RESUMED), 0);
}

static void test_heartbeat_and_dead_link(void)
{
    /* A !PING after 30 quiet seconds; not while other lines go out. */
    reset();
    connect_caps();
    tick(T_PING - 1);
    CHECK_EQ_LONG(count_sent("!PING"), 0);
    tick(1);
    CHECK_EQ_LONG(count_sent("!PING"), 1);
    feed("#PONG\r\n");
    CHECK_EQ_LONG(P.ping_out, 0);
    CHECK_EQ_LONG(P.dropped, 0);
    open_ready(1, "alpha");
    clear_rec();
    tick(T_PING - 5);
    proto_send_prompt(&P, 1, "go", 2);           /* traffic resets the heartbeat */
    clear_rec();
    feed("1T hi\r\n");
    tick(T_PING - 1);
    CHECK_EQ_LONG(count_sent("!PING"), 0);
    tick(1);
    CHECK_EQ_LONG(count_sent("!PING"), 1);

    /* A #PONG does not reset the silence timer of a running turn. */
    reset();
    connect_caps();
    open_ready(1, "alpha");
    proto_send_prompt(&P, 1, "go", 2);
    clear_rec();
    for (int i = 0; i < 4; i++) {
        tick(T_PING);
        feed("#PONG\r\n");
    }
    CHECK_EQ_LONG(count(EV_SILENCE), 1);        /* 90 s of pongs only */
    CHECK_EQ_LONG(count(EV_LOST), 0);

    /* Pongs keep the link alive for as long as they come. */
    clear_rec();
    for (int i = 0; i < 10; i++) {
        tick(T_PING);
        feed("#PONG\r\n");
    }
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(count(EV_LOST), 0);

    /* Silence with an unanswered ping: hang up, then report the loss. */
    reset();
    connect_caps();
    open_ready(1, "alpha");
    proto_send_prompt(&P, 1, "go", 2);
    feed("1Q 1 Edit x\r\n1? 1\r\n");
    clear_rec();
    tick(T_DEAD - 1);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(count(EV_LOST), 0);
    tick(1);                                    /* 90 s silent, pings unanswered */
    CHECK_EQ_LONG(P.state, ST_HANGUP);
    CHECK_EQ_LONG(count(EV_LOST), 0);           /* not before the modem is on hook */
    CHECK_EQ_LONG(count(EV_PERM_CLOSE), 1);
    CHECK_EQ_LONG(count(EV_PERM_ABANDONED), 1);
    tick(T_GUARD);
    CHECK_MEM(R.sent + R.nsent - 3, 3, "+++");
    tick(T_GUARD);
    CHECK_MEM(R.sent + R.nsent - 4, 4, "ATH\r");
    feed("OK\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK_EQ_LONG(count(EV_LOST), 1);
    CHECK_EQ_LONG(count(EV_CLOSED), 0);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_NONE);

    /* A stopped pump (a modal alert): the first tick after it sends a ping
       instead of declaring the link lost. */
    reset();
    connect_caps();
    open_ready(1, "alpha");
    now += T_DEAD * 2;
    clear_rec();
    proto_tick(&P, now);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(count_sent("!PING"), 1);
    feed("#PONG\r\n");
    tick(T_PING);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(count(EV_LOST), 0);

    /* ... and if that ping is never answered, it gets T_PING_WAIT first. */
    reset();
    connect_caps();
    now += T_DEAD * 2;
    proto_tick(&P, now);
    CHECK_EQ_LONG(P.ping_out, 1);
    tick(T_PING_WAIT - 1);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    tick(1);
    CHECK_EQ_LONG(P.state, ST_HANGUP);

    /* NO CARRIER is still immediate. */
    reset();
    connect_caps();
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK_EQ_LONG(count(EV_LOST), 1);
}

static void test_models(void)
{
    /* The list comes with the capability word and ends with the defaults. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\nCONNECT 57600\r\n");
    feed("#OK se30bridge 2 files=rw resume ping models\r\n"
         "M[\r\nM opus\r\nM sonnet\r\nM haiku\r\nM] sonnet -\r\n"
         "P[\r\nP alpha\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK(proto_models_cap(&P));
    CHECK_EQ_LONG(proto_nmodels(&P), 3);
    CHECK(strcmp(proto_model(&P, 0), "opus") == 0);
    CHECK(strcmp(proto_model(&P, 2), "haiku") == 0);
    CHECK(strcmp(proto_model(&P, 3), "") == 0);
    CHECK(strcmp(proto_default_model(&P), "sonnet") == 0);
    CHECK(strcmp(proto_default_effort(&P), "") == 0);
    CHECK_EQ_LONG(P.dropped, 0);
    CHECK_EQ_LONG(P.noise, 0);

    /* Presets and changes go out the same way, open or not. */
    clear_rec();
    CHECK(proto_set_model(&P, 3, "sonnet", "high"));
    CHECK_MEM(R.sent, R.nsent, "3M sonnet high\r");
    clear_rec();
    CHECK(proto_set_model(&P, 2, "", NULL));
    CHECK_MEM(R.sent, R.nsent, "2M - -\r");
    CHECK(!proto_set_model(&P, 0, "x", "y"));
    CHECK(!proto_set_model(&P, 7, "x", "y"));
    CHECK(!proto_set_model(&P, 1, "a b", "y"));
    clear_rec();
    CHECK(proto_open(&P, 1, "alpha"));
    feed("1O alpha\r\n1Y 0123456789abcdef new\r\n1M sonnet high\r\n1>\r\n");
    CHECK_EQ_LONG(count_sid(EV_MODEL, 1), 1);
    CHECK(strcmp(R.text[find(EV_MODEL)], "sonnet high") == 0);
    clear_rec();
    feed("1M - max\r\n");
    CHECK(strcmp(R.text[find(EV_MODEL)], "- max") == 0);

    /* A model line for a session that is not open is dropped, like others. */
    clear_rec();
    feed("4M opus low\r\n");
    CHECK_EQ_LONG(count(EV_MODEL), 0);
    CHECK_EQ_LONG(P.dropped, 1);

    /* More than 16 names keep the first 16; a closing line with no defaults. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\nCONNECT 57600\r\n#OK se30bridge 2 models\r\nM[\r\n");
    {
        int i;
        char line[32];
        for (i = 0; i < 20; i++) {
            sprintf(line, "M model%d\r\n", i);
            feed(line);
        }
    }
    feed("M] - -\r\nP[\r\nP]\r\n");
    CHECK_EQ_LONG(proto_nmodels(&P), MAX_MODELS);
    CHECK(strcmp(proto_model(&P, 15), "model15") == 0);
    CHECK(strcmp(proto_default_model(&P), "") == 0);

    /* No capability word: the lines are ignored without noise and nothing
       is sent. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\nCONNECT 57600\r\n#OK se30bridge 2 files=rw\r\n"
         "M[\r\nM opus\r\nM] - -\r\nP[\r\nP alpha\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK(!proto_models_cap(&P));
    CHECK_EQ_LONG(proto_nmodels(&P), 0);
    CHECK_EQ_LONG(P.dropped, 0);
    CHECK_EQ_LONG(P.noise, 0);
    clear_rec();
    CHECK(!proto_set_model(&P, 1, "opus", "high"));
    CHECK_EQ_LONG(R.nsent, 0);

    /* A new call forgets the list. */
    reset();
    connect_caps();
    CHECK_EQ_LONG(proto_nmodels(&P), 0);
}

static void test_demo_session(void)
{
    long len = 0, at, opened, done, i, ws = 0;
    char *fx = load_fixture(&len);
    CHECK(fx != NULL);
    if (!fx)
        return;
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT 57600\r\n");

    /* Everything up to the end of the project list. */
    at = find_bytes(fx, len, 0, "P]\r\n") + 4;
    proto_feed(&P, fx, at, now);
    proto_drawn(&P);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK_EQ_LONG(P.files, FILES_RW);
    CHECK(proto_nprojects(&P) >= 2);

    proto_open(&P, 1, proto_project(&P, 0));
    proto_open(&P, 2, proto_project(&P, 1));
    opened = find_bytes(fx, len, at, "2>\r\n") + 4;
    proto_feed(&P, fx + at, opened - at, now);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_READY);

    clear_rec();
    R.answer_on_ask = 'y';
    CHECK(proto_send_prompt(&P, 1, "hello\rthere", 11));
    CHECK(proto_send_prompt(&P, 2, "second", 6));
    clear_rec();

    /* Both turns, one byte at a time, drawing after every byte. */
    done = find_bytes(fx, len, opened, "I Bye.");
    if (done < 0)
        done = len;
    for (i = opened; i < len; i++) {
        proto_feed(&P, fx + i, 1, now);
        proto_drawn(&P);
        if (fx[i] == 'W' && fx[i - 1] == '\n' && fx[i + 1] == '\r')
            ws++;
    }
    CHECK(ws > 0);
    CHECK_EQ_LONG(count_sent("!ACK\r"), ws);       /* one per W, no more */
    CHECK_EQ_LONG(count_sent("1A 1 y\r"), 1);
    CHECK_EQ_LONG(count_sent("2A 1 y\r"), 1);
    CHECK_EQ_LONG(count_sid(EV_PERM_ASK, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_PERM_LINE, 1), 4);
    CHECK_EQ_LONG(count_sid(EV_READY, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_READY, 2), 1);
    CHECK_EQ_LONG(count_sid(EV_CODE_BEGIN, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_CODE_END, 2), 1);
    CHECK_EQ_LONG(count_sid(EV_RESULT, 1), 1);
    CHECK_EQ_LONG(count(EV_INTERRUPTED), 0);
    CHECK_EQ_LONG(P.dropped, 0);
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);
    free(fx);
}

static void test_agent_lines(void)
{
    /* The word turns on !AGENTS, sent once after the project list. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\n");
    feed("CONNECT 57600\r\n");
    feed("> \r\n> #HELLO SE30 2\r\n#OK se30bridge 2 files=rw resume ping agents\r\n"
         "P[\r\nP alpha\r\nP]\r\n");
    CHECK(P.agents_cap);
    CHECK_EQ_LONG(count_sent("!AGENTS\r"), 1);
    clear_rec();
    open_ready(1, "alpha");
    CHECK(proto_send_prompt(&P, 1, "go", 2));
    clear_rec();

    /* The three lines arrive as events with the payload unchanged. */
    feed("1J+ 1 Explore Find tests\r\n1J~ 1 3 Grep foo\r\n1J- 1 done 3 found it\r\n");
    CHECK_EQ_LONG(count_sid(EV_AGENT_START, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_AGENT_STEP, 1), 1);
    CHECK_EQ_LONG(count_sid(EV_AGENT_END, 1), 1);
    CHECK(strcmp(R.text[find(EV_AGENT_START)], "1 Explore Find tests") == 0);
    CHECK(strcmp(R.text[find(EV_AGENT_STEP)], "1 3 Grep foo") == 0);
    CHECK(strcmp(R.text[find(EV_AGENT_END)], "1 done 3 found it") == 0);
    CHECK_EQ_LONG(P.sess[1].agents_open, 0);

    /* A turn that ends with a line still open closes it, once. */
    clear_rec();
    feed("1J+ 1 Explore Find tests\r\n1J+ 2 Plan Make a plan\r\n1J- 2 done 1 ok\r\n");
    CHECK_EQ_LONG(P.sess[1].agents_open, 1);
    clear_rec();
    feed("1R success $0.01\r\n1>\r\n");
    CHECK_EQ_LONG(count(EV_AGENT_END), 1);
    CHECK(strcmp(R.text[find(EV_AGENT_END)], "* stopped") == 0);
    CHECK(find(EV_AGENT_END) < find(EV_READY));
    CHECK_EQ_LONG(P.sess[1].agents_open, 0);

    /* A turn with no subagents sends no wildcard. */
    CHECK(proto_send_prompt(&P, 1, "again", 5));
    clear_rec();
    feed("1T hi\r\n1R success $0.01\r\n1>\r\n");
    CHECK_EQ_LONG(count(EV_AGENT_END), 0);

    /* A J line for a session that is not open is counted as dropped. */
    clear_rec();
    P.dropped = 0;
    feed("4J+ 1 Explore x\r\n");
    CHECK_EQ_LONG(count(EV_AGENT_START), 0);
    CHECK_EQ_LONG(P.dropped, 1);

    /* An unknown future tag is still ignored. */
    clear_rec();
    P.dropped = 0;
    feed("1J? what\r\n");
    CHECK_EQ_LONG(P.dropped, 0);
    CHECK_EQ_LONG(R.n, 0);

    /* A lost link closes open lines. */
    CHECK(proto_send_prompt(&P, 1, "third", 5));
    feed("1J+ 1 Explore Find tests\r\n");
    clear_rec();
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(count(EV_AGENT_END), 1);

    /* After a redial the opt-in is sent again; without the word it never is. */
    reset();
    connect_online();
    CHECK(!P.agents_cap);
    CHECK_EQ_LONG(count_sent("!AGENTS"), 0);
}

static void test_chat_capability(void)
{
    /* The word is read from #OK and forgotten at the next one. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\nCONNECT 57600\r\n");
    feed("#OK se30bridge 2 files=rw resume ping models agents chat\r\n"
         "P[\r\nP alpha\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK(proto_chat_cap(&P));

    /* A chat opens with the reserved name, and resumes with its token. */
    clear_rec();
    CHECK(proto_open(&P, 3, "*"));
    CHECK_MEM(R.sent, R.nsent, "3O *\r");
    feed("3O *\r\n3Y 0123456789abcdef new\r\n3>\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 3), SS_READY);
    CHECK(strcmp(R.text[find(EV_OPENED)], "*") == 0);
    clear_rec();
    CHECK(proto_close(&P, 3));
    CHECK_MEM(R.sent, R.nsent, "3X\r");
    feed("3X closed\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 3), SS_NONE);
    clear_rec();
    CHECK(proto_open_resume(&P, 3, "*", "0123456789abcdef"));
    CHECK_MEM(R.sent, R.nsent, "3Y 0123456789abcdef *\r");

    /* A bridge without the word. */
    reset();
    connect_online();
    CHECK(!proto_chat_cap(&P));
}

static void test_stream_capability(void)
{
    /* The word comes with #OK and is forgotten at the next one. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\nCONNECT 57600\r\n");
    feed("#OK se30bridge 2 files=rw resume ping models agents chat stream\r\n"
         "M[\r\nM opus\r\nM] - -\r\nP[\r\nP alpha\r\nP]\r\n");
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    CHECK(proto_stream_cap(&P));

    /* The setting goes out for any session number, open or not. */
    clear_rec();
    CHECK(proto_set_stream(&P, 3, 1));
    CHECK_MEM(R.sent, R.nsent, "3V on\r");
    clear_rec();
    CHECK(proto_set_stream(&P, 3, 0));
    CHECK_MEM(R.sent, R.nsent, "3V off\r");
    CHECK(!proto_set_stream(&P, 0, 1));
    CHECK(!proto_set_stream(&P, 7, 1));
    CHECK_EQ_LONG(P.dropped, 0);

    /* A bridge without the word is never sent the line. */
    reset();
    connect_online();
    CHECK(!proto_stream_cap(&P));
    clear_rec();
    CHECK(!proto_set_stream(&P, 1, 1));
    CHECK_EQ_LONG(R.nsent, 0);

    /* Not online: nothing is sent. */
    reset();
    CHECK(!proto_set_stream(&P, 1, 1));
}

int main(void)
{
    test_stream_capability();
    test_chat_capability();
    test_dial_and_handshake();
    test_handshake_words_and_old_bridge();
    test_handshake_window();
    test_dial_failures();
    test_open_and_close();
    test_routing_and_unknown_sessions();
    test_prompts();
    test_permissions_per_session();
    test_acks();
    test_link_lost();
    test_disconnect();
    test_silence();
    test_capabilities_and_resume_open();
    test_heartbeat_and_dead_link();
    test_agent_lines();
    test_models();
    test_demo_session();
    proto_free(&P);
    CHECK_DONE();
}
