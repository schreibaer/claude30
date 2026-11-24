#include "modem.h"
#include "batch.h"
#include "log.h"
#include "permdlg.h"
#include "prefs.h"
#include "resources.h"
#include "serial.h"
#include "window.h"
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <TextUtils.h>
#include <string.h>
#include <stdio.h>

#define READ_CHUNK   4096
#define DRAIN_LIMIT  (16L * 1024)   /* per pass, so the UI stays responsive */
#define LINK_PROMPT  "link test"
#define kPortInUseErr (-97)      /* portInUse: another program has the driver open */

static Proto gProto;
static Batch gBatch;
static LinkStats gLink;

static Boolean gWantPerm;        /* open the permission dialog */
static short gAlert;             /* 0, 1 lost link, 2 failed call */
static char gAlertMsg[96];
static Boolean gNoiseSeen;       /* serial errors this session */
static char gLastTool[64];
static Boolean gLogTrouble;      /* the log could not be written this session */
static long gDamagedSeen;        /* gProto.lb.damaged already reported */
static Boolean gLinkSendNext;
static Boolean gLinkSession;    /* this call is a link test, until it ends */

enum { kAlertNone, kAlertLost, kAlertFailed };

/* ---- core callbacks ------------------------------------------------------ */

static void on_send(void *ctx, const char *data, long len)
{
    (void)ctx;
    serial_write(data, len);
}

static void copy_tool(const char *t, long n)
{
    if (n > (long)sizeof(gLastTool) - 1)
        n = sizeof(gLastTool) - 1;
    memcpy(gLastTool, t, n);
    gLastTool[n] = 0;
}

static void divider(void)
{
    unsigned long secs;
    Str255 date, time;
    char line[96];
    GetDateTime(&secs);
    IUDateString(secs, shortDate, date);
    IUTimeString(secs, false, time);
    snprintf(line, sizeof(line), "New conversation, %.*s %.*s",
             date[0], (char *)date + 1, time[0], (char *)time + 1);
    batch_marker(&gBatch, RS_DIVIDER, line);
}

static void linktest_event(EventKind k, const char *t, long n)
{
    switch (k) {
    case EV_STATE:
        break;
    case EV_PERM_ASK:
        proto_answer(&gProto, 'n');        /* the demo asks once per turn */
        gLink.lines++;
        break;
    case EV_READY:
        gLink.turns++;
        gLinkSendNext = true;
        break;
    case EV_DIAL_FAILED:
    case EV_LOST:
        gLink.failed = true;
        if (k == EV_LOST) {
            t = "The connection was lost.";
            n = (long)strlen(t);
        }
        if (n > (long)sizeof(gLink.failure) - 1)
            n = sizeof(gLink.failure) - 1;
        memcpy(gLink.failure, t, n);
        gLink.failure[n] = 0;
        gLink.active = false;
        break;
    case EV_CLOSED:
        gLink.active = false;
        break;
    default:
        gLink.lines++;
        break;
    }
}

static void on_event(void *ctx, EventKind k, const char *t, long n)
{
    (void)ctx;
    if (gLinkSession) {
        linktest_event(k, t, n);
        return;
    }
    switch (k) {
    case EV_CONNECTED:
        log_new_session();
        gNoiseSeen = false;
        gLogTrouble = false;
        gLastTool[0] = 0;
        if (!window_transcript_empty() || gBatch.nruns)
            divider();
        break;
    case EV_TOOL:
        copy_tool(t, n);
        batch_event(&gBatch, k, t, n);
        break;
    case EV_PERM_BEGIN:
        permdlg_begin(t, n);
        batch_event(&gBatch, k, t, n);
        break;
    case EV_PERM_LINE:
        permdlg_line(t, n);
        break;
    case EV_PERM_ASK:
        gWantPerm = true;
        break;
    case EV_PERM_CLOSE:
        gWantPerm = false;
        permdlg_request_close();
        break;
    case EV_LOST:
        batch_marker(&gBatch, RS_ERROR, "The connection was lost.");
        gAlert = kAlertLost;
        strcpy(gAlertMsg, "The connection to the bridge was lost.");
        break;
    case EV_DIAL_FAILED:
        gAlert = kAlertFailed;
        if (n > (long)sizeof(gAlertMsg) - 1)
            n = sizeof(gAlertMsg) - 1;
        memcpy(gAlertMsg, t, n);
        gAlertMsg[n] = 0;
        break;
    default:
        batch_event(&gBatch, k, t, n);
        break;
    }
}

/* ---- status line --------------------------------------------------------- */

static void update_status(void)
{
    char s[128];
    long quiet;
    switch (gProto.state) {
    case ST_IDLE:
        strcpy(s, "Not connected");
        break;
    case ST_DIALING:
        strcpy(s, "Dialing\xC9");
        break;
    case ST_HANDSHAKE:
        strcpy(s, "Connected; waiting for the bridge\xC9");
        break;
    case ST_READY:
        snprintf(s, sizeof(s), "Connected at %s. Type a prompt and press Return.",
                 serial_speed_name(gPrefs.speed));
        break;
    case ST_BUSY:
        quiet = proto_silence_secs(&gProto);
        if (quiet >= T_SILENCE / TICKS_PER_SEC)
            snprintf(s, sizeof(s), "No output for %ld:%02ld. Cmd-period stops; Disconnect hangs up.",
                     quiet / 60, quiet % 60);
        else if (gLastTool[0])
            snprintf(s, sizeof(s), "Working: %s", gLastTool);
        else
            strcpy(s, "Claude is working. Cmd-period stops.");
        break;
    case ST_PERMISSION:
        strcpy(s, "Waiting for your answer to a permission request");
        break;
    case ST_CLOSING:
        strcpy(s, "Disconnecting\xC9");
        break;
    case ST_HANGUP:
        strcpy(s, "Hanging up\xC9");
        break;
    }
    if (gNoiseSeen && proto_online(&gProto))
        strncat(s, " (line noise seen)", sizeof(s) - strlen(s) - 1);
    if (gLogTrouble && proto_online(&gProto))
        strncat(s, " (log not saving)", sizeof(s) - strlen(s) - 1);
    window_set_status(s);
    window_set_send_enabled(gProto.state == ST_READY && !gLink.active);
}

/* ---- public -------------------------------------------------------------- */

void session_init(void)
{
    proto_init(&gProto, on_event, on_send, NULL);
    batch_init(&gBatch);
}

static Boolean open_port(void)
{
    OSErr err = serial_open(gPrefs.port, gPrefs.speed);
    if (err) {
        gAlert = kAlertFailed;
        if (err == kPortInUseErr)
            snprintf(gAlertMsg, sizeof(gAlertMsg), "The %s port is in use by another program.",
                     gPrefs.port == kPortPrinter ? "printer" : "modem");
        else
            snprintf(gAlertMsg, sizeof(gAlertMsg), "The %s port could not be opened (error %d).",
                     gPrefs.port == kPortPrinter ? "printer" : "modem", err);
        return false;
    }
    return true;
}

void session_connect(void)
{
    char dial[64];
    if (gProto.state != ST_IDLE)
        return;
    if (!open_port())
        return;
    prefs_dial_cstr(dial, sizeof(dial));
    proto_connect(&gProto, dial, TickCount());
    update_status();
}

void session_disconnect(void)
{
    proto_disconnect(&gProto, TickCount());
    update_status();
}

/* Text the app could not keep for lack of memory gets a visible marker. */
static void report_losses(void)
{
    Boolean lost = gBatch.oom != 0;
    if (gProto.lb.damaged != gDamagedSeen) {
        gDamagedSeen = gProto.lb.damaged;
        lost = true;
        if (gProto.perm_open)
            permdlg_mark_incomplete();  /* the request being built lost text */
    }
    if (lost) {
        window_append(&gBatch);         /* clears the batch and its flag */
        batch_marker(&gBatch, RS_ERROR, "Some output was lost: the app ran out of memory.");
    }
}

static void report_log_trouble(void)
{
    char s[160];
    long lost;
    OSErr err = window_log_trouble(&lost);
    if (err && !gLogTrouble) {
        gLogTrouble = true;
        snprintf(s, sizeof(s), "The session log could not be written (error %d). Older text stays in the window while it fits.", err);
        batch_marker(&gBatch, RS_ERROR, s);
    }
    if (lost > 0) {
        snprintf(s, sizeof(s), "%ld bytes of older text left the window without reaching the log.", lost);
        batch_marker(&gBatch, RS_ERROR, s);
    }
    if (gBatch.nruns)
        window_append(&gBatch);
}

void session_pump(void)
{
    static char buf[READ_CHUNK];
    long n, total = 0;
    short errs;
    GrafPtr save;

    /* Callers include ModalDialog's filter: leave the current port alone. */
    GetPort(&save);

    if (serial_is_open()) {
        while (total < DRAIN_LIMIT && (n = serial_read(buf, sizeof(buf))) > 0) {
            total += n;
            gLink.bytes += n;
            proto_feed(&gProto, buf, n, TickCount());
        }
        errs = serial_errors();
        if (errs) {
            gLink.errors++;
            proto_line_noise(&gProto);
            if (!gLink.active) {
                gNoiseSeen = true;
                batch_marker(&gBatch, RS_NOISE, "Line noise: output above may be damaged");
            }
        }
    }
    proto_tick(&gProto, TickCount());

    if (gLink.active) {
        if (gLinkSendNext && gProto.state == ST_READY) {
            gLinkSendNext = false;
            if (TickCount() >= gLink.ends)
                session_linktest_stop();
            else
                proto_send_prompt(&gProto, LINK_PROMPT, sizeof(LINK_PROMPT) - 1);
        }
        batch_clear(&gBatch);
    } else {
        report_losses();
        window_append(&gBatch);
        report_log_trouble();
    }
    proto_drawn(&gProto);              /* everything received is drawn now */

    if (gProto.state == ST_IDLE && serial_is_open()) {
        serial_close();
        log_close();
        gLink.active = false;
        gLinkSession = false;
    }
    update_status();
    SetPort(save);
}

static void answer_note(char a)
{
    const char *s = a == 'y' ? "You allowed it." : a == 'a' ? "You allowed it for this session."
                  : "You denied it.";
    batch_event(&gBatch, EV_INFO, s, (long)strlen(s));
}

void session_deferred(void)
{
    short r;
    Str255 msg;

    if (gWantPerm) {
        gWantPerm = false;
        r = permdlg_run();
        if (r == kPermResultStop) {
            answer_note('n');
            proto_stop(&gProto);
        } else if (r != kPermResultClosed) {
            if (proto_answer(&gProto, (char)r))
                answer_note((char)r);
        }
        update_status();
    }
    if (gAlert != kAlertNone) {
        short kind = gAlert;
        gAlert = kAlertNone;
        msg[0] = (unsigned char)strlen(gAlertMsg);
        memcpy(msg + 1, gAlertMsg, msg[0]);
        ParamText(msg, "\p", "\p", "\p");
        if (gProto.state == ST_IDLE && serial_is_open())
            serial_close();
        r = Alert(kRedialAlert, NULL);
        (void)kind;
        if (r == kRedialButton)
            session_connect();
    }
}

Boolean session_send_input(void)
{
    long len;
    Handle h;
    char state;
    Boolean sent;
    if (gProto.state != ST_READY || gLink.active)
        return false;
    h = window_input_text(&len);
    while (len > 0 && ((*h)[len - 1] == '\r' || (*h)[len - 1] == ' '))
        len--;
    if (len == 0)
        return false;
    state = HGetState(h);
    HLock(h);
    sent = proto_send_prompt(&gProto, *h, len) != 0;
    if (sent)
        batch_prompt(&gBatch, *h, len);
    HSetState(h, state);
    if (sent) {
        window_clear_input();
        gLastTool[0] = 0;
        window_append(&gBatch);
    }
    update_status();
    return sent;
}

void session_stop(void)
{
    proto_stop(&gProto);
}

SessState session_state(void)
{
    return gProto.state;
}

Boolean session_online(void)
{
    return gProto.state != ST_IDLE;
}

void session_linktest_start(long minutes)
{
    memset(&gLink, 0, sizeof(gLink));
    if (gProto.state != ST_IDLE || !open_port())
        return;
    gLink.active = true;
    gLinkSession = true;
    gLink.started = TickCount();
    gLink.ends = gLink.started + (unsigned long)minutes * 60 * TICKS_PER_SEC;
    {
        char dial[64];
        prefs_dial_cstr(dial, sizeof(dial));
        proto_connect(&gProto, dial, TickCount());
    }
}

void session_linktest_stop(void)
{
    gLink.active = false;
    if (gProto.state != ST_IDLE)
        proto_disconnect(&gProto, TickCount());
}

LinkStats *session_linkstats(void)
{
    return &gLink;
}

void session_note(const char *text)
{
    batch_event(&gBatch, EV_INFO, text, (long)strlen(text));
    window_append(&gBatch);
}
