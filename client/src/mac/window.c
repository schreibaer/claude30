#include "window.h"
#include "filewin.h"
#include "log.h"
#include "resources.h"
#include "toolbox.h"
#include <Fonts.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <string.h>

#define FONT_SIZE    9
#define SCROLL_UNIT  4      /* pixels per scroll bar step; keeps values in a short */
#define INPUT_MAX    8000L
#define MARGIN       4
#define TAB_H        16     /* the tab strip */
#define TAB_MAX_W    120

#ifndef truncMiddle
#define truncMiddle  0x4000 /* TruncString: cut in the middle (smTruncMiddle) */
#endif

static WindowPtr gWin;
static Tab *gTab;             /* the front tab, or NULL */
static TEHandle gTrans;       /* the front tab's transcript, or NULL */
static TEHandle gInput;       /* the front tab's input area, or NULL */
static TEHandle gFocus;       /* the one that has the selection for Copy */
static ControlHandle gScroll, gSend;
static Rect gTabsRect, gTransView, gInputView, gStatusRect;
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

    SetRect(&gTabsRect, r.left, r.top, r.right, r.top + TAB_H);
    SetRect(&gTransView, r.left + MARGIN, gTabsRect.bottom + MARGIN, r.right - 16 - MARGIN,
            inputTop - MARGIN);
    SetRect(&gInputView, r.left + MARGIN, inputTop + MARGIN, r.right - 70, statusTop - MARGIN);
    SetRect(&gStatusRect, r.left + MARGIN, statusTop + 2, r.right - MARGIN, r.bottom - 1);
}

/* ---- drawing suppression for background records ---------------------------- */

/* Background tabs share the window's port. Their records are edited with the
   clip region empty, so TextEdit computes everything and draws nothing. */
static RgnHandle gSavedClip;

static void quiet_begin(TEHandle te)
{
    Rect empty = { 0, 0, 0, 0 };
    if (te == gTrans || te == gInput)
        return;
    if (!gSavedClip)
        gSavedClip = NewRgn();
    GetClip(gSavedClip);
    ClipRect(&empty);
}

static void quiet_end(TEHandle te)
{
    if (te == gTrans || te == gInput)
        return;
    SetClip(gSavedClip);
}

/* ---- scrolling ---------------------------------------------------------- */

static long text_height(TEHandle te)
{
    short n = (*te)->nLines;
    if (n <= 0)
        return 0;
    return TEGetHeight(n, 1, te);
}

static long scrolled_px(TEHandle te)
{
    return (long)((*te)->viewRect.top - (*te)->destRect.top);
}

static long max_scroll_px(TEHandle te)
{
    long m = text_height(te) - ((*te)->viewRect.bottom - (*te)->viewRect.top);
    return m > 0 ? m : 0;
}

static void sync_scrollbar(void)
{
    long maxv;
    if (!gTrans) {
        SetControlMaximum(gScroll, 0);
        return;
    }
    maxv = (max_scroll_px(gTrans) + SCROLL_UNIT - 1) / SCROLL_UNIT;
    if (maxv > 32000)
        maxv = 32000;
    SetControlMaximum(gScroll, (short)maxv);
    SetControlValue(gScroll, (short)(scrolled_px(gTrans) / SCROLL_UNIT));
}

static void scroll_to_px(TEHandle te, long px)
{
    long maxpx = max_scroll_px(te), delta;
    if (px > maxpx)
        px = maxpx;
    if (px < 0)
        px = 0;
    delta = scrolled_px(te) - px;
    if (te != gTrans) {
        /* A hidden record only moves; it is redrawn when shown. */
        OffsetRect(&(*te)->destRect, 0, (short)delta);
        return;
    }
    /* TEScroll takes a short; a jump across a full transcript can exceed it. */
    while (delta != 0) {
        long step = delta > 30000 ? 30000 : delta < -30000 ? -30000 : delta;
        TEScroll(0, (short)step, te);
        delta -= step;
    }
    sync_scrollbar();
}

static Boolean at_bottom(TEHandle te)
{
    return scrolled_px(te) + SCROLL_UNIT >= max_scroll_px(te);
}

static pascal void scroll_action(ControlHandle c, short part);
static ControlActionUPP gScrollUPP;

static pascal void scroll_action(ControlHandle c, short part)
{
    long page = (gTransView.bottom - gTransView.top) - gLineH;
    long delta = 0;
    (void)c;
    if (!gTrans)
        return;
    switch (part) {
    case inUpButton:   delta = -gLineH; break;
    case inDownButton: delta = gLineH; break;
    case inPageUp:     delta = -page; break;
    case inPageDown:   delta = page; break;
    default: return;
    }
    scroll_to_px(gTrans, scrolled_px(gTrans) + delta);
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

/* Fits a tab's records to the current layout, keeping its scroll place. */
static void fit_tab(Tab *t)
{
    Boolean follow = at_bottom(t->trans);
    long scrolled = scrolled_px(t->trans);

    quiet_begin(t->trans);
    (*t->trans)->viewRect = gTransView;
    (*t->trans)->destRect = gTransView;
    TECalText(t->trans);
    OffsetRect(&(*t->trans)->destRect, 0, (short)-(scrolled > 30000 ? 30000 : scrolled));
    scroll_to_px(t->trans, follow ? max_scroll_px(t->trans) : scrolled_px(t->trans));
    quiet_end(t->trans);

    quiet_begin(t->input);
    (*t->input)->viewRect = gInputView;
    (*t->input)->destRect = gInputView;
    TECalText(t->input);
    quiet_end(t->input);
}

/* Moves the scroll bar, Send button and text views after a resize. */
static void relayout(void)
{
    Rect r;
    short i;

    layout();
    for (i = 0; i < tabs_count(); i++)
        fit_tab(tabs_at(i));
    if (gInput)
        TESelView(gInput);

    r = gWin->portRect;
    MoveControl(gScroll, r.right - 16, gTabsRect.bottom - 1);
    SizeControl(gScroll, 17, (short)(gTransView.bottom + MARGIN - gTabsRect.bottom + 1));
    MoveControl(gSend, gInputView.right + 8, gInputView.top + 8);
    SizeControl(gSend, (short)(r.right - 8 - gInputView.right - 8), 20);
    sync_scrollbar();
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

    r = gWin->portRect;
    SetRect(&r, r.right - 16, gTabsRect.bottom - 1, r.right + 1, gTransView.bottom + MARGIN);
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

Boolean window_tab_init(Tab *t)
{
    SetPort(gWin);
    t->trans = TEStyleNew(&gTransView, &gTransView);
    if (!t->trans)
        return false;
    TEAutoView(false, t->trans);
    TEDeactivate(t->trans);
    TextFont(3);
    TextSize(FONT_SIZE);
    t->input = TENew(&gInputView, &gInputView);
    if (!t->input) {
        TEDispose(t->trans);
        t->trans = NULL;
        return false;
    }
    TEAutoView(true, t->input);
    TEDeactivate(t->input);
    return true;
}

void window_tab_free(Tab *t)
{
    if (t == gTab)
        window_show_tab(NULL);
    if (t->trans)
        TEDispose(t->trans);
    if (t->input)
        TEDispose(t->input);
    t->trans = t->input = NULL;
}

Boolean window_transcript_empty(Tab *t)
{
    return (*t->trans)->teLength == 0;
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

static short tab_width(void)
{
    short n = tabs_count(), w;
    if (n == 0)
        return 0;
    w = (short)((gTabsRect.right - gTabsRect.left - 4) / n);
    return w > TAB_MAX_W ? TAB_MAX_W : w;
}

static void tab_rect(short i, Rect *r)
{
    short w = tab_width();
    SetRect(r, (short)(gTabsRect.left + 2 + i * w), gTabsRect.top + 2,
            (short)(gTabsRect.left + 2 + (i + 1) * w), gTabsRect.bottom);
}

/* One tab label. The front tab is open into the transcript; the others sit
   behind a line. Marks: a leading bullet while the turn runs, an underline
   for output that arrived in the background, and the whole label inverted
   while a permission request waits (the strongest mark hides the others). */
static void draw_tab(short i)
{
    Tab *t = tabs_at(i);
    Rect r, inner;
    Str255 label;
    short n = (short)strlen(t->project), room;
    Boolean front = t == gTab;
    Boolean waiting = t->want_perm;

    tab_rect(i, &r);
    inner = r;
    InsetRect(&inner, 1, 1);
    EraseRect(&r);
    MoveTo(r.left, r.bottom - 1);
    LineTo(r.left, r.top + 2);
    LineTo(r.left + 2, r.top);
    LineTo(r.right - 3, r.top);
    LineTo(r.right - 1, r.top + 2);
    LineTo(r.right - 1, r.bottom - 1);
    if (!front) {
        MoveTo(r.left, r.bottom - 1);
        LineTo(r.right - 1, r.bottom - 1);
    }

    TextFont(3);
    TextSize(FONT_SIZE);
    TextFace(front ? bold : 0);
    label[0] = 0;
    if (t->running && !waiting) {
        label[1] = (char)0xA5;   /* bullet */
        label[2] = ' ';
        label[0] = 2;
    }
    if (n > 255 - label[0])
        n = 255 - label[0];
    memcpy(label + 1 + label[0], t->project, n);
    label[0] = (unsigned char)(label[0] + n);
    room = (short)(r.right - r.left - 8);
    TruncString(room, label, truncMiddle);
    MoveTo(r.left + 4, r.bottom - 4);
    DrawString(label);
    if (t->fresh && !waiting && !front) {
        short w = StringWidth(label);
        MoveTo(r.left + 4, r.bottom - 2);
        LineTo(r.left + 4 + w, r.bottom - 2);
    }
    TextFace(0);
    if (waiting)
        InvertRect(&inner);
}

void window_draw_tabs(void)
{
    short i, n = tabs_count();
    Rect r;
    GrafPtr save;
    GetPort(&save);
    SetPort(gWin);
    EraseRect(&gTabsRect);
    for (i = 0; i < n; i++)
        draw_tab(i);
    /* The baseline the back tabs stand on. */
    if (n > 0) {
        tab_rect((short)(n - 1), &r);
        MoveTo(r.right, gTabsRect.bottom - 1);
        LineTo(gTabsRect.right, gTabsRect.bottom - 1);
        MoveTo(gTabsRect.left, gTabsRect.bottom - 1);
        LineTo(gTabsRect.left + 2, gTabsRect.bottom - 1);
    } else {
        MoveTo(gTabsRect.left, gTabsRect.bottom - 1);
        LineTo(gTabsRect.right, gTabsRect.bottom - 1);
    }
    SetPort(save);
}

static void draw_empty(void)
{
    static const char msg[] = "No project open. Press Cmd-T.";
    TextFont(3);
    TextSize(12);
    TextFace(0);
    MoveTo((short)((gTransView.left + gTransView.right - TextWidth((Ptr)msg, 0, sizeof(msg) - 1)) / 2),
           (short)((gTransView.top + gTransView.bottom) / 2));
    DrawText((Ptr)msg, 0, sizeof(msg) - 1);
    TextSize(FONT_SIZE);
}

void window_update(void)
{
    SetPort(gWin);
    BeginUpdate(gWin);
    EraseRect(&gWin->portRect);
    window_draw_tabs();
    draw_frames();
    if (gTrans) {
        TEUpdate(&gTransView, gTrans);
        TEUpdate(&gInputView, gInput);
    } else {
        draw_empty();
    }
    DrawControls(gWin);
    draw_status();
    EndUpdate(gWin);
}

Boolean app_update(WindowPtr w)
{
    if (w == gWin) {
        window_update();
        return true;
    }
    return filewin_update(w);
}

void window_activate(Boolean active)
{
    gActive = active;
    SetPort(gWin);
    if (gFocus) {
        if (active)
            TEActivate(gFocus);
        else
            TEDeactivate(gFocus);
    }
    HiliteControl(gScroll, active && gTrans ? 0 : 255);
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

void window_show_tab(Tab *t)
{
    SetPort(gWin);
    if (gFocus && gActive)
        TEDeactivate(gFocus);
    gTab = t;
    gTrans = t ? t->trans : NULL;
    gInput = t ? t->input : NULL;
    gFocus = gInput;
    if (t) {
        t->fresh = false;
        /* Hidden records moved without drawing; settle them in the view. */
        TECalText(t->trans);
        scroll_to_px(t->trans, scrolled_px(t->trans));
        if (gActive)
            TEActivate(gFocus);
    }
    sync_scrollbar();
    HiliteControl(gScroll, gActive && gTrans ? 0 : 255);
    InvalRect(&gWin->portRect);
}

/* ---- transcript --------------------------------------------------------- */

static StScrpHandle gScrap;   /* one-style scrap reused for every insert */

static void insert_run(TEHandle te, const char *text, long len, RunStyle style)
{
    ScrpSTElement *e;
    short n = (*te)->teLength;
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
    TESetSelect(n, n, te);
    TEStyleInsert((Ptr)text, len, gScrap, te);
}

/* While the log cannot be written, text stays in the window until it would
   pass this size; only then is old text removed without a copy. */
#define TE_HARD_LIMIT 31000L

/* Cuts the first cut bytes to the tab's log, keeping the view on the same
   text. */
static void trim(Tab *t, long cut, long incoming)
{
    TEHandle te = t->trans;
    OSErr err;
    short line = 0, nLines = (*te)->nLines;
    long hCut = 0, scrolled;
    Rect empty = { 0, 0, 0, 0 };
    RgnHandle saveClip = NewRgn();
    char state;

    while (line < nLines && (*te)->lineStarts[line] < cut)
        line++;
    if (line > 0)
        hCut = TEGetHeight(line, 1, te);

    state = HGetState((*te)->hText);
    HLock((*te)->hText);
    err = log_append(&t->log, *(*te)->hText, cut);
    HSetState((*te)->hText, state);
    if (err) {
        t->log_err = err;
        if ((*te)->teLength + incoming < TE_HARD_LIMIT
                && (*te)->nLines < TRIM_AT_LINES + 300) {
            DisposeRgn(saveClip);
            return;              /* keep it; the next append tries again */
        }
        t->lost_bytes += cut;
    }

    /* Delete off screen, then move the text so the visible lines stay put. */
    GetClip(saveClip);
    ClipRect(&empty);
    scrolled = scrolled_px(te);
    TESetSelect(0, cut, te);
    TEDelete(te);
    if (hCut > scrolled)
        hCut = scrolled;
    OffsetRect(&(*te)->destRect, 0, (short)hCut);
    SetClip(saveClip);
    DisposeRgn(saveClip);
    if (te == gTrans)
        InvalRect(&gTransView);
}

static void trim_for(Tab *t, long incoming)
{
    TEHandle te = t->trans;
    long cut;
    char state = HGetState((*te)->hText);
    HLock((*te)->hText);
    cut = batch_trim_cut(*(*te)->hText, (*te)->teLength, (*te)->nLines, incoming);
    HSetState((*te)->hText, state);
    if (cut > 0)
        trim(t, cut, incoming);
}

void window_append(Tab *t)
{
    int i;
    Boolean follow;
    Batch *b = &t->batch;
    TEHandle te = t->trans;
    if (b->nruns == 0)
        return;
    SetPort(gWin);
    follow = at_bottom(te);
    quiet_begin(te);
    for (i = 0; i < b->nruns; i++) {
        trim_for(t, b->runs[i].len);
        insert_run(te, b->text + b->runs[i].start, b->runs[i].len, b->runs[i].style);
    }
    trim_for(t, 0);
    if (follow)
        scroll_to_px(te, max_scroll_px(te));
    quiet_end(te);
    if (te == gTrans)
        sync_scrollbar();
    else if (!t->fresh) {
        t->fresh = true;
        window_draw_tabs();
    }
    batch_clear(b);
}

/* ---- input -------------------------------------------------------------- */

Handle window_input_text(long *len)
{
    if (!gInput) {
        *len = 0;
        return NULL;
    }
    *len = (*gInput)->teLength;
    return (*gInput)->hText;
}

void window_clear_input(void)
{
    if (!gInput)
        return;
    SetPort(gWin);
    TESetSelect(0, 32767, gInput);
    TEDelete(gInput);
}

static void set_focus(TEHandle te)
{
    if (te == gFocus)
        return;
    if (gActive && gFocus)
        TEDeactivate(gFocus);
    gFocus = te;
    if (gActive && gFocus)
        TEActivate(gFocus);
}

short window_click(EventRecord *ev, Tab **tab)
{
    Point p = ev->where;
    ControlHandle c;
    short part, i;
    Boolean shift = (ev->modifiers & shiftKey) != 0;
    Rect r;

    SetPort(gWin);
    GlobalToLocal(&p);
    if (PtInRect(p, &gTabsRect)) {
        for (i = 0; i < tabs_count(); i++) {
            tab_rect(i, &r);
            if (PtInRect(p, &r)) {
                *tab = tabs_at(i);
                return kClickTab;
            }
        }
        return kClickNone;
    }
    part = FindControl(p, gWin, &c);
    if (c == gScroll && part) {
        if (part == inThumb) {
            TrackControl(c, p, NULL);
            if (gTrans)
                scroll_to_px(gTrans, (long)GetControlValue(gScroll) * SCROLL_UNIT);
        } else {
            TrackControl(c, p, gScrollUPP);
        }
        return kClickNone;
    }
    if (c == gSend && part)
        return TrackControl(c, p, NULL) != 0 ? kClickSend : kClickNone;
    if (!gTrans)
        return kClickNone;
    if (PtInRect(p, &gTransView)) {
        set_focus(gTrans);
        TEClick(p, shift, gTrans);
    } else if (PtInRect(p, &gInputView)) {
        set_focus(gInput);
        TEClick(p, shift, gInput);
    }
    return kClickNone;
}

short window_key(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    SetPort(gWin);
    if (!gInput) {
        SysBeep(1);
        return kKeyHandled;
    }
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
    if (gActive && gFocus)
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
    if (!gFocus)
        return;
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
