#include "modem.h"
#include "batch.h"
#include "chatlabel.h"
#include "files.h"
#include "filewin.h"
#include "log.h"
#include "modeldlg.h"
#include "opendlg.h"
#include "permdlg.h"
#include "picker.h"
#include "prefs.h"
#include "usage.h"
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
static Boolean gUsageChanged;    /* ... and the status line's context and cost */
static Boolean gTokensChanged;   /* a resume token changed: save the prefs */

enum { kAlertNone, kAlertFailed, kAlertNote };

/* After a lost link the app redials by itself, after these many seconds,
   before it falls back to the Redial alert. */
#define REDIAL_TRIES 3
static const short kRedialDelay[REDIAL_TRIES] = { 5, 20, 60 };

static struct {
    Boolean active;              /* a lost link is being redialed */
    short attempt;               /* 0-based; the one waiting or dialing */
    unsigned long due;           /* tick at which it dials */
    Boolean dialing;             /* the attempt has started */
} gRedial;

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
    const char *names[MAX_TABS], *tokens[MAX_TABS], *models[MAX_TABS], *efforts[MAX_TABS];
    char streams[MAX_TABS];
    short i, n = 0;
    for (i = 0; i < tabs_count(); i++) {
        Tab *t = tabs_at(i);
        if (t->chat)
            continue;            /* chats are never remembered */
        names[n] = t->project;
        tokens[n] = t->token;
        models[n] = t->model;
        efforts[n] = t->effort;
        streams[n] = t->stream;
        n++;
    }
    prefs_set_tabs(names, tokens, models, efforts, streams, n);
    gTokensChanged = false;
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
    if (t->chat ? !gProto.chat_cap : !in_project_list(t->project)) {
        t->unavailable = true;
        snprintf(s, sizeof(s), t->chat ? "The host does not offer chats." : "%s is not available on the host.", t->project);
        error_line(t, s);
        return;
    }
    /* The tab's model and effort go first, so Claude starts once with them. */
    if (gProto.models_cap && (t->model[0] || t->effort[0]))
        proto_set_model(&gProto, t->sid, t->model, t->effort);
    /* Stream Answers is a preset too, sent only to a bridge that has it. */
    if (t->stream && gProto.stream_cap)
        proto_set_stream(&gProto, t->sid, 1);
    /* A tab with a resume token asks the bridge to continue that conversation. */
    t->token_sent = gProto.resume_cap && t->token[0];
    if (!proto_open_resume(&gProto, t->sid, t->chat ? CHAT_WIRE_NAME : t->project, t->token)) {
        t->unavailable = true;
        t->token_sent = false;
        error_line(t, "The tab could not be opened on the bridge.");
    }
}

/* ---- redialing a lost link -------------------------------------------------- */

static void redial_schedule(short attempt)
{
    gRedial.active = true;
    gRedial.dialing = false;
    gRedial.attempt = attempt;
    gRedial.due = TickCount() + (unsigned long)kRedialDelay[attempt] * TICKS_PER_SEC;
}

/* An attempt failed: try again later, or give the person the Redial alert. */
static void redial_failed(const char *why, long n)
{
    char msg[160];
    if (gRedial.attempt + 1 < REDIAL_TRIES) {
        redial_schedule((short)(gRedial.attempt + 1));
        return;
    }
    gRedial.active = false;
    snprintf(msg, sizeof(msg), "The connection to the bridge was lost and could not be "
             "restored. %.*s", (int)(n > 100 ? 100 : n), why);
    queue_alert(kAlertFailed, msg);
}

static Boolean open_port(void);

static void redial_now(void)
{
    char dial[64];
    gRedial.dialing = true;
    if (!open_port()) {
        char why[160];
        copy_text(why, sizeof(why), gAlertMsg, (long)strlen(gAlertMsg));
        gAlert = kAlertNone;
        redial_failed(why, (long)strlen(why));
        return;
    }
    prefs_dial_cstr(dial, sizeof(dial));
    proto_connect(&gProto, dial, TickCount());
}

Boolean session_redial_pending(void)
{
    return gRedial.active;
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
        gRedial.active = false;
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
        redial_schedule(0);      /* the tabs reopen and resume when it connects */
        break;
    case EV_CLOSED:
        for (i = 0; i < tabs_count(); i++)
            tabs_at(i)->running = false;
        filewin_online(0, false);
        gTabsChanged = true;
        break;
    case EV_DIAL_FAILED:
        if (gRedial.active) {
            redial_failed(t, n);
            break;
        }
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

/* A subagent started, moved on or ended: its line in the transcript. */
static void agent_event(Tab *t, EventKind k, const char *s, long n)
{
    char out[AL_TEXT];
    int i;
    if (k == EV_AGENT_START) {
        i = al_start(&t->agents, s, n, out, sizeof(out));
        if (i >= 0)
            window_agent_new(t, i, out);
        else if (i == -2)
            batch_event(&t->batch, EV_TOOL, out, (long)strlen(out));
    } else if (k == EV_AGENT_STEP) {
        i = al_step(&t->agents, s, n, out, sizeof(out));
        if (i >= 0)
            window_agent_update(t, i, out);
    } else if (n >= 1 && s[0] == '*') {          /* the turn ended: close what is open */
        while ((i = al_stop_next(&t->agents, out, sizeof(out))) >= 0)
            window_agent_update(t, i, out);
    } else {
        i = al_end(&t->agents, s, n, out, sizeof(out));
        if (i >= 0)
            window_agent_update(t, i, out);
        else if (i == -2)
            batch_event(&t->batch, EV_TOOL, out, (long)strlen(out));
    }
}

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
        if (!t->chat)
            log_new_session(&t->log, t->project, (short)(tabs_index(t) + 1));
        /* A tab that asked to resume waits for the Y line to say what it got. */
        if (!t->token_sent && (t->opened_once || !window_transcript_empty(t) || t->batch.nruns))
            divider(t);
        t->opened_once = true;
        t->reopened = true;
        t->from_picker = false;
        t->last_tool[0] = 0;
        t->log_trouble = false;
        break;
    case EV_RESUMED: {
        /* "<new|resumed|cut> <token>": which conversation the session runs. */
        const char *sp = memchr(s, ' ', n);
        char how[16], token[17];
        long tn;
        if (!sp)
            break;
        tn = n - (sp + 1 - s);
        copy_text(how, sizeof(how), s, sp - s);
        copy_text(token, sizeof(token), sp + 1, tn > 16 ? 16 : tn);
        if (strcmp(how, "new") == 0 && usage_known(&t->usage)) {
            usage_clear(&t->usage);      /* a new conversation starts with no reading */
            gTabsChanged = gUsageChanged = true;
        }
        if (strcmp(t->token, token) != 0) {
            memcpy(t->token, token, sizeof(token));
            gTokensChanged = true;
        }
        if (t->token_sent) {         /* a reopen: say how the conversation went */
            t->token_sent = false;
            if (strcmp(how, "resumed") == 0)
                batch_marker(&t->batch, RS_DIVIDER, "Reconnected. The conversation continues.");
            else if (strcmp(how, "cut") == 0)
                batch_marker(&t->batch, RS_DIVIDER,
                             "Reconnected. The last reply was cut off when the link dropped.");
            else {
                if (!window_transcript_empty(t) || t->batch.nruns)
                    divider(t);
                note(t, "That conversation could not be resumed; this is a new one.");
            }
        }
        break;
    }
    case EV_USAGE: {
        Usage u;
        if (usage_parse(&u, s, n)) {
            t->usage = u;
            gTabsChanged = gUsageChanged = true;
        }
        break;
    }
    case EV_MODEL: {
        /* "<model|-> <effort|->": what the tab runs with, or will from its
           next turn. A value the bridge corrected (a model it no longer
           offers) arrives as the default, and the bridge says so itself. */
        const char *sp = memchr(s, ' ', n);
        char model[sizeof(t->model)], effort[sizeof(t->effort)];
        long mn = sp ? sp - s : n, en = sp ? n - (sp + 1 - s) : 0;
        copy_text(model, sizeof(model), s, mn > 31 ? 31 : mn);
        copy_text(effort, sizeof(effort), sp ? sp + 1 : "", en > 7 ? 7 : en);
        if (strcmp(model, "-") == 0)
            model[0] = 0;
        if (strcmp(effort, "-") == 0)
            effort[0] = 0;
        if (strcmp(t->model, model) != 0 || strcmp(t->effort, effort) != 0) {
            memcpy(t->model, model, sizeof(model));
            memcpy(t->effort, effort, sizeof(effort));
            gTokensChanged = true;       /* saved with the tabs */
        }
        break;
    }
    case EV_READY:
        if (t->reopened) {           /* the first > after opening */
            t->reopened = false;
            filewin_online(t->sid, true);
        } else {
            filewin_turn_ended(t->sid);
        }
        break;
    case EV_OPEN_FAILED:
        t->token_sent = false;
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
    case EV_AGENT_START:
    case EV_AGENT_STEP:
    case EV_AGENT_END:
        agent_event(t, k, s, n);
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
        if (gRedial.active) {
            long left = ((long)(gRedial.due - TickCount()) + TICKS_PER_SEC - 1) / TICKS_PER_SEC;
            if (left < 0)
                left = 0;
            snprintf(s, sizeof(s), "Link lost. Redialing in %ld:%02ld (try %d of %d). Cmd-D cancels.",
                     left / 60, left % 60, gRedial.attempt + 1, REDIAL_TRIES);
        } else {
            strcpy(s, "Not connected");
        }
        break;
    case ST_DIALING:
        if (gRedial.active)
            snprintf(s, sizeof(s), "Redialing\xC9 (try %d of %d)", gRedial.attempt + 1, REDIAL_TRIES);
        else
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
        else if (front) {
            front_status(front, s, sizeof(s));
            if (front->model[0] || front->effort[0]) {
                size_t k = strlen(s);
                snprintf(s + k, sizeof(s) - k, " [%s%s%s]", front->model,
                         front->model[0] && front->effort[0] ? ", " : "", front->effort);
            }
        } else
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

Boolean session_model_available(void)
{
    Tab *t = tabs_front();
    return t && gProto.state == ST_ONLINE && gProto.models_cap && gProto.nmodels > 0
        && proto_sess_state(&gProto, t->sid) != SS_NONE
        && proto_sess_state(&gProto, t->sid) != SS_CLOSING;
}

const char *session_model_reason(void)
{
    Tab *t = tabs_front();
    if (session_model_available())
        return "";
    if (gProto.state != ST_ONLINE)
        return "Model and Effort needs a connected tab.";
    if (!t)
        return "Model and Effort needs an open tab.";
    if (!gProto.models_cap || gProto.nmodels == 0)
        return "The bridge offers no models.";
    return "Model and Effort is not available for this tab yet.";
}

void session_model_dialog(void)
{
    Tab *t = tabs_front();
    char model[sizeof(t->model)], effort[sizeof(t->effort)];
    if (!session_model_available())
        return;
    if (modeldlg_run(t->project, t->model, t->effort, model, effort) && session_model_available())
        proto_set_model(&gProto, t->sid, model, effort);   /* the bridge confirms with EV_MODEL */
}

/* Stream Answers is for the front tab. Offline it can be set (the tab
   remembers it and sends it when it opens); online it needs a bridge with
   the stream capability, and a tab that is not closing. */
Boolean session_stream_available(void)
{
    Tab *t = tabs_front();
    if (!t)
        return false;
    if (gProto.state != ST_ONLINE)
        return gProto.state == ST_IDLE;
    return gProto.stream_cap && proto_sess_state(&gProto, t->sid) != SS_CLOSING;
}

Boolean session_usage_shown(void)
{
    return gProto.usage_cap != 0;
}

Boolean session_stream_on(void)
{
    Tab *t = tabs_front();
    return t && t->stream && (gProto.state != ST_ONLINE || gProto.stream_cap);
}

void session_stream_toggle(void)
{
    Tab *t = tabs_front();
    if (!t || !session_stream_available())
        return;
    t->stream = !t->stream;
    /* An open session changes at once; one not yet open gets it from open_tab. */
    if (gProto.state == ST_ONLINE && proto_sess_state(&gProto, t->sid) != SS_NONE)
        proto_set_stream(&gProto, t->sid, t->stream);
    remember_tabs();
}

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
        if (!name[0] || strcmp(name, CHAT_WIRE_NAME) == 0 || tabs_by_project(name))
            continue;
        t = tabs_add(name, (short)(i + 1));
        if (!t)
            continue;
        copy_text(t->token, sizeof(t->token), gPrefs.tokens[i], (long)strlen(gPrefs.tokens[i]));
        copy_text(t->model, sizeof(t->model), gPrefs.models[i], (long)strlen(gPrefs.models[i]));
        copy_text(t->effort, sizeof(t->effort), gPrefs.efforts[i], (long)strlen(gPrefs.efforts[i]));
        t->stream = gPrefs.streams[i] != 0;
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
    gRedial.active = false;      /* a manual Connect takes over from the countdown */
    if (!open_port())
        return;
    prefs_dial_cstr(dial, sizeof(dial));
    proto_connect(&gProto, dial, TickCount());
    update_status();
}

void session_disconnect(void)
{
    gRedial.active = false;      /* also cancels a countdown or an attempt */
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
    if (gRedial.active && !gRedial.dialing && gProto.state == ST_IDLE && !gLink.active
            && TickCount() >= gRedial.due)
        redial_now();

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
    if (gTokensChanged)
        remember_tabs();               /* outside the core's callback: it writes the prefs */

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
    if (gUsageChanged) {
        gUsageChanged = false;
        window_status_redraw();      /* the message may be the same; the reading is not */
    }
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

/* Makes a tab with a label (project name or chat label), opens it. */
static void start_tab(const char *label, Boolean chat)
{
    short sid = free_sid();
    Tab *t = sid ? tabs_add(label, sid) : NULL;
    if (!t) {
        SysBeep(1);
        return;
    }
    t->chat = chat;
    t->log.discard = chat;
    if (!window_tab_init(t)) {
        tabs_remove(t);
        ParamText("\pThere is not enough memory for another tab.", "\p", "\p", "\p");
        Alert(kNoteAlert, NULL);
        return;
    }
    t->from_picker = true;
    if (!chat)
        log_new_session(&t->log, t->project, (short)(tabs_index(t) + 1));
    open_tab(t);
    session_select_tab(t);
    remember_tabs();
    window_draw_tabs();
}

Boolean session_chat_available(void)
{
    return gProto.state == ST_ONLINE && gProto.chat_cap && tabs_count() < MAX_TABS;
}

void session_new_chat(void)
{
    const char *used[MAX_TABS];
    short i, n = 0;
    char label[TAB_NAME_MAX + 1];
    if (!session_chat_available())
        return;
    for (i = 0; i < tabs_count(); i++)
        if (tabs_at(i)->chat)
            used[n++] = tabs_at(i)->project;
    chat_label(label, sizeof(label), used, n);
    start_tab(label, true);
}

void session_new_tab(void)
{
    char name[64];
    PickResult r;
    Tab *t;
    if (gProto.state != ST_ONLINE || tabs_count() >= MAX_TABS)
        return;
    r = picker_run(name, sizeof(name), gProto.chat_cap != 0);
    if (r == PICK_NONE || gProto.state != ST_ONLINE || tabs_count() >= MAX_TABS)
        return;                  /* cancelled, or the link went away while choosing */
    if (r == PICK_CHAT) {
        session_new_chat();
        return;
    }
    t = tabs_by_project(name);
    if (t) {
        session_select_tab(t);
        return;
    }
    start_tab(name, false);
}

void session_close_chats(void)
{
    short i;
    for (i = 0; i < tabs_count(); i++)
        if (tabs_at(i)->chat && proto_sess_state(&gProto, tabs_at(i)->sid) != SS_NONE)
            proto_close(&gProto, tabs_at(i)->sid);
}

Boolean session_chats_closing(void)
{
    short i;
    for (i = 0; i < tabs_count(); i++)
        if (tabs_at(i)->chat && proto_sess_state(&gProto, tabs_at(i)->sid) != SS_NONE)
            return true;
    return false;
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
