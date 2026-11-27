#include "permdlg.h"
#include "permsum.h"
#include "rules.h"
#include "modem.h"
#include "resources.h"
#include "window.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <string.h>

#define MONACO       4
#define LIST_SIZE    9
#define MARK_WIDTH   2           /* "  " or the continuation mark */
#define ARM_TICKS    45          /* clicks on the buttons are ignored for 0.75 s */
#define GENEVA       3
#define HEAD_SIZE    12
#define TITLE_SIZE   10

typedef struct {
    long off;                    /* offset into the request body */
    short len;
    Boolean cont;                /* continuation of the line above */
    Boolean hdr;                 /* off is into the request's header, not its body */
} Row;

static PermReq *gReq;            /* the request the open dialog shows */
static Handle gRows;
static long gNRows;
static unsigned long gArmedAt;   /* buttons take clicks from this tick on */

static DialogPtr gDlg;
static ControlHandle gBar;
static Rect gList;
static short gRowH, gRowAscent, gVisible;
static Rect gHeadRect;           /* the bold headline above the header */
static Rect gTitleRect;          /* the request's header: one plain line, cut to fit */
static Boolean gTitleCut;        /* it did not fit: the list opens with the whole header */
static char gHeadline[PERMSUM_MAX];
static long gHeadlineLen;

/* ---- collecting the request -------------------------------------------- */

static Boolean grow(Handle *h, long size)
{
    if (!*h) {
        *h = NewHandle(size > 256 ? size : 256);
        return *h != NULL;
    }
    if (GetHandleSize(*h) >= size)
        return true;
    SetHandleSize(*h, size + size / 2);
    return MemError() == noErr;
}

void permdlg_begin(PermReq *q, const char *title, long len)
{
    q->body_len = 0;
    q->nlines = 0;
    q->close_requested = false;
    q->incomplete = false;
    q->can_rule = false;
    q->rule[0] = 0;
    if (len > 255)
        len = 255;
    q->title[0] = (unsigned char)len;
    memcpy(q->title + 1, title, len);
}

void permdlg_line(PermReq *q, const char *text, long len)
{
    if (!grow(&q->body, q->body_len + len)
            || !grow(&q->line_ends, (q->nlines + 1) * sizeof(long))) {
        q->incomplete = true;    /* out of memory: the line is dropped */
        return;
    }
    memcpy(*q->body + q->body_len, text, len);
    q->body_len += len;
    ((long *)*q->line_ends)[q->nlines++] = q->body_len;
}

void permdlg_mark_incomplete(PermReq *q)
{
    q->incomplete = true;
}

void permdlg_request_close(PermReq *q)
{
    q->close_requested = true;
}

void permdlg_free(PermReq *q)
{
    if (q->body)
        DisposeHandle(q->body);
    if (q->line_ends)
        DisposeHandle(q->line_ends);
    q->body = q->line_ends = NULL;
    q->body_len = q->nlines = 0;
}

/* Splits body lines into rows that fit the list width. */
static void build_rows(short cols)
{
    long i, start = 0, end, pos;
    Row *r;
    gNRows = 0;
    if (cols < 8)
        cols = 8;
    /* A header cut to one line must still be readable: the list opens with
       all of it, wrapped. */
    if (gTitleCut) {
        pos = 0;
        end = gReq->title[0];
        do {
            long n = end - pos;
            if (n > cols)
                n = cols;
            if (!grow(&gRows, (gNRows + 1) * sizeof(Row))) {
                gReq->incomplete = true;
                return;
            }
            r = &((Row *)*gRows)[gNRows++];
            r->off = pos;
            r->len = (short)n;
            r->cont = pos != 0;
            r->hdr = true;
            pos += n;
        } while (pos < end);
    }
    for (i = 0; i < gReq->nlines; i++) {
        end = ((long *)*gReq->line_ends)[i];
        pos = start;
        do {
            long n = end - pos;
            if (n > cols)
                n = cols;
            if (!grow(&gRows, (gNRows + 1) * sizeof(Row))) {
                gReq->incomplete = true;
                return;
            }
            r = &((Row *)*gRows)[gNRows++];
            r->off = pos;
            r->len = (short)n;
            r->cont = pos != start;
            r->hdr = false;
            pos += n;
        } while (pos < end);
        start = end;
    }
}

/* ---- drawing ------------------------------------------------------------ */

static pascal void draw_list(DialogPtr d, short item)
{
    long first, i;
    short y;
    Rect inner = gList;
    char saveState = 0;
    (void)d;
    (void)item;

    FrameRect(&gList);
    InsetRect(&inner, 1, 1);
    EraseRect(&inner);
    TextFont(MONACO);
    TextSize(LIST_SIZE);
    TextFace(0);
    first = GetControlValue(gBar);
    y = inner.top + gRowAscent + 1;
    if (gNRows == 0) {
        /* Nothing more to show: say what each button does instead. */
        static const char *const kHelp[] = {
            "Nothing more to show for this request.",
            "",
            "Allow    lets it run, this once.",
            "Deny     refuses it; Claude is told and goes on.",
            "Always   allows the same request for the rest of",
            "         this conversation.",
            NULL
        };
        short k;
        for (k = 0; kHelp[k] && y < inner.bottom; k++) {
            MoveTo(inner.left + 4, y);
            DrawText((Ptr)kHelp[k], 0, (short)strlen(kHelp[k]));
            y += gRowH;
        }
        if (gReq->can_rule && !gReq->incomplete) {
            static const char *const kRuleHelp[] = {
                "Rule...  saves a rule in the project, so Claude",
                "         may do this again without asking.",
                NULL
            };
            for (k = 0; kRuleHelp[k] && y < inner.bottom; k++) {
                MoveTo(inner.left + 4, y);
                DrawText((Ptr)kRuleHelp[k], 0, (short)strlen(kRuleHelp[k]));
                y += gRowH;
            }
        }
    }
    if (gNRows > 0) {
        if (gReq->body) {
            saveState = HGetState(gReq->body);
            HLock(gReq->body);
        }
        for (i = first; i < gNRows && i < first + gVisible; i++) {
            Row *r = &((Row *)*gRows)[i];
            MoveTo(inner.left + 3, y);
            DrawText(r->cont ? "\xC2 " : "  ", 0, MARK_WIDTH);
            DrawText(r->hdr ? (Ptr)gReq->title + 1 : *gReq->body + r->off, 0, r->len);
            y += gRowH;
        }
        if (gReq->body)
            HSetState(gReq->body, saveState);
    }
    TextFont(0);
    TextSize(0);
}

/* Bold headline, set in the dialog's port before it is measured or drawn. */
static void headline_font(void)
{
    TextFont(GENEVA);
    TextSize(HEAD_SIZE);
    TextFace(bold);
}

static long measure_headline(const char *s, long n)
{
    return TextWidth((Ptr)s, 0, (short)n);
}

static pascal void draw_headline(DialogPtr d, short item)
{
    (void)d;
    (void)item;
    EraseRect(&gHeadRect);
    headline_font();
    MoveTo(gHeadRect.left + 1, gHeadRect.top + 13);
    DrawText(gHeadline, 0, (short)gHeadlineLen);
    TextFace(0);
    TextFont(0);
    TextSize(0);
}

/* The header the bridge sent ("Bash git status", "Edit src/parser.py"), on
   one line in plain Geneva. A header too wide for the line ends in an
   ellipsis; the full text is in the list below or in the headline. */
static pascal void draw_title(DialogPtr d, short item)
{
    const char *s = (const char *)gReq->title + 1;
    short n = gReq->title[0], w = gTitleRect.right - gTitleRect.left - 2;
    (void)d;
    (void)item;
    EraseRect(&gTitleRect);
    TextFont(GENEVA);
    TextSize(TITLE_SIZE);
    TextFace(0);
    MoveTo(gTitleRect.left + 1, gTitleRect.top + 12);
    if (TextWidth((Ptr)s, 0, n) <= w) {
        DrawText((Ptr)s, 0, n);
    } else {
        short ell = CharWidth((char)0xC9);
        while (n > 0 && TextWidth((Ptr)s, 0, n) + ell > w)
            n--;
        DrawText((Ptr)s, 0, n);
        DrawChar((char)0xC9);
    }
    TextFont(0);
    TextSize(0);
}

static void redraw_list(void)
{
    SetPort(gDlg);
    draw_list(gDlg, kPermList);
}

static pascal void bar_action(ControlHandle c, short part)
{
    short v = GetControlValue(c), page = gVisible - 1;
    switch (part) {
    case inUpButton:   v -= 1; break;
    case inDownButton: v += 1; break;
    case inPageUp:     v -= page; break;
    case inPageDown:   v += page; break;
    default: return;
    }
    SetControlValue(c, v);
    redraw_list();
}

static ControlActionUPP gBarUPP;

/* ---- the dialog --------------------------------------------------------- */

static void flash(short item)
{
    short type;
    Handle h;
    Rect r;
    unsigned long t;
    GetDialogItem(gDlg, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, inButton);
    Delay(8, &t);
    HiliteControl((ControlHandle)h, 0);
}

/* Part of the request was lost: say so, and leave only Deny and Cmd-period. */
static void show_incomplete(void)
{
    /* The headline already reads "Incomplete request: deny it". */
    short type, i;
    Handle h;
    Rect r;
    for (i = 0; i < 2; i++) {
        GetDialogItem(gDlg, i == 0 ? kPermAllow : kPermAlways, &type, &h, &r);
        HiliteControl((ControlHandle)h, 255);
    }
}

/* ---- the Rule dialog ---------------------------------------------------- */

static unsigned long gRuleArmedAt;

static void set_text(DialogPtr d, short item, const char *s, long n)
{
    short type;
    Handle h;
    Rect r;
    Str255 ps;
    if (n > 255)
        n = 255;
    ps[0] = (unsigned char)n;
    memcpy(ps + 1, s, n);
    GetDialogItem(d, item, &type, &h, &r);
    SetDialogItemText(h, ps);
}

/* The reason line and the Save Rule button follow what the field holds. */
static Boolean show_rule_state(DialogPtr d)
{
    short type;
    Handle h;
    Rect r;
    Str255 ps;
    char text[RULE_LEN + 1];
    const char *why;
    long n;
    GetDialogItem(d, kRuleField, &type, &h, &r);
    GetDialogItemText(h, ps);
    n = ps[0] > RULE_LEN - 1 ? RULE_LEN - 1 : ps[0];
    memcpy(text, ps + 1, n);
    text[n] = 0;
    if (n == 0)
        why = "Type the rule to save.";
    else
        why = rule_check(text, n);
    if (why)
        set_text(d, kRuleReason, why, strlen(why));
    else
        set_text(d, kRuleReason, "This request is allowed and the rule is saved.",
                 strlen("This request is allowed and the rule is saved."));
    GetDialogItem(d, kRuleSave, &type, &h, &r);
    HiliteControl((ControlHandle)h, why ? 255 : 0);
    return why == NULL;
}

static pascal Boolean rule_filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    Point p;

    session_pump();
    SetPort(d);
    if (gReq->close_requested) {
        *item = -1;
        return true;
    }
    switch (ev->what) {
    case keyDown:
    case autoKey:
        ch = (char)(ev->message & charCodeMask);
        if ((ev->modifiers & cmdKey) && ch == '.') {
            *item = kRuleCancel;
            return true;
        }
        /* Return and Enter never save; Cmd keys stay out of the field (no menus here). */
        if (ch == '\r' || ch == 3 || (ev->modifiers & cmdKey))
            ev->what = nullEvent;
        return false;
    case mouseDown:
        p = ev->where;
        GlobalToLocal(&p);
        if (TickCount() - gRuleArmedAt < ARM_TICKS) {
            short type;
            Handle h;
            Rect r;
            short i;
            for (i = kRuleSave; i <= kRuleCancel; i++) {
                GetDialogItem(d, i, &type, &h, &r);
                if (PtInRect(p, &r)) {
                    ev->what = nullEvent;   /* a click aimed at the window behind */
                    break;
                }
            }
        }
        return false;
    case updateEvt:
        if ((DialogPtr)ev->message != d && app_update((WindowPtr)ev->message)) {
            SetPort(d);
            ev->what = nullEvent;
        }
        return false;
    }
    return false;
}

/* 1: Save Rule (the rule is in q->rule), 0: Cancel, -1: the request closed. */
static short rule_dialog(PermReq *q)
{
    DialogPtr d;
    ModalFilterUPP f;
    short hit = 0, type, result;
    Handle h;
    Rect r;
    Str255 ps;
    long n;
    GrafPtr save;

    GetPort(&save);
    FlushEvents(keyDownMask | autoKeyMask | mDownMask | mUpMask, 0);
    d = GetNewDialog(kRuleDialog, NULL, (WindowPtr)-1);
    if (!d)
        return 0;
    SetPort(d);
    n = strlen(q->rule);
    set_text(d, kRuleField, q->rule, n);
    SelectDialogItemText(d, kRuleField, 0, 32767);
    show_rule_state(d);
    ShowWindow(d);
    gRuleArmedAt = TickCount();
    f = NewModalFilterUPP(rule_filter);
    for (;;) {
        ModalDialog(f, &hit);
        if (hit == kRuleCancel || hit < 0)
            break;
        if (hit == kRuleField) {
            show_rule_state(d);
        } else if (hit == kRuleSave) {
            if (TickCount() - gRuleArmedAt < ARM_TICKS)
                continue;
            if (show_rule_state(d))
                break;
        }
    }
    if (hit == kRuleSave) {
        GetDialogItem(d, kRuleField, &type, &h, &r);
        GetDialogItemText(h, ps);
        n = ps[0] > RULE_LEN - 1 ? RULE_LEN - 1 : ps[0];
        memcpy(q->rule, ps + 1, n);
        q->rule[n] = 0;
        result = 1;
    } else {
        result = hit < 0 ? -1 : 0;
    }
    DisposeDialog(d);
    DisposeModalFilterUPP(f);
    SetPort(save);
    return result;
}

static pascal Boolean filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    Point p;
    ControlHandle c;
    short part;

    session_pump();
    if (gReq->close_requested) {
        *item = -1;
        return true;
    }
    switch (ev->what) {
    case keyDown:
        ch = (char)(ev->message & charCodeMask);
        if (ev->modifiers & cmdKey) {
            if ((ch == 'r' || ch == 'R') && gReq->can_rule && !gReq->incomplete) { *item = kPermRule; flash(kPermRule); return true; }
            if ((ch == 'y' || ch == 'Y') && !gReq->incomplete) { *item = kPermAllow; flash(kPermAllow); return true; }
            if (ch == 'n' || ch == 'N') { *item = kPermDeny; flash(kPermDeny); return true; }
            if ((ch == 'a' || ch == 'A') && !gReq->incomplete) { *item = kPermAlways; flash(kPermAlways); return true; }
            if (ch == '.') { *item = -2; return true; }
        }
        ev->what = nullEvent;     /* Return, Enter and plain keys do nothing */
        return false;
    case autoKey:                 /* a held key never answers */
        ev->what = nullEvent;     /* Return, Enter and plain keys do nothing */
        return false;
    case mouseDown:
        p = ev->where;
        SetPort(d);
        GlobalToLocal(&p);
        part = FindControl(p, d, &c);
        if (c == gBar && part) {
            if (part == inThumb) {
                TrackControl(c, p, NULL);
                redraw_list();
            } else {
                TrackControl(c, p, gBarUPP);
            }
            ev->what = nullEvent;
        } else if (TickCount() - gArmedAt < ARM_TICKS) {
            ev->what = nullEvent;  /* a click aimed at the window, not an answer */
        }
        return false;
    case updateEvt:
        if ((DialogPtr)ev->message != d && app_update((WindowPtr)ev->message)) {
            SetPort(d);
            ev->what = nullEvent;
        }
        return false;
    }
    return false;
}

short permdlg_run(PermReq *q)
{
    short hit = 0, type, cols, result;
    Handle h;
    Rect barRect;
    FontInfo fi;
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP, headUPP, titleUPP;
    GrafPtr save;

    if (q->close_requested)
        return kPermResultClosed;
    gReq = q;
    GetPort(&save);
    FlushEvents(keyDownMask | autoKeyMask | mDownMask | mUpMask, 0);
    gDlg = GetNewDialog(kPermDialog, NULL, (WindowPtr)-1);
    if (!gDlg)
        return kPermResultDeny;
    SetPort(gDlg);

    TextFont(MONACO);
    TextSize(LIST_SIZE);
    GetFontInfo(&fi);
    gRowH = fi.ascent + fi.descent + fi.leading;
    gRowAscent = fi.ascent;

    GetDialogItem(gDlg, kPermSummary, &type, &h, &gTitleRect);
    titleUPP = NewUserItemUPP(draw_title);
    SetDialogItem(gDlg, kPermSummary, type, (Handle)titleUPP, &gTitleRect);
    /* Does the header fit its one line? If not, the list will open with it. */
    TextFont(GENEVA);
    TextSize(TITLE_SIZE);
    gTitleCut = TextWidth((Ptr)q->title + 1, 0, q->title[0]) > gTitleRect.right - gTitleRect.left - 2;
    TextFont(MONACO);
    TextSize(LIST_SIZE);

    GetDialogItem(gDlg, kPermList, &type, &h, &gList);
    drawUPP = NewUserItemUPP(draw_list);
    SetDialogItem(gDlg, kPermList, type, (Handle)drawUPP, &gList);
    barRect = gList;
    barRect.left = barRect.right - 16;
    gList.right -= 15;
    gVisible = (gList.bottom - gList.top - 2) / gRowH;
    cols = (gList.right - gList.left - 8) / CharWidth('0') - MARK_WIDTH;
    build_rows(cols);
    TextFont(0);
    TextSize(0);

    gBarUPP = NewControlActionUPP(bar_action);
    gBar = NewControl(gDlg, &barRect, "\p", true, 0, 0,
                      gNRows > gVisible ? (short)(gNRows - gVisible) : 0,
                      scrollBarProc, 0);

    /* The headline comes from what was received: counts of a diff or file,
       or a note to deny when part of the request was lost. */
    GetDialogItem(gDlg, kPermHeadline, &type, &h, &gHeadRect);
    headUPP = NewUserItemUPP(draw_headline);
    SetDialogItem(gDlg, kPermHeadline, type, (Handle)headUPP, &gHeadRect);
    headline_font();
    /* Measuring can load a font and move memory, so the body stays locked. */
    if (q->body && q->line_ends) {
        char bodyState = HGetState(q->body), endsState = HGetState(q->line_ends);
        HLock(q->body);
        HLock(q->line_ends);
        gHeadlineLen = permsum_build((const char *)q->title + 1, q->title[0],
                                     *q->body, (const long *)*q->line_ends,
                                     q->nlines, q->incomplete,
                                     measure_headline, gHeadRect.right - gHeadRect.left - 4,
                                     gHeadline, sizeof(gHeadline));
        HSetState(q->body, bodyState);
        HSetState(q->line_ends, endsState);
    } else {
        gHeadlineLen = permsum_build((const char *)q->title + 1, q->title[0],
                                     NULL, NULL, 0, q->incomplete,
                                     measure_headline, gHeadRect.right - gHeadRect.left - 4,
                                     gHeadline, sizeof(gHeadline));
    }
    TextFace(0);
    TextFont(0);
    TextSize(0);

    if (q->incomplete)
        show_incomplete();
    GetDialogItem(gDlg, kPermRule, &type, &h, &barRect);
    if (q->can_rule && !q->incomplete) {
        set_text(gDlg, kPermHint, "\021Y \021N \021A \021R \021. stops",
                 strlen("\021Y \021N \021A \021R \021. stops"));
    } else {
        HideControl((ControlHandle)h);   /* three buttons, as before */
    }
    ShowWindow(gDlg);
    gArmedAt = TickCount();
    filterUPP = NewModalFilterUPP(filter);
    for (;;) {
        ModalDialog(filterUPP, &hit);
        if (hit == kPermRule) {
            short r = rule_dialog(q);
            if (r == 1)
                break;
            if (r < 0) {
                hit = -1;
                break;
            }
            gArmedAt = TickCount();    /* clicks right after the other dialog closes are not answers */
            continue;
        }
        if (hit == kPermAllow || hit == kPermDeny || hit == kPermAlways || hit < 0)
            break;
    }
    switch (hit) {
    case kPermAllow:  result = kPermResultAllow; break;
    case kPermAlways: result = kPermResultAlways; break;
    case kPermDeny:   result = kPermResultDeny; break;
    case kPermRule:   result = kPermResultRule; break;
    case -2:          result = kPermResultStop; break;
    default:          result = kPermResultClosed; break;
    }
    DisposeDialog(gDlg);
    DisposeModalFilterUPP(filterUPP);
    DisposeUserItemUPP(drawUPP);
    DisposeUserItemUPP(headUPP);
    DisposeUserItemUPP(titleUPP);
    DisposeControlActionUPP(gBarUPP);
    gDlg = NULL;
    gReq = NULL;
    SetPort(save);
    return result;
}
