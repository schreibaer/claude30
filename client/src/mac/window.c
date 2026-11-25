#include "window.h"
#include "filewin.h"
#include "log.h"
#include "mascot.h"
#include "modem.h"
#include "prefs.h"
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
static Rect gSpotRect;         /* the mascot's place beside Send while text is typed */
static Rect gSendRect;         /* the Send button */
static Rect gTriRect;          /* the collapse triangle on the line above the input */
static Boolean gCompact;       /* the input box is one line tall now */
static Boolean gAutoOpen;      /* ... but opened for a prompt longer than a line */
static char gStatus[128];
static short gLineH, gAscent;  /* one line height for every style */
static Boolean gActive;

static void mascot_erase(void);
static void mascot_invalidate(void);

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
    { 3, bold | underline },      /* new conversation */
    { 3, italic }                 /* a subagent's line */
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

#define COMPACT_H    24     /* the input band when collapsed to one line */
#define BAR_W        16     /* the transcript scroll bar's width, less its overlap */

#define POS_W        4      /* with the bar hidden: a rule and a 3-pixel position strip */
#define POS_FIELD_W  30     /* the "Top", "End" or "42%" field in the status line */
#define POS_MIN_H    6      /* the position marker's least height */

static Boolean gBarHidden;       /* Settings > Hide scroll bar */
static Rect gPosRect;            /* the position strip, right of the rule */
static Rect gMarkRect;           /* its marker as drawn */
static char gPosText[8];         /* the status line's position field as drawn */

static void layout(void)
{
    Rect r = gWin->portRect;
    short statusTop = r.bottom - (gLineH + 5);
    short inputTop = gCompact ? statusTop - COMPACT_H : statusTop - (gLineH * 5 + 8);
    short reserve = gBarHidden ? POS_W : BAR_W;

    SetRect(&gTabsRect, r.left, r.top, r.right, r.top + TAB_H);
    SetRect(&gTransView, r.left + MARGIN, gTabsRect.bottom + MARGIN, r.right - reserve - MARGIN,
            inputTop - MARGIN);
    SetRect(&gPosRect, r.right - POS_W + 1, gTabsRect.bottom, r.right, gTransView.bottom + MARGIN);
    SetRect(&gStatusRect, r.left + MARGIN, statusTop + 2, r.right - MARGIN, r.bottom - 1);
    SetRect(&gTriRect, r.left + 2, inputTop - 4, r.left + 12, inputTop + 4);
    if (gCompact) {
        /* One text line, the mascot's spot, then Send, all in one strip. */
        short top = (short)(inputTop + (COMPACT_H - gLineH) / 2);
        SetRect(&gInputView, r.left + MARGIN, top, r.right - 70 - MASCOT_W - 6, top + gLineH);
        SetRect(&gSpotRect, gInputView.right + 3, inputTop + (COMPACT_H - MASCOT_H) / 2,
                gInputView.right + 3 + MASCOT_W, inputTop + (COMPACT_H - MASCOT_H) / 2 + MASCOT_H);
        SetRect(&gSendRect, r.right - 62, inputTop + 2, r.right - 8, inputTop + 22);
    } else {
        short cx;
        SetRect(&gInputView, r.left + MARGIN, inputTop + MARGIN, r.right - 70, statusTop - MARGIN);
        cx = (short)((gInputView.right + r.right) / 2);
        SetRect(&gSpotRect, cx - MASCOT_W / 2, gInputView.top + 36, cx + MASCOT_W / 2,
                gInputView.top + 36 + MASCOT_H);
        SetRect(&gSendRect, gInputView.right + 8, gInputView.top + 8, r.right - 8,
                gInputView.top + 28);
    }
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

static void draw_status_pos(const char *pos);

/* ---- position cues while the scroll bar is hidden ---------------------- */

/* The transcript's full height for the cues, or 0 while the bar is shown.
   TEGetHeight walks every line, so callers measure once and pass it on. */
static long pos_total(void)
{
    return gBarHidden && gTrans ? text_height(gTrans) : 0;
}

static long view_height(void)
{
    return (*gTrans)->viewRect.bottom - (*gTrans)->viewRect.top;
}

/* The status line's position field: "Top", "End" or a percentage, empty
   while the bar is shown or the whole transcript fits. */
static void pos_text(char *out, long total)
{
    long maxpx, px, pct;
    out[0] = 0;
    if (total <= 0 || (maxpx = total - view_height()) <= 0)
        return;
    px = scrolled_px(gTrans);
    if (px <= 0) {
        strcpy(out, "Top");
    } else if (px + SCROLL_UNIT >= maxpx) {
        strcpy(out, "End");
    } else {
        pct = px * 100 / maxpx;
        if (pct < 1)
            pct = 1;
        if (pct > 99)
            pct = 99;
        if (pct >= 10)
            *out++ = (char)('0' + pct / 10);
        *out++ = (char)('0' + pct % 10);
        *out++ = '%';
        *out = 0;
    }
}

/* The strip's marker covers the share of the transcript that is on screen. */
static void marker_rect(Rect *m, long total)
{
    long view, h, top, stripH = gPosRect.bottom - gPosRect.top;
    SetRect(m, 0, 0, 0, 0);
    if (total <= 0 || total <= (view = view_height()))
        return;
    top = scrolled_px(gTrans) * stripH / total;
    h = view * stripH / total;
    if (h < POS_MIN_H)
        h = POS_MIN_H;
    if (top + h > stripH)
        top = stripH - h;
    if (top < 0)
        top = 0;
    SetRect(m, gPosRect.left, (short)(gPosRect.top + top), gPosRect.right,
            (short)(gPosRect.top + top + h));
}

/* Draws the strip; with force the rule and track too, else only a moved marker. */
static void draw_strip(Boolean force, long total)
{
    Rect m;
    if (!gBarHidden)
        return;
    marker_rect(&m, total);
    if (!force && EqualRect(&m, &gMarkRect))
        return;
    SetPort(gWin);
    if (force) {
        MoveTo((short)(gPosRect.left - 1), gPosRect.top);
        LineTo((short)(gPosRect.left - 1), (short)(gPosRect.bottom - 1));
        EraseRect(&gPosRect);
    } else {
        EraseRect(&gMarkRect);
    }
    PaintRect(&m);
    gMarkRect = m;
}

/* Brings both position cues up to date; cheap when nothing moved. */
static void sync_position(void)
{
    char t[sizeof(gPosText)];
    long total;
    if (!gWin || !gBarHidden)
        return;
    total = pos_total();
    draw_strip(false, total);
    pos_text(t, total);
    if (strcmp(t, gPosText) != 0)
        draw_status_pos(t);
}

static void sync_scrollbar(void)
{
    long maxv;
    if (!gTrans) {
        SetControlMaximum(gScroll, 0);
        sync_position();
        return;
    }
    maxv = (max_scroll_px(gTrans) + SCROLL_UNIT - 1) / SCROLL_UNIT;
    if (maxv > 32000)
        maxv = 32000;
    SetControlMaximum(gScroll, (short)maxv);
    SetControlValue(gScroll, (short)(scrolled_px(gTrans) / SCROLL_UNIT));
    sync_position();
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
    SetRect(&gMarkRect, 0, 0, 0, 0);     /* the whole window is redrawn below */
    if (gBarHidden)
        HideControl(gScroll);
    else
        ShowControl(gScroll);
    MoveControl(gScroll, r.right - 16, gTabsRect.bottom - 1);
    SizeControl(gScroll, 17, (short)(gTransView.bottom + MARGIN - gTabsRect.bottom + 1));
    MoveControl(gSend, gSendRect.left, gSendRect.top);
    SizeControl(gSend, (short)(gSendRect.right - gSendRect.left), 20);
    sync_scrollbar();
    mascot_invalidate();
    InvalRect(&gWin->portRect);
}

void window_prefs_changed(void)
{
    if ((gPrefs.hide_scrollbar != 0) == gBarHidden)
        return;
    gBarHidden = !gBarHidden;
    SetPort(gWin);
    relayout();
}

/* ---- collapsing the input box -------------------------------------------- */

static void set_compact(Boolean on)
{
    if (on == gCompact)
        return;
    gCompact = on;
    SetPort(gWin);
    relayout();
}

/* A collapsed box opens while the prompt needs more than one line. */
static void check_grow(void)
{
    if (gCompact && gInput && (*gInput)->nLines > 1) {
        gAutoOpen = true;
        set_compact(false);
    }
}

Boolean window_input_compact(void)
{
    return gPrefs.compact_input != 0;
}

void window_toggle_input(void)
{
    gPrefs.compact_input = !gPrefs.compact_input;
    prefs_save();
    gAutoOpen = false;
    set_compact(gPrefs.compact_input != 0);
    check_grow();
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
    gCompact = gPrefs.compact_input != 0;
    gBarHidden = gPrefs.hide_scrollbar != 0;
    layout();
    gScrollUPP = NewControlActionUPP(scroll_action);

    r = gWin->portRect;
    SetRect(&r, r.right - 16, gTabsRect.bottom - 1, r.right + 1, gTransView.bottom + MARGIN);
    gScroll = NewControl(gWin, &r, "\p", !gBarHidden, 0, 0, 0, scrollBarProc, 0);
    gSend = NewControl(gWin, &gSendRect, "\pSend", true, 0, 0, 1, pushButProc, 0);
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

/* Draws the status text with pos, the position field, at its right end. */
static void draw_status_pos(const char *pos)
{
    short len = (short)strlen(gStatus), room = gStatusRect.right - gStatusRect.left - 4;
    short y = gStatusRect.top + gAscent + 1;
    SetPort(gWin);
    EraseRect(&gStatusRect);
    TextFont(3);
    TextSize(FONT_SIZE);
    TextFace(0);
    strcpy(gPosText, pos);
    if (gPosText[0]) {
        short w = TextWidth(gPosText, 0, (short)strlen(gPosText));
        MoveTo((short)(gStatusRect.right - 2 - w), y);
        DrawText(gPosText, 0, (short)strlen(gPosText));
        room -= POS_FIELD_W;
        while (len > 0 && TextWidth(gStatus, 0, len) > room)
            len--;              /* a long message stops short of the field */
    }
    MoveTo(gStatusRect.left + 2, y);
    DrawText(gStatus, 0, len);
}

static void draw_status(void)
{
    char pos[sizeof(gPosText)];
    pos_text(pos, pos_total());
    draw_status_pos(pos);
}

/* The Finder's disclosure triangle: pointing down while the input box is
   open, right while it is collapsed. */
static void draw_triangle(void)
{
    short i, x = (short)(gTriRect.left + 1), y = (short)(gTriRect.top);
    EraseRect(&gTriRect);
    if (gPrefs.compact_input) {
        for (i = 0; i < 7; i++) {             /* rows 1,2,3,4,3,2,1 wide */
            short w = (short)(i < 4 ? i : 6 - i);
            MoveTo((short)(x + 2), (short)(y + i));
            LineTo((short)(x + 2 + w), (short)(y + i));
        }
    } else {
        for (i = 0; i < 4; i++) {             /* rows 7,5,3,1 wide */
            MoveTo((short)(x + i), (short)(y + 2 + i));
            LineTo((short)(x + 6 - i), (short)(y + 2 + i));
        }
    }
}

static void draw_frames(void)
{
    Rect r = gWin->portRect;
    short y1 = gTransView.bottom + MARGIN, y2 = gStatusRect.top - 2;
    MoveTo(r.left, y1);
    LineTo(r.right, y1);
    MoveTo(r.left, y2);
    LineTo(r.right, y2);
    draw_triangle();
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
        mascot_invalidate();       /* erased above; the next idle draws it */
    } else {
        draw_empty();
    }
    DrawControls(gWin);
    {
        char pos[sizeof(gPosText)];
        long total = pos_total();
        draw_strip(true, total);
        pos_text(pos, total);
        draw_status_pos(pos);
    }
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
    if (gAutoOpen && (!gInput || (*gInput)->teLength == 0)) {
        gAutoOpen = false;
        set_compact(gPrefs.compact_input != 0);
    }
    check_grow();
    mascot_invalidate();
    InvalRect(&gWin->portRect);
}

/* ---- transcript --------------------------------------------------------- */

static StScrpHandle gScrap;   /* one-style scrap reused for every insert */

static void insert_at(TEHandle te, long n, const char *text, long len, RunStyle style)
{
    ScrpSTElement *e;
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

static void insert_run(TEHandle te, const char *text, long len, RunStyle style)
{
    insert_at(te, (*te)->teLength, text, len, style);
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
    pm_cut(&t->marks, cut);
    al_cut(&t->agents, cut);
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
        if (b->runs[i].style == RS_PROMPT) {
            /* The run may open with the line break that starts its block. */
            const char *p = b->text + b->runs[i].start;
            long k = 0;
            while (k < b->runs[i].len && p[k] == '\r')
                k++;
            pm_add(&t->marks, (*te)->teLength + k);
        }
        if (b->runs[i].style == RS_AGENT && t->agent_pending >= 0) {
            /* A subagent's line: remember where it sits so it can be rewritten. */
            const char *p = b->text + b->runs[i].start;
            long k = 0;
            AgentSlot *s = &t->agents.s[t->agent_pending];
            while (k < b->runs[i].len && p[k] == '\r')
                k++;
            s->drawn = 1;
            s->gone = 0;
            s->off = (*te)->teLength + k;
            s->len = b->runs[i].len - k;
            t->agent_pending = -1;
        }
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

/* ---- subagent lines ----------------------------------------------------- */

static long measure_px(const char *s, long n)
{
    return TextWidth((Ptr)s, 0, (short)n);
}

/* Cuts a composed line to one screen line of the transcript. */
static long fit_agent_line(char *s, long len)
{
    GrafPtr save;
    short font, size;
    Style face;
    long w = gTransView.right - gTransView.left - 4;
    GetPort(&save);
    SetPort(gWin);
    font = gWin->txFont;
    face = gWin->txFace;
    size = gWin->txSize;
    TextFont(kStyles[RS_AGENT].font);
    TextFace(kStyles[RS_AGENT].face);
    TextSize(FONT_SIZE);
    len = al_fit(s, len, w, measure_px);
    TextFont(font);
    TextFace(face);
    TextSize(size);
    SetPort(save);
    return len;
}

/* Draws slot's line at the end of the transcript as a new line. */
static void agent_new_line(Tab *t, int slot, const char *buf)
{
    batch_marker(&t->batch, RS_AGENT, buf);
    t->agent_pending = slot;
    window_append(t);
    t->agent_pending = -1;
}

void window_agent_new(Tab *t, int slot, const char *text)
{
    char buf[AL_TEXT + 4];
    long len = (long)strlen(text);
    memcpy(buf, text, len + 1);
    len = fit_agent_line(buf, len);
    buf[len] = 0;
    agent_new_line(t, slot, buf);
}

/* Replaces slot's line in place. The edit runs with the clip empty, so the
   window does not flicker; the view is redrawn once. */
void window_agent_update(Tab *t, int slot, const char *text)
{
    char buf[AL_TEXT + 4];
    long len = (long)strlen(text), off, oldlen, delta, st, en;
    AgentSlot *s = &t->agents.s[slot];
    TEHandle te = t->trans;
    Rect empty = { 0, 0, 0, 0 };
    RgnHandle saveClip;
    memcpy(buf, text, len + 1);
    len = fit_agent_line(buf, len);
    buf[len] = 0;

    window_append(t);                    /* earlier text first: offsets must be current */
    SetPort(gWin);
    trim_for(t, len);
    if (!s->drawn || s->gone) {          /* its line was trimmed away: say it again below */
        agent_new_line(t, slot, buf);
        return;
    }
    off = s->off;
    oldlen = s->len;
    delta = len - oldlen;
    st = (*te)->selStart;
    en = (*te)->selEnd;
    saveClip = NewRgn();
    GetClip(saveClip);
    ClipRect(&empty);
    TESetSelect(off, off + oldlen, te);
    TEDelete(te);
    insert_at(te, off, buf, len, RS_AGENT);
    if (st > off)
        st = st >= off + oldlen ? st + delta : off + len;
    if (en > off)
        en = en >= off + oldlen ? en + delta : off + len;
    TESetSelect(st, en, te);
    SetClip(saveClip);
    DisposeRgn(saveClip);
    s->len = len;
    al_shift(&t->agents, off, delta);
    pm_shift(&t->marks, off, delta);
    if (te == gTrans)
        InvalRect(&gTransView);
    else if (!t->fresh) {
        t->fresh = true;
        window_draw_tabs();
    }
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
    if (gAutoOpen) {              /* sent: the box collapses again */
        gAutoOpen = false;
        set_compact(gPrefs.compact_input != 0);
    }
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

void window_erase_input(void)
{
    if (!gInput) {
        SysBeep(1);
        return;
    }
    SetPort(gWin);
    set_focus(gInput);
    mascot_erase();               /* it may stand on the text */
    window_clear_input();
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
    if (PtInRect(p, &gTriRect)) {
        window_toggle_input();
        return kClickNone;
    }
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
    mascot_erase();               /* it may stand where the text goes */
    TEKey(ch, gInput);
    check_grow();
    return kKeyHandled;
}

/* ---- the mascot ----------------------------------------------------------- */

/* While the front tab's turn runs, the Claude mascot walks in the empty
   input box beside a word, or steps in place beside Send once text is typed.
   Paced by TickCount from window_idle, so it never holds up the serial pump,
   and it pauses while a modal dialog runs its own loop. */
static unsigned short gMRows[MF_COUNT][MASCOT_H];
static unsigned short gPRows[PF_COUNT][MASCOT_H];
static Boolean gMReady;
static Boolean gMDrawn;          /* something of it is on screen */
static MascotView gMView;
static Rect gMRect, gWRect;      /* the sprite and the word as drawn */
static Rect gPRect;              /* the little Mac, in the box only */

static void mascot_erase(void)
{
    if (!gMDrawn)
        return;
    SetPort(gWin);
    EraseRect(&gMRect);
    if (!EmptyRect(&gWRect))
        EraseRect(&gWRect);
    if (!EmptyRect(&gPRect))
        EraseRect(&gPRect);
    gMDrawn = false;
}

static void mascot_invalidate(void)
{
    gMDrawn = false;
    gMView.place = MP_HIDDEN;
}

static void blit16(unsigned short *rows, const Rect *dst)
{
    BitMap bm;
    bm.baseAddr = (Ptr)rows;
    bm.rowBytes = 2;
    SetRect(&bm.bounds, 0, 0, MASCOT_W, MASCOT_H);
    CopyBits(&bm, &gWin->portBits, &bm.bounds, dst, srcCopy, NULL);
}

static void mascot_sprite(MascotFrame f, const Rect *dst)
{
    short r;
    if (!gMReady) {
        short k;
        for (k = 0; k < MF_COUNT; k++)
            for (r = 0; r < MASCOT_H; r++)
                gMRows[k][r] = mascot_row((MascotFrame)k, r);
        for (k = 0; k < PF_COUNT; k++)
            for (r = 0; r < MASCOT_H; r++)
                gPRows[k][r] = mascot_prop_row((PropFrame)k, r);
        gMReady = true;
    }
    blit16(gMRows[f], dst);
}

static void mascot_tick(void)
{
    MascotView v;
    unsigned long now = TickCount();
    Boolean busy = false, waiting = false;
    const char *tool = NULL;
    Rect m, w, pr;
    short wordW = 0, left, track, mid;
    GrafPtr save;

    if (gTab && gInput) {
        busy = gTab->running;
        waiting = session_tab_waiting(gTab);
        tool = gTab->last_tool;
    }
    mascot_view(now, busy, waiting, gInput && (*gInput)->teLength > 0, tool, &v);

    GetPort(&save);
    SetPort(gWin);
    SetRect(&w, 0, 0, 0, 0);
    SetRect(&pr, 0, 0, 0, 0);
    if (v.place == MP_SPOT) {
        m = gSpotRect;
    } else if (v.place == MP_BOX) {
        /* The word stays put at the left; the mascot walks to its right. */
        TextFont(3);
        TextSize(FONT_SIZE);
        TextFace(italic);
        wordW = TextWidth(v.word, 0, (short)strlen(v.word));
        mid = (short)((gInputView.top + gInputView.bottom) / 2);
        left = (short)(gInputView.left + 8);
        SetRect(&w, left, (short)(mid - gLineH / 2), (short)(left + wordW + 1),
                (short)(mid - gLineH / 2 + gLineH));
        SetRect(&pr, (short)(gInputView.right - 4 - MASCOT_W), (short)(mid - MASCOT_H / 2),
                (short)(gInputView.right - 4), (short)(mid - MASCOT_H / 2 + MASCOT_H));
        left = (short)(w.right + 8);
        track = (short)(pr.left - 4 - MASCOT_W - left);
        if (track < 0)            /* too narrow for the Mac: no Mac */
            SetRect(&pr, 0, 0, 0, 0);
        if (!waiting)
            left = (short)(left + mascot_walk_x(now, track));
        SetRect(&m, left, (short)(mid - MASCOT_H / 2), (short)(left + MASCOT_W),
                (short)(mid - MASCOT_H / 2 + MASCOT_H));
    } else {
        SetRect(&m, 0, 0, 0, 0);
    }

    if (gMDrawn && v.place == gMView.place && EqualRect(&m, &gMRect)
            && EqualRect(&w, &gWRect) && EqualRect(&pr, &gPRect)
            && strcmp(v.word, gMView.word) == 0) {
        if (v.frame != gMView.frame) {
            mascot_sprite(v.frame, &m);   /* same place: the new frame covers the old */
            gMView.frame = v.frame;
        }
        if (v.prop != gMView.prop && !EmptyRect(&pr)) {
            blit16(gPRows[v.prop], &pr);
            gMView.prop = v.prop;
        }
        TextFace(0);
        SetPort(save);
        return;
    }
    if (gMDrawn || v.place != MP_HIDDEN) {
        Boolean wordSame = gMDrawn && EqualRect(&w, &gWRect) && strcmp(v.word, gMView.word) == 0;
        if (gMDrawn) {
            EraseRect(&gMRect);
            if (!wordSame && !EmptyRect(&gWRect))
                EraseRect(&gWRect);
        }
        if (gMDrawn && !EmptyRect(&gPRect) && !EqualRect(&pr, &gPRect))
            EraseRect(&gPRect);
        if (v.place != MP_HIDDEN) {
            mascot_sprite(v.frame, &m);
            if (!EmptyRect(&pr))
                blit16(gPRows[v.prop], &pr);
            if (!wordSame && v.word[0]) {
                MoveTo(w.left, (short)(w.top + gAscent));
                DrawText(v.word, 0, (short)strlen(v.word));
            }
        }
    }
    TextFace(0);
    gMDrawn = v.place != MP_HIDDEN;
    gMView = v;
    gMRect = m;
    gWRect = w;
    gPRect = pr;
    SetPort(save);
}

void window_idle(void)
{
    if (gActive && gFocus)
        TEIdle(gFocus);
    if (gWin)
        mascot_tick();
}

/* ---- keyboard scrolling ------------------------------------------------- */

/* Every transcript line has the same height (gLineH), so lines and pixels
   convert by multiplication. */
static short line_of(TEHandle te, long off)
{
    short lo = 0, hi = (short)((*te)->nLines - 1), m;
    if (hi < 0)
        return 0;
    while (lo < hi) {
        m = (short)((lo + hi + 1) / 2);
        if ((*te)->lineStarts[m] <= off)
            lo = m;
        else
            hi = (short)(m - 1);
    }
    return lo;
}

void window_jump_prompt(short dir)
{
    long px, pos, off, target;
    short top;
    if (!gTrans || !gTab) {
        SysBeep(1);
        return;
    }
    SetPort(gWin);
    px = scrolled_px(gTrans);
    top = (short)(px / gLineH);
    if (top >= (*gTrans)->nLines)
        top = (short)((*gTrans)->nLines - 1);
    pos = top >= 0 ? (*gTrans)->lineStarts[top] : 0;
    off = dir < 0 ? pm_prev(&gTab->marks, pos) : pm_next(&gTab->marks, pos);
    while (off >= 0) {
        target = (long)line_of(gTrans, off) * gLineH;
        if (target > max_scroll_px(gTrans))
            target = max_scroll_px(gTrans);
        if (target != px) {
            scroll_to_px(gTrans, target);
            return;
        }
        /* Near the end a prompt cannot reach the top: try the next one. */
        off = dir < 0 ? pm_prev(&gTab->marks, off) : pm_next(&gTab->marks, off);
    }
    if (dir > 0 && px < max_scroll_px(gTrans))
        scroll_to_px(gTrans, max_scroll_px(gTrans));
    else
        SysBeep(1);
}

/* The Control key modifier (Multiversal spells it ControlKey, Apple controlKey). */
#define kControlMask 0x1000

/* Virtual key codes of the extended keyboard's navigation keys. */
#define kHomeKey     0x73
#define kPageUpKey   0x74
#define kEndKey      0x77
#define kPageDownKey 0x79

Boolean window_scroll_key(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    short code = (short)((ev->message & keyCodeMask) >> 8);
    short mods = ev->modifiers;
    Boolean arrow = ch == 0x1E || ch == 0x1F;
    short dir = ch == 0x1E ? -1 : 1;
    long page, px;

    if (code == kPageUpKey || code == kPageDownKey || code == kHomeKey || code == kEndKey) {
        arrow = false;
    } else if (!arrow || !(mods & (cmdKey | optionKey | kControlMask))) {
        return false;
    }
    if (!gTrans) {
        SysBeep(1);
        return true;
    }
    SetPort(gWin);
    page = (gTransView.bottom - gTransView.top) - gLineH;
    px = scrolled_px(gTrans);
    if (code == kPageUpKey)
        scroll_to_px(gTrans, px - page);
    else if (code == kPageDownKey)
        scroll_to_px(gTrans, px + page);
    else if (code == kHomeKey)
        scroll_to_px(gTrans, 0);
    else if (code == kEndKey)
        scroll_to_px(gTrans, max_scroll_px(gTrans));
    else if ((mods & cmdKey) && (mods & optionKey))
        scroll_to_px(gTrans, dir < 0 ? 0 : max_scroll_px(gTrans));
    else if (mods & cmdKey)
        window_jump_prompt(dir);
    else if (mods & optionKey)
        scroll_to_px(gTrans, px + dir * page);
    else
        scroll_to_px(gTrans, px + dir * 3L * gLineH);
    return true;
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
    mascot_erase();
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
        check_grow();
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
