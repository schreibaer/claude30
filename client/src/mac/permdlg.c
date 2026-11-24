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
    long off;                    /* offset into gBody */
    short len;
    Boolean cont;                /* continuation of the line above */
} Row;

static Handle gBody;             /* all body lines, back to back */
static Handle gLineEnds;         /* long offsets: end of each body line */
static long gBodyLen, gNLines;
static Handle gRows;
static long gNRows;
static Str255 gTitle;
static Boolean gCloseRequested;
static Boolean gIncomplete;      /* some of the request could not be kept */
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

void permdlg_begin(const char *title, long len)
{
    gBodyLen = 0;
    gNLines = 0;
    gNRows = 0;
    gCloseRequested = false;
    gIncomplete = false;
    if (len > 255)
        len = 255;
    gTitle[0] = (unsigned char)len;
    memcpy(gTitle + 1, title, len);
}

void permdlg_line(const char *text, long len)
{
    if (!grow(&gBody, gBodyLen + len) || !grow(&gLineEnds, (gNLines + 1) * sizeof(long)))
    {
        gIncomplete = true;   /* out of memory: the line is dropped */
        return;
    }
    memcpy(*gBody + gBodyLen, text, len);
    gBodyLen += len;
    ((long *)*gLineEnds)[gNLines++] = gBodyLen;
}

void permdlg_mark_incomplete(void)
{
    gIncomplete = true;
}

void permdlg_request_close(void)
{
    gCloseRequested = true;
}

/* Splits body lines into rows that fit the list width. */
static void build_rows(short cols)
{
    long i, start = 0, end, pos;
    Row *r;
    gNRows = 0;
    if (cols < 8)
        cols = 8;
    for (i = 0; i < gNLines; i++) {
        end = ((long *)*gLineEnds)[i];
        pos = start;
        do {
            long n = end - pos;
            if (n > cols)
                n = cols;
            if (!grow(&gRows, (gNRows + 1) * sizeof(Row))) {
                gIncomplete = true;
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
    if (gBody) {
        saveState = HGetState(gBody);
        HLock(gBody);
        for (i = first; i < gNRows && i < first + gVisible; i++) {
            Row *r = &((Row *)*gRows)[i];
            MoveTo(inner.left + 3, y);
            DrawText(r->cont ? "\xC2 " : "  ", 0, MARK_WIDTH);
            DrawText(*gBody + r->off, 0, r->len);
            y += gRowH;
        }
        HSetState(gBody, saveState);
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
    short room = 255 - gTitle[0];
    short n = note[0] < room ? note[0] : room;
    memcpy(t, gTitle, gTitle[0] + 1);
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
    if (gCloseRequested) {
        *item = -1;
        return true;
    }
    switch (ev->what) {
    case keyDown:
        ch = (char)(ev->message & charCodeMask);
        if (ev->modifiers & cmdKey) {
            if ((ch == 'y' || ch == 'Y') && !gIncomplete) { *item = kPermAllow; flash(kPermAllow); return true; }
            if (ch == 'n' || ch == 'N') { *item = kPermDeny; flash(kPermDeny); return true; }
            if ((ch == 'a' || ch == 'A') && !gIncomplete) { *item = kPermAlways; flash(kPermAlways); return true; }
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
        if ((WindowPtr)ev->message == window_ptr()) {
            window_update();
            SetPort(d);
            ev->what = nullEvent;
        }
        return false;
    }
    return false;
}

short permdlg_run(void)
{
    short hit = 0, type, cols, result;
    Handle h;
    Rect r, barRect;
    FontInfo fi;
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP;
    GrafPtr save;

    if (gCloseRequested)
        return kPermResultClosed;
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
    SetDialogItemText(h, gTitle);

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

    if (gIncomplete)
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
    SetPort(save);
    return result;
}
