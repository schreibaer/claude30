#include "window.h"
#include "log.h"
#include "resources.h"
#include "toolbox.h"
#include <Fonts.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <ToolUtils.h>
#include <string.h>

#define FONT_SIZE    9
#define SCROLL_UNIT  4      /* pixels per scroll bar step; keeps values in a short */
#define INPUT_MAX    8000L
#define MARGIN       4

static WindowPtr gWin;
static TEHandle gTrans;       /* transcript, styled */
static TEHandle gInput;       /* input area */
static TEHandle gFocus;       /* the one that has the selection for Copy */
static ControlHandle gScroll, gSend;
static Rect gTransView, gInputView, gStatusRect;
static char gStatus[128];
static short gLineH, gAscent;  /* one line height for every style */
static Boolean gActive;

/* ---- styles ------------------------------------------------------------- */

typedef struct { short font; Style face; } StyleDef;

/* Fonts and faces per kind of transcript line, so no two kinds look alike
   on a 1-bit screen. Markers ("> ", bullet, "i ", "Error: ") come from the
   batcher. */
static const StyleDef kStyles[RS_COUNT] = {
    { 3, 0 },                     /* prose: Geneva */
    { 4, 0 },                     /* code: Monaco */
    { 3, bold },                  /* the user's prompt */
    { 3, italic },                /* tool line */
    { 3, italic },                /* bridge info */
    { 3, bold },                  /* tool error */
    { 3, underline },             /* turn result */
    { 3, bold | italic },         /* interrupted */
    { 3, bold },                  /* line noise */
    { 3, bold | underline }       /* new conversation */
};

static void measure_fonts(void)
{
    FontInfo fi;
    short font, h = 0, a = 0;
    for (font = 3; font <= 4; font++) {
        TextFont(font);
        TextSize(FONT_SIZE);
        TextFace(bold | italic);
        GetFontInfo(&fi);
        if (fi.ascent + fi.descent + fi.leading > h)
            h = fi.ascent + fi.descent + fi.leading;
        if (fi.ascent > a)
            a = fi.ascent;
    }
    TextFace(0);
    gLineH = h;
    gAscent = a;
}

/* ---- layout ------------------------------------------------------------- */

static void layout(void)
{
    Rect r = gWin->portRect;
    short statusTop = r.bottom - (gLineH + 5);
    short inputTop = statusTop - (gLineH * 5 + 8);

    SetRect(&gTransView, r.left + MARGIN, r.top + MARGIN, r.right - 16 - MARGIN, inputTop - MARGIN);
    SetRect(&gInputView, r.left + MARGIN, inputTop + MARGIN, r.right - 70, statusTop - MARGIN);
    SetRect(&gStatusRect, r.left + MARGIN, statusTop + 2, r.right - MARGIN, r.bottom - 1);
}

/* ---- scrolling ---------------------------------------------------------- */

static long text_height(void)
{
    short n = (*gTrans)->nLines;
    if (n <= 0)
        return 0;
    return TEGetHeight(n, 1, gTrans);
}

static long scrolled_px(void)
{
    return (long)((*gTrans)->viewRect.top - (*gTrans)->destRect.top);
}

static long max_scroll_px(void)
{
    long m = text_height() - (gTransView.bottom - gTransView.top);
    return m > 0 ? m : 0;
}

static void sync_scrollbar(void)
{
    long maxv = (max_scroll_px() + SCROLL_UNIT - 1) / SCROLL_UNIT;
    if (maxv > 32000)
        maxv = 32000;
    SetControlMaximum(gScroll, (short)maxv);
    SetControlValue(gScroll, (short)(scrolled_px() / SCROLL_UNIT));
}

static void scroll_to_px(long px)
{
    long maxpx = max_scroll_px(), delta;
    if (px > maxpx)
        px = maxpx;
    if (px < 0)
        px = 0;
    /* TEScroll takes a short; a jump across a full transcript can exceed it. */
    delta = scrolled_px() - px;
    while (delta != 0) {
        long step = delta > 30000 ? 30000 : delta < -30000 ? -30000 : delta;
        TEScroll(0, (short)step, gTrans);
        delta -= step;
    }
    sync_scrollbar();
}

static Boolean at_bottom(void)
{
    return scrolled_px() + SCROLL_UNIT >= max_scroll_px();
}

static pascal void scroll_action(ControlHandle c, short part);
static ControlActionUPP gScrollUPP;

static pascal void scroll_action(ControlHandle c, short part)
{
    long page = (gTransView.bottom - gTransView.top) - gLineH;
    long delta = 0;
    (void)c;
    switch (part) {
    case inUpButton:   delta = -gLineH; break;
    case inDownButton: delta = gLineH; break;
    case inPageUp:     delta = -page; break;
    case inPageDown:   delta = page; break;
    default: return;
    }
    scroll_to_px(scrolled_px() + delta);
}

/* ---- full screen -------------------------------------------------------- */

static Boolean gFull;
static Rect gNormalBounds;       /* content rect in global coordinates */
static short gSavedMBarH;

/* The desktop region (low memory GrayRgn at 0x09EE). GCC mistakes the
   fixed address for an empty array, so its bounds warning is off here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static RgnHandle gray_rgn(void)
{
    return GetGrayRgn();
}
#pragma GCC diagnostic pop

/* Moves the scroll bar, Send button and text views after a resize. */
static void relayout(void)
{
    Rect r;
    Boolean follow = at_bottom();
    long scrolled = scrolled_px();

    layout();
    (*gTrans)->viewRect = gTransView;
    (*gTrans)->destRect = gTransView;
    TECalText(gTrans);
    OffsetRect(&(*gTrans)->destRect, 0, (short)-(scrolled > 30000 ? 30000 : scrolled));

    (*gInput)->viewRect = gInputView;
    (*gInput)->destRect = gInputView;
    TECalText(gInput);
    TESelView(gInput);

    r = gWin->portRect;
    MoveControl(gScroll, r.right - 16, r.top - 1);
    SizeControl(gScroll, 17, (short)(gTransView.bottom + MARGIN - r.top + 1));
    MoveControl(gSend, gInputView.right + 8, gInputView.top + 8);
    SizeControl(gSend, (short)(r.right - 8 - gInputView.right - 8), 20);

    scroll_to_px(follow ? max_scroll_px() : scrolled);
    InvalRect(&gWin->portRect);
}

Boolean window_full_screen(void)
{
    return gFull;
}

void window_set_full_screen(Boolean on)
{
    Rect screen = qd.screenBits.bounds, bar;
    RgnHandle barRgn;
    Point tl;

    if (on == gFull)
        return;
    SetPort(gWin);
    barRgn = NewRgn();
    if (on) {
        gNormalBounds = gWin->portRect;
        tl.h = gNormalBounds.left;
        tl.v = gNormalBounds.top;
        LocalToGlobal(&tl);
        OffsetRect(&gNormalBounds, (short)(tl.h - gNormalBounds.left), (short)(tl.v - gNormalBounds.top));

        /* Give the menu bar strip to the desktop, then cover the screen.
           The title bar sits just above the top edge, out of sight. */
        gSavedMBarH = GetMBarHeight();
        SetRect(&bar, screen.left, screen.top, screen.right, screen.top + gSavedMBarH);
        RectRgn(barRgn, &bar);
        UnionRgn(gray_rgn(), barRgn, gray_rgn());
        LMSetMBarHeight(0);
        MoveWindow(gWin, screen.left, screen.top, true);
        SizeWindow(gWin, (short)(screen.right - screen.left), (short)(screen.bottom - screen.top), true);
        gFull = true;
    } else {
        MoveWindow(gWin, gNormalBounds.left, gNormalBounds.top, true);
        SizeWindow(gWin, (short)(gNormalBounds.right - gNormalBounds.left),
                   (short)(gNormalBounds.bottom - gNormalBounds.top), true);
        LMSetMBarHeight(gSavedMBarH);
        SetRect(&bar, screen.left, screen.top, screen.right, screen.top + gSavedMBarH);
        RectRgn(barRgn, &bar);
        DiffRgn(gray_rgn(), barRgn, gray_rgn());
        DrawMenuBar();
        gFull = false;
    }
    DisposeRgn(barRgn);
    SetPort(gWin);
    relayout();
}

/* ---- creation ----------------------------------------------------------- */

void window_create(void)
{
    Rect r;
    gWin = GetNewWindow(kMainWindow, NULL, (WindowPtr)-1);
    SetPort(gWin);
    measure_fonts();
    layout();

    gScrollUPP = NewControlActionUPP(scroll_action);
    gTrans = TEStyleNew(&gTransView, &gTransView);
    TEAutoView(false, gTrans);

    TextFont(3);
    TextSize(FONT_SIZE);
    gInput = TENew(&gInputView, &gInputView);
    TEAutoView(true, gInput);
    gFocus = gInput;

    r = gWin->portRect;
    SetRect(&r, r.right - 16, r.top - 1, r.right + 1, gTransView.bottom + MARGIN);
    gScroll = NewControl(gWin, &r, "\p", true, 0, 0, 0, scrollBarProc, 0);
    SetRect(&r, gInputView.right + 8, gInputView.top + 8, gWin->portRect.right - 8,
            gInputView.top + 28);
    gSend = NewControl(gWin, &r, "\pSend", true, 0, 0, 1, pushButProc, 0);
    window_set_send_enabled(false);
    window_set_status("Not connected");
    ShowWindow(gWin);
}

WindowPtr window_ptr(void)
{
    return gWin;
}

Boolean window_transcript_empty(void)
{
    return (*gTrans)->teLength == 0;
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_status(void)
{
    SetPort(gWin);
    EraseRect(&gStatusRect);
    TextFont(3);
    TextSize(FONT_SIZE);
    TextFace(0);
    MoveTo(gStatusRect.left + 2, gStatusRect.top + gAscent + 1);
    DrawText(gStatus, 0, (short)strlen(gStatus));
}

static void draw_frames(void)
{
    Rect r = gWin->portRect;
    short y1 = gTransView.bottom + MARGIN, y2 = gStatusRect.top - 2;
    MoveTo(r.left, y1);
    LineTo(r.right, y1);
    MoveTo(r.left, y2);
    LineTo(r.right, y2);
}

void window_update(void)
{
    SetPort(gWin);
    BeginUpdate(gWin);
    EraseRect(&gWin->portRect);
    draw_frames();
    TEUpdate(&gTransView, gTrans);
    TEUpdate(&gInputView, gInput);
    DrawControls(gWin);
    draw_status();
    EndUpdate(gWin);
}

void window_activate(Boolean active)
{
    gActive = active;
    SetPort(gWin);
    if (active) {
        TEActivate(gFocus);
        HiliteControl(gScroll, 0);
    } else {
        TEDeactivate(gFocus);
        HiliteControl(gScroll, 255);
    }
}

void window_set_status(const char *s)
{
    if (strcmp(s, gStatus) == 0)
        return;
    strncpy(gStatus, s, sizeof(gStatus) - 1);
    gStatus[sizeof(gStatus) - 1] = 0;
    if (gWin)
        draw_status();
}

void window_set_send_enabled(Boolean on)
{
    HiliteControl(gSend, on ? 0 : 255);
}

/* ---- transcript --------------------------------------------------------- */

static StScrpHandle gScrap;   /* one-style scrap reused for every insert */

static void insert_run(const char *text, long len, RunStyle style)
{
    ScrpSTElement *e;
    short n = (*gTrans)->teLength;
    if (!gScrap) {
        gScrap = (StScrpHandle)NewHandleClear(sizeof(StScrpRec));
        if (!gScrap)
            return;
    }
    (*gScrap)->scrpNStyles = 1;
    e = &(*gScrap)->scrpStyleTab[0];
    e->scrpStartChar = 0;
    e->scrpHeight = gLineH;
    e->scrpAscent = gAscent;
    e->scrpFont = kStyles[style].font;
    e->scrpFace = kStyles[style].face;
    e->scrpSize = FONT_SIZE;
    TESetSelect(n, n, gTrans);
    TEStyleInsert((Ptr)text, len, gScrap, gTrans);
}

/* While the log cannot be written, text stays in the window until it would
   pass this size; only then is old text removed without a copy. */
#define TE_HARD_LIMIT 31000L

static OSErr gLogErr;            /* last log write error, until reported */
static long gLostBytes;          /* removed without reaching the log */

/* Cuts the first cut bytes to the log, keeping the view on the same text. */
static void trim(long cut, long incoming)
{
    OSErr err;
    short line = 0, nLines = (*gTrans)->nLines;
    long hCut = 0, scrolled;
    Rect empty = { 0, 0, 0, 0 };
    RgnHandle saveClip = NewRgn();
    char state;

    while (line < nLines && (*gTrans)->lineStarts[line] < cut)
        line++;
    if (line > 0)
        hCut = TEGetHeight(line, 1, gTrans);

    state = HGetState((*gTrans)->hText);
    HLock((*gTrans)->hText);
    err = log_append(*(*gTrans)->hText, cut);
    HSetState((*gTrans)->hText, state);
    if (err) {
        gLogErr = err;
        if ((*gTrans)->teLength + incoming < TE_HARD_LIMIT
                && (*gTrans)->nLines < TRIM_AT_LINES + 300) {
            DisposeRgn(saveClip);
            return;              /* keep it; the next append tries again */
        }
        gLostBytes += cut;
    }

    /* Delete off screen, then move the text so the visible lines stay put. */
    GetClip(saveClip);
    ClipRect(&empty);
    scrolled = scrolled_px();
    TESetSelect(0, cut, gTrans);
    TEDelete(gTrans);
    if (hCut > scrolled)
        hCut = scrolled;
    OffsetRect(&(*gTrans)->destRect, 0, (short)hCut);
    SetClip(saveClip);
    DisposeRgn(saveClip);
    InvalRect(&gTransView);
}

static void trim_for(long incoming)
{
    long cut;
    char state = HGetState((*gTrans)->hText);
    HLock((*gTrans)->hText);
    cut = batch_trim_cut(*(*gTrans)->hText, (*gTrans)->teLength, (*gTrans)->nLines, incoming);
    HSetState((*gTrans)->hText, state);
    if (cut > 0)
        trim(cut, incoming);
}

void window_append(Batch *b)
{
    int i;
    Boolean follow;
    if (b->nruns == 0)
        return;
    SetPort(gWin);
    follow = at_bottom();
    for (i = 0; i < b->nruns; i++) {
        trim_for(b->runs[i].len);
        insert_run(b->text + b->runs[i].start, b->runs[i].len, b->runs[i].style);
    }
    trim_for(0);
    if (follow)
        scroll_to_px(max_scroll_px());
    else
        sync_scrollbar();
    batch_clear(b);
}

/* ---- input -------------------------------------------------------------- */

Handle window_input_text(long *len)
{
    *len = (*gInput)->teLength;
    return (*gInput)->hText;
}

void window_clear_input(void)
{
    SetPort(gWin);
    TESetSelect(0, 32767, gInput);
    TEDelete(gInput);
}

static void set_focus(TEHandle te)
{
    if (te == gFocus)
        return;
    if (gActive)
        TEDeactivate(gFocus);
    gFocus = te;
    if (gActive)
        TEActivate(gFocus);
}

Boolean window_click(EventRecord *ev)
{
    Point p = ev->where;
    ControlHandle c;
    short part;
    Boolean shift = (ev->modifiers & shiftKey) != 0;

    SetPort(gWin);
    GlobalToLocal(&p);
    part = FindControl(p, gWin, &c);
    if (c == gScroll && part) {
        if (part == inThumb) {
            TrackControl(c, p, NULL);
            scroll_to_px((long)GetControlValue(gScroll) * SCROLL_UNIT);
        } else {
            TrackControl(c, p, gScrollUPP);
        }
        return false;
    }
    if (c == gSend && part)
        return TrackControl(c, p, NULL) != 0;
    if (PtInRect(p, &gTransView)) {
        set_focus(gTrans);
        TEClick(p, shift, gTrans);
    } else if (PtInRect(p, &gInputView)) {
        set_focus(gInput);
        TEClick(p, shift, gInput);
    }
    return false;
}

short window_key(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    SetPort(gWin);
    if (ch == '\r' || ch == 3) {
        if (!(ev->modifiers & optionKey))
            return kKeySend;
        ch = '\r';            /* Option-Return: a line break */
    }
    set_focus(gInput);
    if (ch != '\b' && (*gInput)->teLength >= INPUT_MAX)
        return kKeyHandled;
    TEKey(ch, gInput);
    return kKeyHandled;
}

void window_idle(void)
{
    if (gActive)
        TEIdle(gFocus);
}

/* ---- Edit menu ---------------------------------------------------------- */

/* Copies the selection of te to the desk scrap as plain text. */
static void copy_selection(TEHandle te)
{
    short a = (*te)->selStart, b = (*te)->selEnd;
    char state;
    if (b <= a)
        return;
    ZeroScrap();
    state = HGetState((*te)->hText);
    HLock((*te)->hText);
    PutScrap(b - a, 'TEXT', *(*te)->hText + a);
    HSetState((*te)->hText, state);
}

static void paste_text(void)
{
    Handle h = NewHandle(0);
    long offset, len;
    if (!h)
        return;
    len = GetScrap(h, 'TEXT', &offset);
    if (len > 0 && (*gInput)->teLength - ((*gInput)->selEnd - (*gInput)->selStart) + len <= INPUT_MAX) {
        TEDelete(gInput);
        HLock(h);
        TEInsert(*h, len, gInput);
        HUnlock(h);
    }
    DisposeHandle(h);
}

void window_edit(short item)
{
    SetPort(gWin);
    switch (item) {
    case kCopyItem:
        copy_selection(gFocus);
        break;
    case kCutItem:
        if (gFocus == gInput) {
            copy_selection(gInput);
            TEDelete(gInput);
        }
        break;
    case kPasteItem:
        set_focus(gInput);
        paste_text();
        break;
    case kClearItem:
        if (gFocus == gInput)
            TEDelete(gInput);
        break;
    case kSelectAllItem:
        TESetSelect(0, 32767, gFocus);
        break;
    }
}

OSErr window_log_trouble(long *lost)
{
    OSErr err = gLogErr;
    *lost = gLostBytes;
    gLogErr = noErr;
    gLostBytes = 0;
    return err;
}
