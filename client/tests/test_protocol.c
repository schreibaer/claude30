/* Protocol core: dialing, handshake, turns, flow control, permissions,
   stopping, disconnecting and link loss. The demo-session test replays the
   bytes bridge.py --demo really sent (fixtures/demo_session.bin). */
#include "check.h"
#include "protocol.h"

#define MAXEV 512

typedef struct {
    EventKind kind[MAXEV];
    char text[MAXEV][128];
    long len[MAXEV];
    int n;
    char sent[8192];
    long nsent;
    int answer_on_ask;  /* answer this when a permission dialog opens */
    Proto *p;
} Rec;

static Rec R;
static Proto P;
static unsigned long now;

static void on_event(void *ctx, EventKind k, const char *t, long n)
{
    Rec *r = (Rec *)ctx;
    if (r->n < MAXEV) {
        r->kind[r->n] = k;
        r->len[r->n] = n;
        memcpy(r->text[r->n], t, n < 127 ? n : 127);
        r->text[r->n][n < 127 ? n : 127] = 0;
        r->n++;
    }
    if (k == EV_PERM_ASK && r->answer_on_ask)
        proto_answer(r->p, (char)r->answer_on_ask);
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

/* Dials and completes the handshake with a minimal bridge greeting. */
static void connect_ready(void)
{
    proto_connect(&P, "ATDTbridge", now);
    feed("AT\r\nOK\r\n");
    feed("CONNECT 57600\r\n");
    feed("> \r\n> #HELLO SE30 1\r\n#OK se30bridge 1\r\n>\r\n");
    CHECK_EQ_LONG(P.state, ST_READY);
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
    CHECK_MEM(R.sent, R.nsent, "\r#HELLO SE30 1\r");
    /* tty banner and echo are ignored until #OK, then > means ready. */
    feed("se30bridge (demo)\r\nReady.\r\n> \r\n> #HELLO SE30 1\r\n");
    CHECK_EQ_LONG(P.state, ST_HANDSHAKE);
    feed("#OK se30bridge 1\r\n>\r\n");
    CHECK_EQ_LONG(P.state, ST_READY);
    CHECK(find(EV_CONNECTED) >= 0);
    CHECK_EQ_LONG(count(EV_READY), 1);
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

static void test_handshake_timeout_hangs_up(void)
{
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n");
    clear_rec();
    tick(T_HANDSHAKE);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
    CHECK_EQ_LONG(R.nsent, 0);              /* guard time is silent */
    tick(T_GUARD * 2);
    CHECK_MEM(R.sent, R.nsent, "+++ATH\r");
    feed("OK\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_DIAL_FAILED) >= 0);
    /* The next Connect starts with AT, not with the dial string. */
    clear_rec();
    proto_connect(&P, "ATDTbridge", now);
    CHECK_MEM(R.sent, R.nsent, "AT\r");
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

static void test_demo_session(void)
{
    long len = 0, ready_at, bye_at, i, ws = 0;
    char *fx = load_fixture(&len);
    CHECK(fx != NULL);
    if (!fx)
        return;
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT 57600\r\n");

    /* Everything up to the ready marker after #OK. */
    ready_at = find_bytes(fx, len, find_bytes(fx, len, 0, "#OK se30bridge 1"), ">\r\n") + 3;
    proto_feed(&P, fx, ready_at, now);
    CHECK_EQ_LONG(P.state, ST_READY);

    clear_rec();
    R.answer_on_ask = 'y';
    CHECK(proto_send_prompt(&P, "hello\rthere", 11));
    CHECK_MEM(R.sent, R.nsent, "hello\\\rthere\r");
    CHECK_EQ_LONG(P.state, ST_BUSY);
    clear_rec();

    /* The turn, one byte at a time, drawing after every byte. */
    bye_at = find_bytes(fx, len, ready_at, "I Bye.");
    for (i = ready_at; i < bye_at; i++) {
        proto_feed(&P, fx + i, 1, now);
        proto_drawn(&P);
        if (fx[i] == 'W' && fx[i - 1] == '\n' && fx[i + 1] == '\r')
            ws++;
    }
    CHECK(ws > 0);
    CHECK_EQ_LONG(count_sent("!ACK\r"), ws);       /* one per W, no more */
    CHECK_EQ_LONG(count_sent("A 1 y\r"), 1);
    CHECK_EQ_LONG(count(EV_PERM_ASK), 1);
    CHECK_EQ_LONG(count(EV_PERM_LINE), 4);
    CHECK_EQ_LONG(count(EV_READY), 1);              /* continuation > swallowed */
    CHECK_EQ_LONG(count(EV_CODE_BEGIN), 1);
    CHECK_EQ_LONG(count(EV_CODE_END), 1);
    CHECK_EQ_LONG(count(EV_TEXT_CONT), 1);
    CHECK_EQ_LONG(count(EV_INTERRUPTED), 0);
    CHECK(find(EV_RESULT) >= 0);
    CHECK_MEM(R.text[find(EV_RESULT)], R.len[find(EV_RESULT)], "success $0.0000");
    CHECK_MEM(R.text[find(EV_TOOL)], R.len[find(EV_TOOL)], "Read src/parser.py");
    CHECK_EQ_LONG(P.state, ST_READY);

    /* Disconnect: /quit; the bridge says Bye and may still send a W. */
    clear_rec();
    proto_disconnect(&P, now);
    CHECK_MEM(R.sent, R.nsent, "/quit\r");
    CHECK_EQ_LONG(P.state, ST_CLOSING);
    clear_rec();
    proto_feed(&P, fx + bye_at, len - bye_at, now);
    proto_drawn(&P);
    CHECK_EQ_LONG(count_sent("!ACK\r"), 1);
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_CLOSED) >= 0);
    CHECK_EQ_LONG(count(EV_LOST), 0);
    free(fx);
}

static void test_turn_details(void)
{
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    clear_rec();
    feed("T first\r\n+ second\r\nC[\r\nC code\r\n+ more\r\n");
    CHECK_EQ_LONG(R.kind[0], EV_TEXT);
    CHECK_EQ_LONG(R.kind[1], EV_TEXT_CONT);
    CHECK_MEM(R.text[1], R.len[1], "second");
    CHECK_EQ_LONG(R.kind[2], EV_CODE_BEGIN);
    CHECK_EQ_LONG(R.kind[3], EV_CODE);
    CHECK_EQ_LONG(R.kind[4], EV_CODE_CONT);
    /* An empty T is a blank line. */
    clear_rec();
    feed("T\r\n");
    CHECK_EQ_LONG(R.kind[0], EV_TEXT);
    CHECK_EQ_LONG(R.len[0], 0);
    /* An exception ends the turn with I and > but no R; open code closes. */
    clear_rec();
    feed("I Error: boom\r\n>\r\n");
    CHECK_EQ_LONG(count(EV_RESULT), 0);
    CHECK(find(EV_CODE_END) >= 0);
    CHECK(find(EV_CODE_END) < find(EV_READY));
    CHECK_EQ_LONG(P.state, ST_READY);
    /* A line the core does not know is still shown. */
    clear_rec();
    proto_send_prompt(&P, "x", 1);
    feed("Zq garbled\r\n");
    CHECK_EQ_LONG(count(EV_UNKNOWN), 1);
}

static void test_long_permission_line(void)
{
    static char line[3100];
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    clear_rec();
    memset(line, 'x', sizeof(line));
    memcpy(line, "D ", 2);
    line[3002] = '\r';
    line[3003] = 0;
    feed("Q 1 Bash long\r\nD[\r\n");
    feed(line);
    feed("D]\r\n? 1\r\n");
    CHECK_EQ_LONG(count(EV_PERM_LINE), 1);
    CHECK_EQ_LONG(R.len[find(EV_PERM_LINE)], 3000);
    CHECK_MEM(R.text[find(EV_PERM_BEGIN)], R.len[find(EV_PERM_BEGIN)], "Bash long");
    CHECK_EQ_LONG(P.state, ST_PERMISSION);
}

static void test_answers(void)
{
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    feed("Q 7 Write a.txt (1 lines)\r\nD[\r\nD +x\r\nD]\r\n");
    CHECK_EQ_LONG(P.state, ST_BUSY);       /* not until ? */
    feed("? 8\r\n");                        /* not the request we hold */
    CHECK_EQ_LONG(P.state, ST_BUSY);
    feed("? 7\r\n");
    CHECK_EQ_LONG(P.state, ST_PERMISSION);
    clear_rec();
    proto_answer(&P, 'a');
    proto_answer(&P, 'y');                  /* a second answer is never sent */
    CHECK_MEM(R.sent, R.nsent, "A 7 a\r");
    CHECK_EQ_LONG(P.state, ST_BUSY);
    /* W during a permission request is acknowledged. */
    feed("Q 8 Bash rm x\r\nW\r\n");
    clear_rec();
    proto_drawn(&P);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
}

static void test_stop(void)
{
    /* Stop in a running turn: !STOP once; the app marks the turn. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    clear_rec();
    proto_stop(&P);
    proto_stop(&P);
    CHECK_MEM(R.sent, R.nsent, "!STOP\r");
    feed("R success $0.01\r\n>\r\n");
    CHECK(find(EV_INTERRUPTED) > find(EV_RESULT));
    CHECK_EQ_LONG(P.state, ST_READY);

    /* Stop with the dialog open: Deny, then !STOP. */
    proto_send_prompt(&P, "go", 2);
    feed("Q 2 Bash ls\r\n? 2\r\n");
    clear_rec();
    proto_stop(&P);
    CHECK_MEM(R.sent, R.nsent, "A 2 n\r!STOP\r");
    CHECK_EQ_LONG(P.state, ST_BUSY);
    feed(">\r\n");

    /* Stop after Q but before ?: wait for ?, then Deny and !STOP. */
    proto_send_prompt(&P, "go", 2);
    feed("Q 3 Bash ls\r\n");
    clear_rec();
    proto_stop(&P);
    CHECK_EQ_LONG(R.nsent, 0);
    feed("D ls -la\r\n? 3\r\n");
    CHECK_MEM(R.sent, R.nsent, "A 3 n\r!STOP\r");
    CHECK_EQ_LONG(count(EV_PERM_ASK), 0);
    feed(">\r\n");
    CHECK(find(EV_INTERRUPTED) >= 0);
}

static void test_link_lost(void)
{
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    feed("Q 1 Edit x\r\n? 1\r\n");
    clear_rec();
    feed("NO CARRIER\r\n");
    CHECK(find(EV_PERM_CLOSE) >= 0);
    CHECK(find(EV_PERM_ABANDONED) >= 0);
    CHECK(find(EV_LOST) >= 0);
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK_EQ_LONG(R.nsent, 0);             /* nothing answered */
}

static void test_disconnect_paths(void)
{
    /* Mid-turn: stop, keep acknowledging, /quit after >. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    clear_rec();
    proto_disconnect(&P, now);
    CHECK_MEM(R.sent, R.nsent, "!STOP\r");
    CHECK_EQ_LONG(P.state, ST_CLOSING);
    clear_rec();
    feed("T more\r\nW\r\n");
    proto_drawn(&P);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    clear_rec();
    feed("R interrupted $0.01\r\n>\r\n");
    CHECK_MEM(R.sent, R.nsent, "/quit\r");
    feed("I Bye.\r\nNO CARRIER\r\n");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_CLOSED) >= 0);
    CHECK_EQ_LONG(count(EV_LOST), 0);

    /* With the dialog open: Deny and !STOP first. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    feed("Q 4 Bash ls\r\n? 4\r\n");
    clear_rec();
    proto_disconnect(&P, now);
    CHECK_MEM(R.sent, R.nsent, "A 4 n\r!STOP\r");

    /* The turn never ends: hang up after the fallback time. */
    clear_rec();
    tick(T_STOP_TURN);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
    tick(T_GUARD * 2 + T_ATH);
    CHECK_MEM(R.sent, R.nsent, "+++ATH\r");
    CHECK_EQ_LONG(P.state, ST_IDLE);
    CHECK(find(EV_CLOSED) >= 0);

    /* Ready but the bridge does not drop after /quit: hang up after 3 s. */
    reset();
    connect_ready();
    proto_disconnect(&P, now);
    clear_rec();
    tick(T_QUIT + T_GUARD * 2);
    CHECK_MEM(R.sent, R.nsent, "+++ATH\r");

    /* During the handshake: hang up straight away. */
    reset();
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT\r\n");
    proto_disconnect(&P, now);
    CHECK_EQ_LONG(P.state, ST_HANGUP);
}

static void test_silence(void)
{
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    clear_rec();
    tick(T_SILENCE - 1);
    CHECK_EQ_LONG(count(EV_SILENCE), 0);
    tick(1);
    CHECK_EQ_LONG(count(EV_SILENCE), 1);
    CHECK_EQ_LONG(P.state, ST_BUSY);        /* silence is not a lost link */
    CHECK_EQ_LONG(R.nsent, 0);              /* no serial errors: no spare ACK */
    CHECK_EQ_LONG(proto_silence_secs(&P), 90);
    feed("T back\r\n");
    CHECK_EQ_LONG(proto_silence_secs(&P), 0);

    /* After line noise, one spare ACK per turn. */
    proto_line_noise(&P);
    clear_rec();
    tick(T_SILENCE);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    feed("T x\r\n");
    tick(T_SILENCE);
    CHECK_MEM(R.sent, R.nsent, "!ACK\r");
    CHECK_EQ_LONG(count(EV_SILENCE), 2);
}

static void test_prompt_guards(void)
{
    /* An empty prompt, or one sent outside Ready, sends nothing. */
    reset();
    CHECK_EQ_LONG(proto_send_prompt(&P, "hi", 2), 0);      /* Idle */
    proto_connect(&P, "ATDTbridge", now);
    clear_rec();
    CHECK_EQ_LONG(proto_send_prompt(&P, "hi", 2), 0);      /* Dialing */
    CHECK_EQ_LONG(R.nsent, 0);
    reset();
    connect_ready();
    CHECK_EQ_LONG(proto_send_prompt(&P, "", 0), 0);
    CHECK_EQ_LONG(R.nsent, 0);
    CHECK_EQ_LONG(proto_send_prompt(&P, "go", 2), 1);
    clear_rec();
    CHECK_EQ_LONG(proto_send_prompt(&P, "again", 5), 0);   /* Busy */
    feed("Q 3 Bash ls\r\n? 3\r\n");
    CHECK_EQ_LONG(P.state, ST_PERMISSION);
    CHECK_EQ_LONG(proto_send_prompt(&P, "y", 1), 0);       /* Permission */
    CHECK_EQ_LONG(R.nsent, 0);
}

static void test_acks_batched(void)
{
    /* Two W lines received before one draw get exactly two !ACKs. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "go", 2);
    feed("Q 8 Write a.txt\r\nW\r\nD[\r\nD x\r\nW\r\nD]\r\n");
    clear_rec();
    proto_drawn(&P);
    CHECK_EQ_LONG(count_sent("!ACK\r"), 2);
    clear_rec();
    proto_drawn(&P);
    CHECK_EQ_LONG(R.nsent, 0);
}

static void test_typed_quit(void)
{
    /* A typed /quit ends the session like Disconnect: the drop is normal. */
    reset();
    connect_ready();
    CHECK_EQ_LONG(proto_send_prompt(&P, "  /QUIT", 7), 1);
    CHECK_EQ_LONG(P.state, ST_CLOSING);
    feed("I Bye.\r\nNO CARRIER\r\n");
    CHECK(find(EV_CLOSED) >= 0);
    CHECK_EQ_LONG(count(EV_LOST), 0);
    /* No drop: the hangup runs after the quit wait. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "/quit", 5);
    tick(T_QUIT + T_GUARD + 2);
    CHECK(count_sent("+++") == 1);
    /* Words that only start with /quit are ordinary prompts. */
    reset();
    connect_ready();
    proto_send_prompt(&P, "/quitter", 8);
    CHECK_EQ_LONG(P.state, ST_BUSY);
}

int main(void)
{
    test_dial_and_handshake();
    test_dial_failures();
    test_handshake_timeout_hangs_up();
    test_demo_session();
    test_turn_details();
    test_long_permission_line();
    test_answers();
    test_stop();
    test_link_lost();
    test_disconnect_paths();
    test_silence();
    test_prompt_guards();
    test_acks_batched();
    test_typed_quit();
    proto_free(&P);
    CHECK_DONE();
}
