#include "protocol.h"
#include "files.h"
#include "prompt.h"
#include <stdlib.h>
#include <string.h>

enum { AH_CLOSED, AH_FAILED, AH_DIAL, AH_LOST };

static void emit(Proto *p, int sid, EventKind k, const char *t, long n)
{
    p->on_event(p->ctx, sid, k, t ? t : "", t ? n : 0);
}

static void emits(Proto *p, int sid, EventKind k, const char *t)
{
    emit(p, sid, k, t, (long)strlen(t));
}

static void sends(Proto *p, const char *s)
{
    p->last_tx = p->now;
    p->send(p->ctx, s, (long)strlen(s));
}

/* Sends "<n><rest>" with a CR. */
static void send_sess(Proto *p, int sid, const char *rest)
{
    char buf[160];
    long k = (long)strlen(rest);
    if (k > (long)sizeof(buf) - 3)
        k = (long)sizeof(buf) - 3;
    buf[0] = (char)('0' + sid);
    memcpy(buf + 1, rest, k);
    buf[1 + k] = '\r';
    p->last_tx = p->now;
    p->send(p->ctx, buf, k + 2);
}

static void set_state(Proto *p, LinkState s)
{
    if (p->state != s) {
        p->state = s;
        emit(p, 0, EV_STATE, "", 0);
    }
}

static void set_sess(Proto *p, int sid, SessState s)
{
    if (p->sess[sid].state != s) {
        p->sess[sid].state = s;
        emit(p, sid, EV_STATE, "", 0);
    }
}

static void set_timer(Proto *p, unsigned long ticks)
{
    p->timer = p->now + ticks;
    if (p->timer == 0)
        p->timer = 1;
}

static void reset_turn(Session *s)
{
    s->last_kind = 'T';
    s->in_code = 0;
    s->perm_open = 0;
    s->perm_asked = 0;
    s->deny_on_ask = 0;
    s->stopped = 0;
}

/* A turn ended or was abandoned with subagent lines still open: they are over. */
static void close_agent_lines(Proto *p, int sid)
{
    Session *s = &p->sess[sid];
    if (s->agents_open > 0)
        emit(p, sid, EV_AGENT_END, "* stopped", 9);
    s->agents_open = 0;
}

static void reset_sessions(Proto *p)
{
    int i;
    for (i = 1; i <= MAX_SESS; i++) {
        memset(&p->sess[i], 0, sizeof(Session));
        p->sess[i].state = SS_NONE;
        p->sess[i].rx = -1;
        reset_turn(&p->sess[i]);
    }
}

static void reset_link(Proto *p)
{
    files_fail(p, 0, "The link was lost.");
    reset_sessions(p);
    p->pending_acks = 0;
    p->noise = 0;
    p->spare_ack_sent = 0;
    p->silence_warned = 0;
    p->ping_out = 0;
    p->resume_cap = 0;
    p->ping_cap = 0;
    p->agents_cap = 0;
    p->quit_sent = 0;
    p->got_ok = 0;
    p->timer = 0;
    linebuf_reset(&p->lb);
}

void proto_init(Proto *p, ProtoEventFn ev, ProtoSendFn send, void *ctx)
{
    memset(p, 0, sizeof(*p));
    p->on_event = ev;
    p->send = send;
    p->ctx = ctx;
    p->state = ST_IDLE;
    p->cur_xfer = -1;
    p->next_num = 1;
    linebuf_init(&p->lb);
    reset_link(p);
}

void proto_free(Proto *p)
{
    int i;
    linebuf_free(&p->lb);
    for (i = 0; i < MAX_XFER; i++) {
        free(p->xfer[i].buf);
        free(p->xfer[i].text);
    }
    free(p->proj);
    p->proj = NULL;
}

int proto_online(const Proto *p)
{
    return p->state == ST_HANDSHAKE || p->state == ST_ONLINE || p->state == ST_CLOSING;
}

SessState proto_sess_state(const Proto *p, int sid)
{
    if (sid < 1 || sid > MAX_SESS)
        return SS_NONE;
    return p->sess[sid].state;
}

int proto_any_busy(const Proto *p)
{
    int i;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].state == SS_BUSY)
            return 1;
    return 0;
}

int proto_files_mode(const Proto *p)
{
    return p->files;
}

/* ---- project list ------------------------------------------------------- */

static void projects_reset(Proto *p)
{
    p->proj_len = 0;
    p->nproj = 0;
    p->proj_complete = 0;
}

static void projects_add(Proto *p, const char *name, long n)
{
    if (p->nproj >= MAX_PROJECTS || n <= 0)
        return;
    if (p->proj_len + n + 1 > p->proj_cap) {
        long cap = p->proj_cap ? p->proj_cap * 2 : 512;
        char *np;
        while (cap < p->proj_len + n + 1)
            cap *= 2;
        np = (char *)realloc(p->proj, cap);
        if (!np)
            return;
        p->proj = np;
        p->proj_cap = cap;
    }
    p->proj_off[p->nproj++] = p->proj_len;
    memcpy(p->proj + p->proj_len, name, n);
    p->proj[p->proj_len + n] = 0;
    p->proj_len += n + 1;
}

int proto_nprojects(const Proto *p)
{
    return p->nproj;
}

const char *proto_project(const Proto *p, int i)
{
    if (i < 0 || i >= p->nproj)
        return "";
    return p->proj + p->proj_off[i];
}

void proto_list_projects(Proto *p)
{
    if (p->state == ST_ONLINE)
        sends(p, "!LIST\r");
}

/* ---- modem ------------------------------------------------------------ */

static void start_dial(Proto *p)
{
    reset_link(p);
    p->dial_phase = 0;
    set_state(p, ST_DIALING);
    sends(p, "AT\r");
    set_timer(p, T_AT);
}

static void fail_to_idle(Proto *p, const char *msg)
{
    reset_link(p);
    set_state(p, ST_IDLE);
    emits(p, 0, EV_DIAL_FAILED, msg);
}

/* Puts the modem on hook: guard, +++, guard, ATH. Nothing else is sent
   meanwhile, so the guard times stay silent. */
static void start_hangup(Proto *p, int after, const char *msg)
{
    files_fail(p, 0, "The link was lost.");
    p->after_hangup = after;
    strncpy(p->fail_msg, msg ? msg : "", sizeof(p->fail_msg) - 1);
    p->fail_msg[sizeof(p->fail_msg) - 1] = 0;
    p->hangup_step = 0;
    set_state(p, ST_HANGUP);
    set_timer(p, T_GUARD);
}

static void finish_hangup(Proto *p)
{
    int after = p->after_hangup;
    reset_link(p);
    if (after == AH_DIAL) {
        start_dial(p);
        return;
    }
    set_state(p, ST_IDLE);
    if (after == AH_FAILED)
        emits(p, 0, EV_DIAL_FAILED, p->fail_msg);
    else if (after == AH_LOST)
        emit(p, 0, EV_LOST, "", 0);
    else
        emit(p, 0, EV_CLOSED, "", 0);
}

static int starts(const char *line, long n, const char *pfx)
{
    long k = (long)strlen(pfx);
    return n >= k && memcmp(line, pfx, k) == 0;
}

static int equals(const char *line, long n, const char *s)
{
    return n == (long)strlen(s) && memcmp(line, s, n) == 0;
}

static const char *modem_failure(const char *line, long n)
{
    if (starts(line, n, "NO CARRIER"))
        return "The modem reported NO CARRIER. Is run.sh running on the host?";
    if (starts(line, n, "BUSY"))
        return "The modem reported BUSY.";
    if (starts(line, n, "NO ANSWER"))
        return "The modem reported NO ANSWER. Is run.sh running on the host?";
    if (starts(line, n, "NO DIALTONE"))
        return "The modem reported NO DIALTONE. Is the WiFi232 on the network?";
    if (starts(line, n, "ERROR"))
        return "The modem reported ERROR. Check the dial string in Settings.";
    return NULL;
}

static void dialing_line(Proto *p, const char *line, long n)
{
    const char *fail;
    if (p->dial_phase == 0) {
        if (equals(line, n, "OK")) {
            p->dial_phase = 1;
            sends(p, p->dial);
            sends(p, "\r");
            set_timer(p, T_DIAL);
        }
        return;
    }
    if (starts(line, n, "CONNECT")) {
        set_state(p, ST_HANDSHAKE);
        /* A bare CR first, so leftover input cannot become part of HELLO. */
        sends(p, "\r#HELLO SE30 2\r");
        set_timer(p, T_HANDSHAKE);
        return;
    }
    fail = modem_failure(line, n);
    if (fail)
        fail_to_idle(p, fail);
}

/* "#OK se30bridge 2", alone or followed by capability words. */
static void read_ok(Proto *p, const char *line, long n)
{
    long i = 16;
    p->got_ok = 1;
    p->files = FILES_OFF;
    p->resume_cap = 0;
    p->ping_cap = 0;
    p->agents_cap = 0;
    while (i < n) {
        long w;
        while (i < n && line[i] == ' ')
            i++;
        w = i;
        while (i < n && line[i] != ' ')
            i++;
        if (equals(line + w, i - w, "files=rw"))
            p->files = FILES_RW;
        else if (equals(line + w, i - w, "files=ro"))
            p->files = FILES_RO;
        else if (equals(line + w, i - w, "resume"))
            p->resume_cap = 1;
        else if (equals(line + w, i - w, "ping"))
            p->ping_cap = 1;
        else if (equals(line + w, i - w, "agents"))
            p->agents_cap = 1;
    }
}

/* Link-level lines: W, the project list, I. Returns 1 if it was one. */
static int link_line(Proto *p, const char *line, long n)
{
    if (equals(line, n, "W")) {
        p->pending_acks++;
        p->noise = 0;            /* a W arrived intact: no ACK is owed for it */
        return 1;
    }
    if (equals(line, n, "P[")) {
        projects_reset(p);
        return 1;
    }
    if (starts(line, n, "P ")) {
        projects_add(p, line + 2, n - 2);
        return 1;
    }
    if (equals(line, n, "P]")) {
        p->proj_complete = 1;
        if (p->state == ST_HANDSHAKE) {
            p->timer = 0;
            set_state(p, ST_ONLINE);
            if (p->agents_cap)
                sends(p, "!AGENTS\r");   /* draw subagents as live lines */
            emit(p, 0, EV_CONNECTED, "", 0);
        } else {
            emit(p, 0, EV_PROJECTS, "", 0);
        }
        return 1;
    }
    if (starts(line, n, "I ")) {
        emit(p, 0, EV_INFO, line + 2, n - 2);
        return 1;
    }
    if (equals(line, n, "#PONG")) {
        p->ping_out = 0;
        p->feed_pong = 1;
        return 1;
    }
    return 0;
}

static void handshake_line(Proto *p, const char *line, long n)
{
    if (starts(line, n, "#OK se30bridge 2") && (n == 16 || line[16] == ' ')) {
        read_ok(p, line, n);
        return;
    }
    if (starts(line, n, "NO CARRIER")) {
        fail_to_idle(p, "The connection dropped before the bridge answered.");
        return;
    }
    if (p->got_ok)
        link_line(p, line, n);
}

static void hangup_line(Proto *p, const char *line, long n)
{
    if (p->hangup_step == 2 && (equals(line, n, "OK") || starts(line, n, "NO CARRIER")))
        finish_hangup(p);
}

/* ---- sessions ----------------------------------------------------------- */

static void send_answer(Proto *p, int sid, char c)
{
    Session *s = &p->sess[sid];
    char buf[32];
    size_t k = strlen(s->perm_id);
    memcpy(buf, "A ", 2);
    memcpy(buf + 2, s->perm_id, k);
    buf[2 + k] = ' ';
    buf[3 + k] = c;
    buf[4 + k] = 0;
    send_sess(p, sid, buf);
    s->perm_asked = 0;
    s->perm_open = 0;
}

static void send_stop(Proto *p, int sid)
{
    if (!p->sess[sid].stopped) {
        send_sess(p, sid, "!STOP");
        p->sess[sid].stopped = 1;
    }
}

/* Closes what a session showed when it ends without its turn finishing. */
static void abandon_turn(Proto *p, int sid)
{
    Session *s = &p->sess[sid];
    if (s->perm_asked)
        emit(p, sid, EV_PERM_CLOSE, "", 0);
    if (s->perm_asked || s->perm_open)
        emit(p, sid, EV_PERM_ABANDONED, "", 0);
    if (s->in_code)
        emit(p, sid, EV_CODE_END, "", 0);
    close_agent_lines(p, sid);
    reset_turn(s);
}

static void link_lost(Proto *p)
{
    int i;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].state != SS_NONE)
            abandon_turn(p, i);
    reset_link(p);
    set_state(p, ST_IDLE);
    emit(p, 0, EV_LOST, "", 0);
}

/* Nothing heard for T_DEAD and a !PING went unanswered: the modem may still
   be in data mode, so put it on hook first, then report the loss. */
static void link_dead(Proto *p)
{
    int i;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].state != SS_NONE)
            abandon_turn(p, i);
    start_hangup(p, AH_LOST, NULL);
}

/* Disconnecting: once no turn is left to finish, ask the bridge to quit. */
static void maybe_quit(Proto *p)
{
    int i;
    if (p->state != ST_CLOSING || p->quit_sent)
        return;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].closing_turn)
            return;
    sends(p, "!QUIT\r");
    p->quit_sent = 1;
    set_timer(p, T_QUIT);
}

static void turn_end(Proto *p, int sid)
{
    Session *s = &p->sess[sid];
    if (s->state == SS_OPENING) {
        /* The first > after <n>O: the session takes prompts from now on. */
        set_sess(p, sid, SS_READY);
        emit(p, sid, EV_READY, "", 0);
        return;
    }
    if (s->state != SS_BUSY && s->state != SS_PERMISSION)
        return;
    if (s->in_code)
        emit(p, sid, EV_CODE_END, "", 0);
    close_agent_lines(p, sid);
    if (s->stopped)
        emit(p, sid, EV_INTERRUPTED, "", 0);
    reset_turn(s);
    set_sess(p, sid, SS_READY);
    if (s->closing_turn) {
        s->closing_turn = 0;
        maybe_quit(p);
    }
    emit(p, sid, EV_READY, "", 0);
}

static void session_x(Proto *p, int sid, const char *pay, long plen)
{
    Session *s = &p->sess[sid];
    SessState was = s->state;
    if (was == SS_OPENING) {
        reset_turn(s);
        s->state = SS_NONE;
        emit(p, sid, EV_OPEN_FAILED, pay, plen);
        return;
    }
    abandon_turn(p, sid);
    files_fail(p, sid, "The tab was closed.");
    s->closing_turn = 0;
    s->state = SS_NONE;
    emit(p, sid, EV_SESSION_CLOSED, pay, plen);
    maybe_quit(p);
}

static void session_line(Proto *p, int sid, const char *line, long n)
{
    Session *s = &p->sess[sid];
    const char *pay = "";
    long plen = 0, tlen = 0;

    if (s->state == SS_NONE) {
        p->dropped++;            /* no such session: probably a garbled digit */
        p->noise = 1;
        return;
    }
    while (tlen < n && line[tlen] != ' ')
        tlen++;
    if (tlen < n) {
        pay = line + tlen + 1;
        plen = n - tlen - 1;
    }

#define TAG(x) (tlen == (long)sizeof(x) - 1 && memcmp(line, x, tlen) == 0)
    if (TAG(">")) {
        turn_end(p, sid);
    } else if (TAG("T")) {
        s->last_kind = 'T';
        emit(p, sid, EV_TEXT, pay, plen);
    } else if (TAG("+")) {
        if (s->last_kind == 'B')
            files_line(p, sid, line, tlen, pay, plen);
        else
            emit(p, sid, s->last_kind == 'C' ? EV_CODE_CONT : EV_TEXT_CONT, pay, plen);
    } else if (TAG("C[")) {
        s->in_code = 1;
        emit(p, sid, EV_CODE_BEGIN, "", 0);
    } else if (TAG("C]")) {
        s->in_code = 0;
        emit(p, sid, EV_CODE_END, "", 0);
    } else if (TAG("C")) {
        s->last_kind = 'C';
        emit(p, sid, EV_CODE, pay, plen);
    } else if (TAG("U")) {
        emit(p, sid, EV_TOOL, pay, plen);
    } else if (TAG("E")) {
        emit(p, sid, EV_ERROR, pay, plen);
    } else if (TAG("I")) {
        emit(p, sid, EV_INFO, pay, plen);
    } else if (TAG("R")) {
        emit(p, sid, EV_RESULT, pay, plen);
    } else if (TAG("Q")) {
        long k = 0;
        while (k < plen && pay[k] != ' ' && k < (long)sizeof(s->perm_id) - 1)
            k++;
        memcpy(s->perm_id, pay, k);
        s->perm_id[k] = 0;
        s->perm_open = 1;
        s->perm_asked = 0;
        while (k < plen && pay[k] != ' ')
            k++;
        if (k < plen)
            k++;
        emit(p, sid, EV_PERM_BEGIN, pay + k, plen - k);
    } else if (TAG("D[") || TAG("D]")) {
        /* body delimiters carry nothing to show */
    } else if (TAG("D")) {
        emit(p, sid, EV_PERM_LINE, pay, plen);
    } else if (TAG("?")) {
        if (!s->perm_open || !equals(pay, plen, s->perm_id))
            return;              /* not the request we are holding */
        if (s->deny_on_ask || s->closing_turn || s->state == SS_CLOSING) {
            send_answer(p, sid, 'n');
            send_stop(p, sid);
            s->deny_on_ask = 0;
            return;
        }
        s->perm_open = 0;
        s->perm_asked = 1;
        set_sess(p, sid, SS_PERMISSION);
        emit(p, sid, EV_PERM_ASK, "", 0);
    } else if (TAG("Y")) {
        /* "<token> <new|resumed|cut>": which conversation this session runs. */
        long k = 0;
        while (k < plen && pay[k] != ' ')
            k++;
        if (k > 0 && k <= 16 && k + 1 < plen) {
            char text[48];
            long hn = plen - k - 1;
            if (hn > 8)
                hn = 8;
            memcpy(text, pay + k + 1, hn);
            text[hn] = ' ';
            memcpy(text + hn + 1, pay, k);
            emit(p, sid, EV_RESUMED, text, hn + 1 + k);
        }
    } else if (TAG("J+")) {
        s->agents_open++;
        emit(p, sid, EV_AGENT_START, pay, plen);
    } else if (TAG("J~")) {
        emit(p, sid, EV_AGENT_STEP, pay, plen);
    } else if (TAG("J-")) {
        if (s->agents_open > 0)
            s->agents_open--;
        emit(p, sid, EV_AGENT_END, pay, plen);
    } else if (TAG("O")) {
        /* Stay in SS_OPENING until the first >, so that > cannot end a
           turn the shell starts in between. */
        if (s->state == SS_OPENING)
            emit(p, sid, EV_OPENED, pay, plen);
    } else if (TAG("X")) {
        /* A late "closed" for an earlier session with this id is not a
           refusal of the one now opening. */
        if (!(s->state == SS_OPENING && equals(pay, plen, "closed")))
            session_x(p, sid, pay, plen);
    } else if (TAG("L[") || TAG("L") || TAG("L]") || TAG("H") || TAG("B")
               || TAG("Z") || TAG("K") || TAG("N")) {
        if (TAG("B"))
            s->last_kind = 'B';
        files_line(p, sid, line, tlen, pay, plen);
    }
    /* Any other session tag is one this version does not know: ignored. */
#undef TAG
}

static void online_line(Proto *p, const char *line, long n)
{
    if (starts(line, n, "NO CARRIER")) {
        if (p->quit_sent || p->state == ST_CLOSING) {
            reset_link(p);
            set_state(p, ST_IDLE);
            emit(p, 0, EV_CLOSED, "", 0);
        } else {
            link_lost(p);
        }
        return;
    }
    if (n > 0 && line[0] >= '1' && line[0] <= '0' + MAX_SESS) {
        session_line(p, line[0] - '0', line + 1, n - 1);
        return;
    }
    if (n > 0 && !link_line(p, line, n) && !starts(line, n, "#OK")) {
        p->dropped++;
        p->noise = 1;
    }
}

static void on_line(void *ctx, const char *line, long n)
{
    Proto *p = (Proto *)ctx;
    if (!equals(line, n, "#PONG"))
        p->feed_real = 1;
    switch (p->state) {
    case ST_IDLE:
        break;
    case ST_DIALING:
        dialing_line(p, line, n);
        break;
    case ST_HANDSHAKE:
        handshake_line(p, line, n);
        break;
    case ST_HANGUP:
        hangup_line(p, line, n);
        break;
    default:
        online_line(p, line, n);
        break;
    }
}

/* ---- public ----------------------------------------------------------- */

void proto_connect(Proto *p, const char *dial, unsigned long now)
{
    p->now = now;
    if (p->state != ST_IDLE)
        return;
    strncpy(p->dial, dial, sizeof(p->dial) - 1);
    p->dial[sizeof(p->dial) - 1] = 0;
    p->at_retry = 0;
    start_dial(p);
}

void proto_disconnect(Proto *p, unsigned long now)
{
    int i, waiting = 0;
    p->now = now;
    switch (p->state) {
    case ST_DIALING:
    case ST_HANDSHAKE:
        start_hangup(p, AH_CLOSED, NULL);
        break;
    case ST_ONLINE:
        files_fail(p, 0, "Disconnected.");
        for (i = 1; i <= MAX_SESS; i++) {
            Session *s = &p->sess[i];
            if (s->state == SS_PERMISSION) {
                send_answer(p, i, 'n');
                send_stop(p, i);
                s->closing_turn = 1;
                set_sess(p, i, SS_BUSY);
                waiting = 1;
            } else if (s->state == SS_BUSY) {
                s->closing_turn = 1;
                if (s->perm_open)
                    s->deny_on_ask = 1;   /* answered when ? arrives */
                else
                    send_stop(p, i);
                waiting = 1;
            }
        }
        set_state(p, ST_CLOSING);
        if (waiting)
            set_timer(p, T_STOP_TURN);
        else
            maybe_quit(p);
        break;
    default:
        break;
    }
}

void proto_feed(Proto *p, const char *data, long n, unsigned long now)
{
    int was_warned = p->silence_warned;
    p->now = now;
    p->prev_input = p->last_input;
    p->feed_real = p->feed_pong = 0;
    if (n > 0) {
        p->last_heard = now;
        p->last_input = now;
        p->silence_warned = 0;
    }
    linebuf_feed(&p->lb, data, n, on_line, p);
    /* A heartbeat answer alone shows the link is up; it says nothing about
       Claude, so it must not reset the "no output" timer. */
    if (p->feed_pong && !p->feed_real && p->lb.len == 0) {
        p->last_input = p->prev_input;
        p->silence_warned = was_warned;
    }
}

void proto_tick(Proto *p, unsigned long now)
{
    int i;
    p->now = now;
    if (proto_any_busy(p) && !p->silence_warned
            && now - p->last_input >= (unsigned long)T_SILENCE) {
        p->silence_warned = 1;
        if (p->noise && !p->spare_ack_sent) {
            /* A W may have been garbled; one spare ACK releases it. */
            sends(p, "!ACK\r");
            p->spare_ack_sent = 1;
        }
        emit(p, 0, EV_SILENCE, "", 0);
    }
    if (p->state == ST_ONLINE && p->ping_cap) {
        if (now - p->last_heard >= (unsigned long)T_DEAD && p->ping_out
                && now - p->last_ping >= (unsigned long)T_PING_WAIT) {
            link_dead(p);
            return;
        }
        if (now - p->last_tx >= (unsigned long)T_PING
                || (now - p->last_heard >= (unsigned long)T_DEAD && !p->ping_out)) {
            sends(p, "!PING\r");
            p->last_ping = now;
            p->ping_out = 1;
        }
    }
    if (p->state == ST_ONLINE || p->state == ST_CLOSING) {
        for (i = 1; i <= MAX_SESS; i++) {
            Session *s = &p->sess[i];
            if (s->state == SS_CLOSING && (long)(now - s->close_by) >= 0)
                session_x(p, i, "closed", 6);   /* the answer never came */
        }
        files_tick(p);
    }
    if (!p->timer || (long)(now - p->timer) < 0)
        return;
    p->timer = 0;
    switch (p->state) {
    case ST_DIALING:
        if (p->dial_phase == 0 && !p->at_retry) {
            /* No OK: the modem may still be online. Hang up and try again. */
            p->at_retry = 1;
            start_hangup(p, AH_DIAL, NULL);
        } else if (p->dial_phase == 0) {
            fail_to_idle(p, "The modem does not answer. Check the port and cable.");
        } else {
            start_hangup(p, AH_FAILED, "The call was not answered in time.");
        }
        break;
    case ST_HANDSHAKE:
        start_hangup(p, AH_FAILED,
                     "No answer from the bridge. It may be older than this app: "
                     "update bridge.py on the host.");
        break;
    case ST_CLOSING:
        if (!p->quit_sent) {
            /* A turn never ended: quit anyway, then hang up if that fails. */
            for (i = 1; i <= MAX_SESS; i++)
                p->sess[i].closing_turn = 0;
            maybe_quit(p);
        } else {
            start_hangup(p, AH_CLOSED, NULL);
        }
        break;
    case ST_HANGUP:
        if (p->hangup_step == 0) {
            sends(p, "+++");
            p->hangup_step = 1;
            set_timer(p, T_GUARD);
        } else if (p->hangup_step == 1) {
            sends(p, "ATH\r");
            p->hangup_step = 2;
            set_timer(p, T_ATH);
        } else {
            finish_hangup(p);
        }
        break;
    default:
        break;
    }
}

int proto_open_resume(Proto *p, int sid, const char *project, const char *token)
{
    char buf[140];
    long k = (long)strlen(project), t = token ? (long)strlen(token) : 0;
    long at = 2;
    if (p->state != ST_ONLINE || sid < 1 || sid > MAX_SESS || p->sess[sid].state != SS_NONE)
        return 0;
    if (!p->resume_cap || t == 0 || t > 16)
        t = 0;
    if (k + t + 4 > (long)sizeof(buf) - 3)
        return 0;
    buf[0] = t ? 'Y' : 'O';
    buf[1] = ' ';
    if (t) {
        memcpy(buf + at, token, t);
        at += t;
        buf[at++] = ' ';
    }
    memcpy(buf + at, project, k + 1);
    send_sess(p, sid, buf);
    reset_turn(&p->sess[sid]);
    p->sess[sid].closing_turn = 0;
    set_sess(p, sid, SS_OPENING);
    return 1;
}

int proto_open(Proto *p, int sid, const char *project)
{
    return proto_open_resume(p, sid, project, NULL);
}

int proto_close(Proto *p, int sid)
{
    Session *s;
    if (sid < 1 || sid > MAX_SESS)
        return 0;
    s = &p->sess[sid];
    if (s->state == SS_NONE || s->state == SS_CLOSING || p->state != ST_ONLINE)
        return 0;
    send_sess(p, sid, "X");
    files_fail(p, sid, "The tab was closed.");
    s->perm_asked = 0;          /* the bridge denies it; nothing to answer */
    s->close_by = p->now + T_CLOSE;
    set_sess(p, sid, SS_CLOSING);
    return 1;
}

/* True when the bridge would read this prompt as /quit: it lowercases the
   prompt and compares its first whitespace-separated word. */
int proto_is_quit(const char *t, long len)
{
    static const char q[] = "/quit";
    long i = 0, k;
    while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n'))
        i++;
    if (len - i < 5)
        return 0;
    for (k = 0; k < 5; k++) {
        char c = t[i + k];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != q[k])
            return 0;
    }
    i += 5;
    return i == len || t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n';
}

int proto_send_prompt(Proto *p, int sid, const char *text, long len)
{
    char prefix[4];
    long cap, n;
    char *buf;
    if (sid < 1 || sid > MAX_SESS || p->state != ST_ONLINE
            || p->sess[sid].state != SS_READY || len <= 0 || proto_is_quit(text, len))
        return 0;
    prefix[0] = (char)('0' + sid);
    prefix[1] = 'P';
    prefix[2] = ' ';
    prefix[3] = 0;
    cap = prompt_encoded_max(len, 3);
    buf = (char *)malloc(cap);
    if (!buf)
        return 0;
    n = prompt_encode(text, len, prefix, buf, cap, NULL);
    if (n > 0) {
        p->last_tx = p->now;
        p->send(p->ctx, buf, n);
    }
    free(buf);
    if (n <= 0)
        return 0;
    reset_turn(&p->sess[sid]);
    p->last_input = p->now;
    set_sess(p, sid, SS_BUSY);
    return 1;
}

void proto_stop(Proto *p, int sid)
{
    Session *s;
    if (sid < 1 || sid > MAX_SESS)
        return;
    s = &p->sess[sid];
    if (s->state == SS_BUSY) {
        if (s->perm_open)
            s->deny_on_ask = 1;
        else
            send_stop(p, sid);
    } else if (s->state == SS_PERMISSION) {
        send_answer(p, sid, 'n');
        send_stop(p, sid);
        set_sess(p, sid, SS_BUSY);
        p->last_input = p->now;
    }
}

int proto_answer(Proto *p, int sid, char answer)
{
    Session *s;
    if (sid < 1 || sid > MAX_SESS)
        return 0;
    s = &p->sess[sid];
    if (s->state != SS_PERMISSION || !s->perm_asked)
        return 0;
    if (answer != 'y' && answer != 'n' && answer != 'a')
        return 0;
    send_answer(p, sid, answer);
    set_sess(p, sid, SS_BUSY);
    p->last_input = p->now;
    return 1;
}

void proto_drawn(Proto *p)
{
    if (p->state == ST_HANGUP || p->state == ST_IDLE) {
        p->pending_acks = 0;
        return;
    }
    while (p->pending_acks > 0) {
        sends(p, "!ACK\r");
        p->pending_acks--;
    }
}

void proto_line_noise(Proto *p)
{
    p->noise = 1;
    p->noise_count++;
}

long proto_silence_secs(const Proto *p)
{
    if (!proto_any_busy(p))
        return 0;
    return (long)((p->now - p->last_input) / TICKS_PER_SEC);
}
