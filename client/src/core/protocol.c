#include "protocol.h"
#include "prompt.h"
#include <stdlib.h>
#include <string.h>

enum { AH_CLOSED, AH_FAILED, AH_DIAL };

static void emit(Proto *p, EventKind k, const char *t, long n)
{
    p->on_event(p->ctx, k, t ? t : "", t ? n : 0);
}

static void emits(Proto *p, EventKind k, const char *t)
{
    emit(p, k, t, (long)strlen(t));
}

static void sends(Proto *p, const char *s)
{
    p->send(p->ctx, s, (long)strlen(s));
}

static void set_state(Proto *p, SessState s)
{
    if (p->state != s) {
        p->state = s;
        emit(p, EV_STATE, "", 0);
    }
}

static void set_timer(Proto *p, unsigned long ticks)
{
    p->timer = p->now + ticks;
    if (p->timer == 0)
        p->timer = 1;
}

static void reset_turn(Proto *p)
{
    p->swallow = 0;
    p->last_kind = 'T';
    p->in_code = 0;
    p->perm_open = 0;
    p->perm_asked = 0;
    p->deny_on_ask = 0;
    p->stopped = 0;
    p->noise = 0;
    p->spare_ack_sent = 0;
    p->silence_warned = 0;
}

static void reset_session(Proto *p)
{
    reset_turn(p);
    p->pending_acks = 0;
    p->closing_turn = 0;
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
    linebuf_init(&p->lb);
    reset_session(p);
}

void proto_free(Proto *p)
{
    linebuf_free(&p->lb);
}

int proto_online(const Proto *p)
{
    return p->state == ST_HANDSHAKE || p->state == ST_READY || p->state == ST_BUSY
        || p->state == ST_PERMISSION || p->state == ST_CLOSING;
}

/* ---- modem ------------------------------------------------------------ */

static void start_dial(Proto *p)
{
    reset_session(p);
    p->dial_phase = 0;
    set_state(p, ST_DIALING);
    sends(p, "AT\r");
    set_timer(p, T_AT);
}

static void fail_to_idle(Proto *p, const char *msg)
{
    reset_session(p);
    set_state(p, ST_IDLE);
    emits(p, EV_DIAL_FAILED, msg);
}

/* Puts the modem on hook: guard, +++, guard, ATH. Nothing else is sent
   meanwhile, so the guard times stay silent. */
static void start_hangup(Proto *p, int after, const char *msg)
{
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
    reset_session(p);
    if (after == AH_DIAL) {
        start_dial(p);
        return;
    }
    set_state(p, ST_IDLE);
    if (after == AH_FAILED)
        emits(p, EV_DIAL_FAILED, p->fail_msg);
    else
        emit(p, EV_CLOSED, "", 0);
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
        sends(p, "\r#HELLO SE30 1\r");
        set_timer(p, T_HANDSHAKE);
        return;
    }
    fail = modem_failure(line, n);
    if (fail)
        fail_to_idle(p, fail);
}

static void handshake_line(Proto *p, const char *line, long n)
{
    if (equals(line, n, "#OK se30bridge 1")) {
        p->got_ok = 1;
        return;
    }
    if (p->got_ok && equals(line, n, ">")) {
        p->timer = 0;
        set_state(p, ST_READY);
        emit(p, EV_CONNECTED, "", 0);
        emit(p, EV_READY, "", 0);
        return;
    }
    if (starts(line, n, "NO CARRIER"))
        fail_to_idle(p, "The connection dropped before the bridge answered.");
}

static void hangup_line(Proto *p, const char *line, long n)
{
    if (p->hangup_step == 2 && (equals(line, n, "OK") || starts(line, n, "NO CARRIER")))
        finish_hangup(p);
}

/* ---- session ---------------------------------------------------------- */

static void send_answer(Proto *p, char c)
{
    char buf[32];
    size_t k = strlen(p->perm_id);
    memcpy(buf, "A ", 2);
    memcpy(buf + 2, p->perm_id, k);
    buf[2 + k] = ' ';
    buf[3 + k] = c;
    buf[4 + k] = '\r';
    p->send(p->ctx, buf, (long)(5 + k));
    p->perm_asked = 0;
    p->perm_open = 0;
}

static void send_stop(Proto *p)
{
    if (!p->stopped) {
        sends(p, "!STOP\r");
        p->stopped = 1;
    }
}

static void link_lost(Proto *p)
{
    if (p->perm_asked)
        emit(p, EV_PERM_CLOSE, "", 0);
    if (p->perm_asked || p->perm_open)
        emit(p, EV_PERM_ABANDONED, "", 0);
    if (p->in_code)
        emit(p, EV_CODE_END, "", 0);
    reset_session(p);
    set_state(p, ST_IDLE);
    emit(p, EV_LOST, "", 0);
}

static void turn_end(Proto *p)
{
    if (p->in_code)
        emit(p, EV_CODE_END, "", 0);
    if (p->stopped)
        emit(p, EV_INTERRUPTED, "", 0);
    reset_turn(p);
    if (p->closing_turn) {
        p->closing_turn = 0;
        sends(p, "/quit\r");
        p->quit_sent = 1;
        set_timer(p, T_QUIT);
        return;
    }
    if (p->state != ST_READY) {
        set_state(p, ST_READY);
        emit(p, EV_READY, "", 0);
    }
}

static void session_line(Proto *p, const char *line, long n)
{
    const char *pay = "";
    long plen = 0, tlen = 0;

    if (starts(line, n, "NO CARRIER")) {
        if (p->quit_sent || p->state == ST_CLOSING) {
            reset_session(p);
            set_state(p, ST_IDLE);
            emit(p, EV_CLOSED, "", 0);
        } else {
            link_lost(p);
        }
        return;
    }

    while (tlen < n && line[tlen] != ' ')
        tlen++;
    if (tlen < n) {
        pay = line + tlen + 1;
        plen = n - tlen - 1;
    }

#define TAG(s) (tlen == (long)sizeof(s) - 1 && memcmp(line, s, tlen) == 0)
    if (TAG("W")) {
        p->pending_acks++;
    } else if (TAG(">")) {
        if (p->swallow > 0)
            p->swallow--;
        else
            turn_end(p);
    } else if (TAG("T")) {
        p->last_kind = 'T';
        emit(p, EV_TEXT, pay, plen);
    } else if (TAG("+")) {
        emit(p, p->last_kind == 'C' ? EV_CODE_CONT : EV_TEXT_CONT, pay, plen);
    } else if (TAG("C[")) {
        p->in_code = 1;
        emit(p, EV_CODE_BEGIN, "", 0);
    } else if (TAG("C]")) {
        p->in_code = 0;
        emit(p, EV_CODE_END, "", 0);
    } else if (TAG("C")) {
        p->last_kind = 'C';
        emit(p, EV_CODE, pay, plen);
    } else if (TAG("U")) {
        emit(p, EV_TOOL, pay, plen);
    } else if (TAG("E")) {
        emit(p, EV_ERROR, pay, plen);
    } else if (TAG("I")) {
        emit(p, EV_INFO, pay, plen);
    } else if (TAG("R")) {
        emit(p, EV_RESULT, pay, plen);
    } else if (TAG("Q")) {
        long k = 0;
        while (k < plen && pay[k] != ' ' && k < (long)sizeof(p->perm_id) - 1)
            k++;
        memcpy(p->perm_id, pay, k);
        p->perm_id[k] = 0;
        p->perm_open = 1;
        p->perm_asked = 0;
        while (k < plen && pay[k] != ' ')
            k++;
        if (k < plen)
            k++;
        emit(p, EV_PERM_BEGIN, pay + k, plen - k);
    } else if (TAG("D[") || TAG("D]")) {
        /* body delimiters carry nothing to show */
    } else if (TAG("D")) {
        emit(p, EV_PERM_LINE, pay, plen);
    } else if (TAG("?")) {
        if (!p->perm_open || !equals(pay, plen, p->perm_id))
            return;              /* not the request we are holding */
        if (p->deny_on_ask || p->closing_turn) {
            send_answer(p, 'n');
            send_stop(p);
            p->deny_on_ask = 0;
            return;
        }
        p->perm_open = 0;
        p->perm_asked = 1;
        set_state(p, ST_PERMISSION);
        emit(p, EV_PERM_ASK, "", 0);
    } else if (TAG("#OK")) {
        /* already in app mode */
    } else if (n > 0) {
        emit(p, EV_UNKNOWN, line, n);
    }
#undef TAG
}

static void on_line(void *ctx, const char *line, long n)
{
    Proto *p = (Proto *)ctx;
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
        session_line(p, line, n);
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
    p->now = now;
    switch (p->state) {
    case ST_DIALING:
    case ST_HANDSHAKE:
        start_hangup(p, AH_CLOSED, NULL);
        break;
    case ST_READY:
        sends(p, "/quit\r");
        p->quit_sent = 1;
        set_state(p, ST_CLOSING);
        set_timer(p, T_QUIT);
        break;
    case ST_BUSY:
        p->closing_turn = 1;
        if (p->perm_open)
            p->deny_on_ask = 1;   /* answered when ? arrives */
        else
            send_stop(p);
        set_state(p, ST_CLOSING);
        set_timer(p, T_STOP_TURN);
        break;
    case ST_PERMISSION:
        send_answer(p, 'n');
        send_stop(p);
        p->closing_turn = 1;
        set_state(p, ST_CLOSING);
        set_timer(p, T_STOP_TURN);
        break;
    default:
        break;
    }
}

void proto_feed(Proto *p, const char *data, long n, unsigned long now)
{
    p->now = now;
    if (n > 0) {
        p->last_input = now;
        p->silence_warned = 0;
    }
    linebuf_feed(&p->lb, data, n, on_line, p);
}

void proto_tick(Proto *p, unsigned long now)
{
    p->now = now;
    if (p->state == ST_BUSY && !p->silence_warned
            && now - p->last_input >= (unsigned long)T_SILENCE) {
        p->silence_warned = 1;
        if (p->noise && !p->spare_ack_sent) {
            /* A W may have been garbled; one spare ACK releases it. */
            sends(p, "!ACK\r");
            p->spare_ack_sent = 1;
        }
        emit(p, EV_SILENCE, "", 0);
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
                     "The bridge did not answer. Is run.sh running on the host?");
        break;
    case ST_CLOSING:
        start_hangup(p, AH_CLOSED, NULL);
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

int proto_send_prompt(Proto *p, const char *text, long len)
{
    long cap = prompt_encoded_max(len), n;
    int lines = 0;
    char *buf;
    if (p->state != ST_READY || len <= 0)
        return 0;
    buf = (char *)malloc(cap);
    if (!buf)
        return 0;
    n = prompt_encode(text, len, buf, cap, &lines);
    if (n > 0)
        p->send(p->ctx, buf, n);
    free(buf);
    if (n <= 0)
        return 0;
    reset_turn(p);
    p->swallow = lines - 1;
    p->last_input = p->now;
    set_state(p, ST_BUSY);
    return 1;
}

void proto_stop(Proto *p)
{
    if (p->state == ST_BUSY) {
        if (p->perm_open)
            p->deny_on_ask = 1;
        else
            send_stop(p);
    } else if (p->state == ST_PERMISSION) {
        send_answer(p, 'n');
        send_stop(p);
        set_state(p, ST_BUSY);
        p->last_input = p->now;
    }
}

void proto_answer(Proto *p, char answer)
{
    if (p->state != ST_PERMISSION || !p->perm_asked)
        return;
    if (answer != 'y' && answer != 'n' && answer != 'a')
        return;
    send_answer(p, answer);
    set_state(p, ST_BUSY);
    p->last_input = p->now;
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
}

long proto_silence_secs(const Proto *p)
{
    if (p->state != ST_BUSY)
        return 0;
    return (long)((p->now - p->last_input) / TICKS_PER_SEC);
}
