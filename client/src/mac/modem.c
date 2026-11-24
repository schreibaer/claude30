#include "modem.h"
#include "batch.h"
#include "files.h"
#include "filewin.h"
#include "log.h"
#include "opendlg.h"
#include "permdlg.h"
#include "picker.h"
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
static LinkStats gLink;

static short gAlert;             /* what session_deferred must show */
static char gAlertMsg[160];
static Boolean gWantPicker;      /* connected with no tab: offer the picker */
static Boolean gNoiseSeen;       /* serial errors this call */
static long gDamagedSeen;        /* gProto.lb.damaged already reported */
static Boolean gLinkSendNext;
static Boolean gLinkSession;    /* this call is a link test, until it ends */
static Boolean gTabsChanged;     /* redraw the strip at the end of the pass */

enum { kAlertNone, kAlertLost, kAlertFailed, kAlertNote };

/* ---- helpers ------------------------------------------------------------- */

static void on_send(void *ctx, const char *data, long len)
{
    (void)ctx;
    serial_write(data, len);
}

static void copy_text(char *dst, long cap, const char *t, long n)
{
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, t, n);
    dst[n] = 0;
}

static void note(Tab *t, const char *s)
{
    batch_event(&t->batch, EV_INFO, s, (long)strlen(s));
}

static void error_line(Tab *t, const char *s)
{
    batch_marker(&t->batch, RS_ERROR, s);
}

/* GetDateTime reads the Time low-memory global, which GCC mistakes for an
   empty array. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static void divider(Tab *t)
{
    unsigned long secs;
    Str255 date, time;
    char line[96];
    GetDateTime(&secs);
    IUDateString(secs, shortDate, date);
    IUTimeString(secs, false, time);
    snprintf(line, sizeof(line), "New conversation, %.*s %.*s",
             date[0], (char *)date + 1, time[0], (char *)time + 1);
    batch_marker(&t->batch, RS_DIVIDER, line);
}
#pragma GCC diagnostic pop

static void remember_tabs(void)
{
    const char *names[MAX_TABS];
    short i, n = tabs_count();
    for (i = 0; i < n; i++)
        names[i] = tabs_at(i)->project;
    prefs_set_tabs(names, n);
}

static void queue_alert(short kind, const char *msg)
{
    if (gAlert != kAlertNone && kind == kAlertNote)
        return;                  /* a link alert outranks a note */
    gAlert = kind;
    copy_text(gAlertMsg, sizeof(gAlertMsg), msg, (long)strlen(msg));
}

static Boolean in_project_list(const char *name)
{
    short i, n = (short)proto_nprojects(&gProto);
    for (i = 0; i < n; i++)
        if (strcmp(proto_project(&gProto, i), name) == 0)
            return true;
    return false;
}

/* Opens a tab's project on the bridge, or marks it unavailable. */
static void open_tab(Tab *t)
{
    char s[128];
    t->unavailable = false;
    if (!in_project_list(t->project)) {
        t->unavailable = true;
        snprintf(s, sizeof(s), "%s is not available on the host.", t->project);
        error_line(t, s);
        return;
    }
    if (!proto_open(&gProto, t->sid, t->project)) {
        t->unavailable = true;
        error_line(t, "The tab could not be opened on the bridge.");
    }
}

/* ---- link test events ----------------------------------------------------- */

static void linktest_event(int sid, EventKind k, const char *t, long n)
{
    switch (k) {
    case EV_STATE:
    case EV_PROJECTS:
        break;
    case EV_CONNECTED:
        if (proto_nprojects(&gProto) > 0)
            proto_open(&gProto, 1, proto_project(&gProto, 0));
        else
            session_linktest_stop();
        break;
    case EV_PERM_ASK:
        proto_answer(&gProto, sid, 'n');   /* the demo asks once per turn */
        gLink.lines++;
        break;
    case EV_READY:
        if (sid == 1) {
            gLink.turns++;
            gLinkSendNext = true;
        }
        break;
    case EV_OPEN_FAILED:
    case EV_DIAL_FAILED:
    case EV_LOST:
        gLink.failed = true;
        if (k == EV_LOST) {
            t = "The connection was lost.";
            n = (long)strlen(t);
        }
        copy_text(gLink.failure, sizeof(gLink.failure), t, n);
        gLink.active = false;
        if (k == EV_OPEN_FAILED)
            proto_disconnect(&gProto, TickCount());
        break;
    case EV_CLOSED:
        gLink.active = false;
        break;
    default:
        gLink.lines++;
        break;
    }
}

/* ---- events for the link (session id 0) -------------------------------------- */

static void link_event(EventKind k, const char *t, long n)
{
    short i;
    Tab *tab;
    switch (k) {
    case EV_CONNECTED:
        gNoiseSeen = false;
        if (tabs_count() == 0)
            gWantPicker = true;
        for (i = 0; i < tabs_count(); i++)
            open_tab(tabs_at(i));
        break;
    case EV_PROJECTS:
        picker_projects_changed();
        break;
    case EV_LOST:
        for (i = 0; i < tabs_count(); i++) {
            tab = tabs_at(i);
            error_line(tab, "The connection was lost.");
            tab->running = false;
        }
        filewin_online(0, false);
        gTabsChanged = true;
        queue_alert(kAlertLost, "The connection to the bridge was lost.");
        break;
    case EV_CLOSED:
        for (i = 0; i < tabs_count(); i++)
            tabs_at(i)->running = false;
        filewin_online(0, false);
        gTabsChanged = true;
        break;
    case EV_DIAL_FAILED:
        gAlert = kAlertFailed;
        copy_text(gAlertMsg, sizeof(gAlertMsg), t, n);
        break;
    case EV_INFO:
        tab = tabs_front();
        if (tab)
            batch_event(&tab->batch, k, t, n);
        break;
    default:
        break;                   /* EV_STATE, EV_SILENCE: the status line shows them */
    }
}

/* ---- events for one tab ----------------------------------------------------- */

static void tab_event(Tab *t, EventKind k, const char *s, long n)
{
    char msg[160];
    SessState st;
    switch (k) {
    case EV_STATE:
        st = proto_sess_state(&gProto, t->sid);
        if (t->running != (st == SS_BUSY || st == SS_PERMISSION)) {
            t->running = !t->running;
            gTabsChanged = true;
        }
        break;
    case EV_OPENED:
        log_new_session(&t->log, t->project, (short)(tabs_index(t) + 1));
        if (t->opened_once || !window_transcript_empty(t) || t->batch.nruns)
            divider(t);
        t->opened_once = true;
        t->reopened = true;
        t->from_picker = false;
        t->last_tool[0] = 0;
        t->log_trouble = false;
        break;
    case EV_READY:
        if (t->reopened) {           /* the first > after opening */
            t->reopened = false;
            filewin_online(t->sid, true);
        } else {
            filewin_turn_ended(t->sid);
        }
        break;
    case EV_OPEN_FAILED:
        if (t->from_picker) {
            snprintf(msg, sizeof(msg), "%s could not be opened: %.*s", t->project, (int)n, s);
            queue_alert(kAlertNote, msg);
            t->unavailable = true;
            t->refused = true;
            t->from_picker = false;
            /* session_deferred removes it, outside the core's callback */
        } else {
            t->unavailable = true;
            snprintf(msg, sizeof(msg), "%s is not available on the host (%.*s).",
                     t->project, (int)n, s);
            error_line(t, msg);
        }
        break;
    case EV_SESSION_CLOSED:
        t->unavailable = true;
        t->running = false;
        gTabsChanged = true;
        filewin_online(t->sid, false);
        snprintf(msg, sizeof(msg), "The bridge ended this conversation: %.*s", (int)n, s);
        error_line(t, msg);
        break;
    case EV_TOOL:
        copy_text(t->last_tool, sizeof(t->last_tool), s, n);
        batch_event(&t->batch, k, s, n);
        break;
    case EV_PERM_BEGIN:
        permdlg_begin(&t->perm, s, n);
        batch_event(&t->batch, k, s, n);
        break;
    case EV_PERM_LINE:
        permdlg_line(&t->perm, s, n);
        break;
    case EV_PERM_ASK:
        t->want_perm = true;
        gTabsChanged = true;
        break;
    case EV_PERM_CLOSE:
        t->want_perm = false;
        gTabsChanged = true;
        permdlg_request_close(&t->perm);
        break;
    default:
        batch_event(&t->batch, k, s, n);
        break;
    }
}

static void on_event(void *ctx, int sid, EventKind k, const char *t, long n)
{
    Tab *tab;
    (void)ctx;
    if (gLinkSession) {
        linktest_event(sid, k, t, n);
        return;
    }
    if (k >= EV_LIST_BEGIN && k <= EV_LIST_FAILED) {
        opendlg_event((short)sid, k, t, n);
        return;
    }
    if (k >= EV_PIECE) {
        filewin_event((short)sid, k, t, n);
        return;
    }
    if (sid == 0) {
        link_event(k, t, n);
        return;
    }
    tab = tabs_by_sid((short)sid);
    if (tab)
        tab_event(tab, k, t, n);
}

/* ---- status line --------------------------------------------------------- */

static void front_status(Tab *t, char *s, short max)
{
    long quiet;
    switch (proto_sess_state(&gProto, t->sid)) {
    case SS_NONE:
        if (t->unavailable)
            snprintf(s, max, "%s is not available. Cmd-W closes the tab.", t->project);
        else
            snprintf(s, max, "%s is not open.", t->project);
        break;
    case SS_OPENING:
        snprintf(s, max, "Opening %s\xC9", t->project);
        break;
    case SS_READY:
        snprintf(s, max, "%s: connected at %s. Type a prompt and press Return.",
                 t->project, serial_speed_name(gPrefs.speed));
        break;
    case SS_BUSY:
        quiet = proto_silence_secs(&gProto);
        if (quiet >= T_SILENCE / TICKS_PER_SEC)
            snprintf(s, max, "No output for %ld:%02ld. Cmd-period stops; Disconnect hangs up.",
                     quiet / 60, quiet % 60);
        else if (t->last_tool[0])
            snprintf(s, max, "%s: working: %s", t->project, t->last_tool);
        else
            snprintf(s, max, "%s: Claude is working. Cmd-period stops.", t->project);
        break;
    case SS_PERMISSION:
        snprintf(s, max, "%s: waiting for your answer to a permission request", t->project);
        break;
    case SS_CLOSING:
        snprintf(s, max, "Closing %s\xC9", t->project);
        break;
    }
}

static void update_status(void)
{
    char s[160];
    Tab *front = tabs_front(), *wait = NULL, *t;
    short i, waiting = 0;
    Boolean ready = false;

    for (i = 0; i < tabs_count(); i++) {
        t = tabs_at(i);
        if (t != front && t->want_perm) {
            if (!wait)
                wait = t;
            waiting++;
        }
    }
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
    case ST_CLOSING:
        strcpy(s, "Disconnecting\xC9");
        break;
    case ST_HANGUP:
        strcpy(s, "Hanging up\xC9");
        break;
    case ST_ONLINE:
        if (waiting == 1)
            snprintf(s, sizeof(s), "%s is waiting for permission", wait->project);
        else if (waiting > 1)
            snprintf(s, sizeof(s), "%s and %d more tabs are waiting for permission",
                     wait->project, waiting - 1);
        else if (front)
            front_status(front, s, sizeof(s));
        else
            strcpy(s, "No project open. Press Cmd-T.");
        ready = front && !front->unavailable
             && proto_sess_state(&gProto, front->sid) == SS_READY;
        break;
    }
    if (gNoiseSeen && proto_online(&gProto))
        strncat(s, " (line noise seen)", sizeof(s) - strlen(s) - 1);
    if (front && front->log_trouble && proto_online(&gProto))
        strncat(s, " (log not saving)", sizeof(s) - strlen(s) - 1);
    window_set_status(s);
    window_set_send_enabled(ready && !gLink.active);
}

/* ---- public -------------------------------------------------------------- */

void session_init(void)
{
    short i;
    char name[64];
    Tab *t;
    proto_init(&gProto, on_event, on_send, NULL);
    /* The remembered tabs come back with empty transcripts; Connect opens
       them. */
    for (i = 0; i < gPrefs.ntabs; i++) {
        copy_text(name, sizeof(name), (const char *)gPrefs.tabs[i] + 1, gPrefs.tabs[i][0]);
        if (!name[0] || tabs_by_project(name))
            continue;
        t = tabs_add(name, (short)(i + 1));
        if (!t)
            continue;
        if (!window_tab_init(t)) {
            tabs_remove(t);
            continue;
        }
        log_new_session(&t->log, t->project, (short)(tabs_index(t) + 1));
        tabs_set_front(t);
    }
    if (tabs_count() > 0)
        tabs_set_front(tabs_at(0));
    window_show_tab(tabs_front());
    window_draw_tabs();
}

Proto *session_proto(void)
{
    return &gProto;
}

LinkState session_link_state(void)
{
    return gProto.state;
}

Boolean session_online(void)
{
    return gProto.state != ST_IDLE;
}

Boolean session_tab_ready(const Tab *t)
{
    return t && !t->unavailable && gProto.state == ST_ONLINE
        && proto_sess_state(&gProto, t->sid) != SS_NONE
        && proto_sess_state(&gProto, t->sid) != SS_OPENING
        && proto_sess_state(&gProto, t->sid) != SS_CLOSING;
}

Boolean session_tab_busy(const Tab *t)
{
    SessState s = proto_sess_state(&gProto, t->sid);
    return s == SS_BUSY || s == SS_PERMISSION;
}

Boolean session_tab_waiting(const Tab *t)
{
    return t->want_perm || proto_sess_state(&gProto, t->sid) == SS_PERMISSION;
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
static void report_losses(Tab *t)
{
    Boolean lost = t->batch.oom != 0;
    if (lost) {
        window_append(t);              /* clears the batch and its flag */
        batch_marker(&t->batch, RS_ERROR, "Some output was lost: the app ran out of memory.");
    }
}

static void report_damage(void)
{
    short i;
    Tab *t;
    if (gProto.lb.damaged == gDamagedSeen)
        return;
    gDamagedSeen = gProto.lb.damaged;
    for (i = 0; i < tabs_count(); i++) {
        t = tabs_at(i);
        if (gProto.sess[t->sid].perm_open)
            permdlg_mark_incomplete(&t->perm);   /* the request being built lost text */
        if (session_tab_busy(t))
            error_line(t, "Some output was lost: the app ran out of memory.");
    }
}

static void report_log_trouble(Tab *t)
{
    char s[160];
    if (t->log_err && !t->log_trouble) {
        t->log_trouble = true;
        snprintf(s, sizeof(s), "The session log could not be written (error %d). Older text stays in the window while it fits.", t->log_err);
        batch_marker(&t->batch, RS_ERROR, s);
    }
    t->log_err = noErr;
    if (t->lost_bytes > 0) {
        snprintf(s, sizeof(s), "%ld bytes of older text left the window without reaching the log.",
                 t->lost_bytes);
        t->lost_bytes = 0;
        batch_marker(&t->batch, RS_ERROR, s);
    }
    if (t->batch.nruns)
        window_append(t);
}

void session_pump(void)
{
    static char buf[READ_CHUNK];
    long n, total = 0;
    short errs, i;
    GrafPtr save;
    Tab *t;

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
                for (i = 0; i < tabs_count(); i++) {
                    t = tabs_at(i);
                    if (session_tab_busy(t))
                        batch_marker(&t->batch, RS_NOISE, "Line noise: output above may be damaged");
                }
            }
        }
    }
    proto_tick(&gProto, TickCount());
    proto_files_step(&gProto);

    if (gLink.active) {
        if (gLinkSendNext && proto_sess_state(&gProto, 1) == SS_READY) {
            gLinkSendNext = false;
            if (TickCount() >= gLink.ends)
                session_linktest_stop();
            else
                proto_send_prompt(&gProto, 1, LINK_PROMPT, sizeof(LINK_PROMPT) - 1);
        }
    } else {
        report_damage();
        for (i = 0; i < tabs_count(); i++) {
            t = tabs_at(i);
            report_losses(t);
            window_append(t);
            report_log_trouble(t);
        }
    }
    proto_drawn(&gProto);              /* everything received is drawn now */

    if (gProto.state == ST_IDLE && serial_is_open()) {
        serial_close();
        for (i = 0; i < tabs_count(); i++)
            log_close(&tabs_at(i)->log);
        gLink.active = false;
        gLinkSession = false;
    }
    if (gTabsChanged) {
        gTabsChanged = false;
        window_draw_tabs();
    }
    update_status();
    SetPort(save);
}

static void answer_note(Tab *t, char a)
{
    const char *s = a == 'y' ? "You allowed it." : a == 'a' ? "You allowed it for this session."
                  : "You denied it.";
    note(t, s);
}

/* Removes a tab that never opened (a refused New Tab). */
static void drop_refused_tabs(void)
{
    short i;
    Tab *t;
    for (i = tabs_count() - 1; i >= 0; i--) {
        t = tabs_at(i);
        if (t->refused) {
            window_tab_free(t);
            tabs_remove(t);
            window_show_tab(tabs_front());
            filewin_show_tab(tabs_front() ? tabs_front()->sid : 0);
            remember_tabs();
            gTabsChanged = true;
        }
    }
}

void session_deferred(void)
{
    short r;
    Str255 msg;
    Tab *front;

    drop_refused_tabs();
    front = tabs_front();
    if (front && front->want_perm) {
        front->want_perm = false;
        window_draw_tabs();
        r = permdlg_run(&front->perm);
        if (r == kPermResultStop) {
            answer_note(front, 'n');
            proto_stop(&gProto, front->sid);
        } else if (r != kPermResultClosed) {
            if (proto_answer(&gProto, front->sid, (char)r))
                answer_note(front, (char)r);
        }
        update_status();
    }
    if (gAlert != kAlertNone) {
        short kind = gAlert;
        gAlert = kAlertNone;
        msg[0] = (unsigned char)strlen(gAlertMsg);
        memcpy(msg + 1, gAlertMsg, msg[0]);
        ParamText(msg, "\p", "\p", "\p");
        if (kind == kAlertNote) {
            Alert(kNoteAlert, NULL);
        } else {
            if (gProto.state == ST_IDLE && serial_is_open())
                serial_close();
            r = Alert(kRedialAlert, NULL);
            if (r == kRedialButton)
                session_connect();
        }
    }
    if (gWantPicker) {
        gWantPicker = false;
        if (gProto.state == ST_ONLINE && tabs_count() == 0)
            session_new_tab();
    }
    filewin_deferred();
}

Boolean session_send_input(void)
{
    long len;
    Handle h;
    char state;
    Boolean sent;
    Tab *t = tabs_front();
    if (!t || t->unavailable || gLink.active
            || proto_sess_state(&gProto, t->sid) != SS_READY)
        return false;
    h = window_input_text(&len);
    while (len > 0 && ((*h)[len - 1] == '\r' || (*h)[len - 1] == ' '))
        len--;
    if (len == 0)
        return false;
    state = HGetState(h);
    HLock(h);
    if (proto_is_quit(*h, len)) {
        HSetState(h, state);
        window_clear_input();
        session_close_tab(t);        /* /quit closes this tab only */
        return false;
    }
    sent = proto_send_prompt(&gProto, t->sid, *h, len) != 0;
    if (sent)
        batch_prompt(&t->batch, *h, len);
    HSetState(h, state);
    if (sent) {
        window_clear_input();
        t->last_tool[0] = 0;
        window_append(t);
    }
    update_status();
    return sent;
}

void session_stop(void)
{
    Tab *t = tabs_front();
    if (t)
        proto_stop(&gProto, t->sid);
}

void session_note(const char *text)
{
    Tab *t = tabs_front();
    if (!t)
        return;
    note(t, text);
    window_append(t);
}

/* ---- tabs ---------------------------------------------------------------- */

void session_select_tab(Tab *t)
{
    if (!t)
        return;
    if (t != tabs_front()) {
        tabs_set_front(t);
        window_show_tab(t);
        filewin_show_tab(t->sid);
    }
    if (FrontWindow() != window_ptr())
        SelectWindow(window_ptr());
    update_status();
    /* A waiting permission request opens from session_deferred. */
}

/* A session id no tab uses and the core has fully closed. */
static short free_sid(void)
{
    short sid;
    for (sid = 1; sid <= MAX_TABS; sid++)
        if (!tabs_by_sid(sid) && proto_sess_state(&gProto, sid) == SS_NONE)
            return sid;
    return 0;
}

void session_new_tab(void)
{
    char name[64];
    short sid;
    Tab *t;
    if (gProto.state != ST_ONLINE || tabs_count() >= MAX_TABS)
        return;
    if (!picker_run(name, sizeof(name)))
        return;
    if (gProto.state != ST_ONLINE)
        return;                  /* the link went away while choosing */
    t = tabs_by_project(name);
    if (t) {
        session_select_tab(t);
        return;
    }
    sid = free_sid();
    t = sid ? tabs_add(name, sid) : NULL;
    if (!t) {
        SysBeep(1);
        return;
    }
    if (!window_tab_init(t)) {
        tabs_remove(t);
        ParamText("\pThere is not enough memory for another tab.", "\p", "\p", "\p");
        Alert(kNoteAlert, NULL);
        return;
    }
    t->from_picker = true;
    log_new_session(&t->log, t->project, (short)(tabs_index(t) + 1));
    open_tab(t);
    session_select_tab(t);
    remember_tabs();
    window_draw_tabs();
}

Boolean session_close_tab(Tab *t)
{
    Str255 name;
    short n;
    if (!t)
        return false;
    if (session_tab_busy(t)) {
        n = (short)strlen(t->project);
        name[0] = (unsigned char)n;
        memcpy(name + 1, t->project, n);
        ParamText(name, "\p", "\p", "\p");
        if (CautionAlert(kCloseBusyAlert, NULL) != 1)
            return false;
    }
    if (!filewin_settle(t->sid, true))
        return false;
    if (proto_sess_state(&gProto, t->sid) != SS_NONE)
        proto_close(&gProto, t->sid);
    permdlg_request_close(&t->perm);
    window_append(t);                /* the last lines reach its log */
    window_tab_free(t);
    tabs_remove(t);
    window_show_tab(tabs_front());
    filewin_show_tab(tabs_front() ? tabs_front()->sid : 0);
    remember_tabs();
    window_draw_tabs();
    update_status();
    return true;
}

const char *session_waiting_project(void)
{
    short i;
    for (i = 0; i < tabs_count(); i++)
        if (tabs_at(i)->want_perm)
            return tabs_at(i)->project;
    return NULL;
}

/* ---- link test ------------------------------------------------------------- */

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
