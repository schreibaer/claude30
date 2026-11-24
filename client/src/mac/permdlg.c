#include "permdlg.h"
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

typedef struct {
    long off;                    /* offset into the request body */
    short len;
    Boolean cont;                /* continuation of the line above */
} Row;

static PermReq *gReq;            /* the request the open dialog shows */
static Handle gRows;
static long gNRows;
static unsigned long gArmedAt;   /* buttons take clicks from this tick on */

static DialogPtr gDlg;
static ControlHandle gBar;
static Rect gList;
static short gRowH, gRowAscent, gVisible;

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
    char saveState;
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
        MoveTo(inner.left + 4, y);
        DrawString("\p(no details)");
    }
    if (gReq->body && gNRows > 0) {
        saveState = HGetState(gReq->body);
        HLock(gReq->body);
        for (i = first; i < gNRows && i < first + gVisible; i++) {
            Row *r = &((Row *)*gRows)[i];
            MoveTo(inner.left + 3, y);
            DrawText(r->cont ? "\xC2 " : "  ", 0, MARK_WIDTH);
            DrawText(*gReq->body + r->off, 0, r->len);
            y += gRowH;
        }
        HSetState(gReq->body, saveState);
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
    static const unsigned char note[] = "\p  [Part of this request could not be shown. Deny it.]";
    short type, i;
    Handle h;
    Rect r;
    Str255 t;
    short room = 255 - gReq->title[0];
    short n = note[0] < room ? note[0] : room;
    memcpy(t, gReq->title, gReq->title[0] + 1);
    memcpy(t + 1 + t[0], note + 1, n);
    t[0] = (unsigned char)(t[0] + n);
    GetDialogItem(gDlg, kPermSummary, &type, &h, &r);
    SetDialogItemText(h, t);
    for (i = 0; i < 2; i++) {
        GetDialogItem(gDlg, i == 0 ? kPermAllow : kPermAlways, &type, &h, &r);
        HiliteControl((ControlHandle)h, 255);
    }
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
    Rect r, barRect;
    FontInfo fi;
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP;
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

    GetDialogItem(gDlg, kPermSummary, &type, &h, &r);
    SetDialogItemText(h, q->title);

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

    if (q->incomplete)
        show_incomplete();
    ShowWindow(gDlg);
    gArmedAt = TickCount();
    filterUPP = NewModalFilterUPP(filter);
    for (;;) {
        ModalDialog(filterUPP, &hit);
        if (hit == kPermAllow || hit == kPermDeny || hit == kPermAlways || hit < 0)
            break;
    }
    switch (hit) {
    case kPermAllow:  result = kPermResultAllow; break;
    case kPermAlways: result = kPermResultAlways; break;
    case kPermDeny:   result = kPermResultDeny; break;
    case -2:          result = kPermResultStop; break;
    default:          result = kPermResultClosed; break;
    }
    DisposeDialog(gDlg);
    DisposeModalFilterUPP(filterUPP);
    DisposeUserItemUPP(drawUPP);
    DisposeControlActionUPP(gBarUPP);
    gDlg = NULL;
    gReq = NULL;
    SetPort(save);
    return result;
}
