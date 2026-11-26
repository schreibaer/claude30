#include "logwin.h"
#include "find.h"
#include "window.h"
#include "toolbox.h"
#include <Files.h>
#include <Fonts.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <stdio.h>
#include <string.h>

#define MONACO     4
#define FONT_SIZE  9
#define CHUNK      4096L
#define CONTEXT    1400L             /* bytes shown before and after a match */

static WindowPtr gWin;
static TEHandle gTE;
static Rect gView;
static short gSid;                   /* the tab it belongs to */
static Boolean gHidden;

static Boolean gHave;                /* a log search has a position */
static short gHaveSid;
static long gPos;                    /* start of the last log match */

void logwin_reset(void)
{
    gHave = false;
}

typedef struct { short ref; } LogRd;

static int rd(void *ctx, long off, long len, char *out)
{
    short ref = ((LogRd *)ctx)->ref;
    long n = len;
    if (SetFPos(ref, fsFromStart, off) != noErr)
        return 1;
    if (FSRead(ref, &n, out) != noErr || n != len)
        return 1;
    return 0;
}

static void c2p(const char *s, Str255 p)
{
    short n = (short)strlen(s);
    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static void make_window(void)
{
    Rect r = { 44, 16, 330, 496 };
    GrafPtr save;
    FontInfo fi;
    if (gWin)
        return;
    gWin = NewWindow(NULL, &r, "\pLog", false, documentProc, (WindowPtr)-1, true, 0);
    if (!gWin)
        return;
    GetPort(&save);
    SetPort(gWin);
    TextFont(MONACO);
    TextSize(FONT_SIZE);
    GetFontInfo(&fi);
    gView = gWin->portRect;
    InsetRect(&gView, 4, 3);
    gTE = TENew(&gView, &gView);
    if (!gTE) {
        DisposeWindow(gWin);
        gWin = NULL;
    }
    SetPort(save);
}

/* Fills the window with the log text around [m, m+plen) and selects the match. */
static short show(Tab *t, long size, long m, long plen, short ref)
{
    long lo = m - CONTEXT, hi = m + plen + CONTEXT, n, i, s0;
    Ptr buf;
    char title[96];
    Str255 pt;
    LogRd lr;
    GrafPtr save;
    if (lo < 0)
        lo = 0;
    if (hi > size)
        hi = size;
    n = hi - lo;
    buf = NewPtr(n + 1);
    if (!buf)
        return LOG_ERROR;
    lr.ref = ref;
    if (rd(&lr, lo, n, buf)) {
        DisposePtr(buf);
        return LOG_ERROR;
    }
    /* Whole lines: drop a partial first line and a partial last one. */
    s0 = 0;
    if (lo > 0)
        for (i = 0; i < m - lo; i++)
            if (buf[i] == '\r') {
                s0 = i + 1;
                break;
            }
    if (hi < size) {
        for (i = n - 1; i > m - lo + plen; i--)
            if (buf[i] == '\r')
                break;
        if (i > m - lo + plen)
            n = i;
    }
    make_window();
    if (!gWin) {
        DisposePtr(buf);
        return LOG_ERROR;
    }
    gSid = t->sid;
    GetPort(&save);
    SetPort(gWin);
    TESetText(buf + s0, n - s0, gTE);
    TESetSelect(m - lo - s0, m - lo - s0 + plen, gTE);
    {
        /* Show the match a third of the way down the window. */
        short line = 0, k;
        long px;
        for (k = 0; k < (*gTE)->nLines && (*gTE)->lineStarts[k] <= m - lo - s0; k++)
            line = k;
        px = (long)line * (long)((*gTE)->lineHeight) - (gView.bottom - gView.top) / 3;
        if (px < 0)
            px = 0;
        (*gTE)->destRect = gView;
        OffsetRect(&(*gTE)->destRect, 0, (short)-px);
        TECalText(gTE);
    }
    DisposePtr(buf);
    snprintf(title, sizeof(title), "Log %ld%% - %.*s", size > 0 ? m * 100 / size : 0,
             (int)t->log.name[0], (char *)t->log.name + 1);
    c2p(title, pt);
    SetWTitle(gWin, pt);
    gHidden = false;
    ShowWindow(gWin);
    SelectWindow(gWin);
    InvalRect(&gWin->portRect);
    SetPort(save);
    return LOG_FOUND;
}

short logwin_search(Tab *t, const char *pat, short dir)
{
    short ref = 0, res;
    long size = 0, plen = (long)strlen(pat), m;
    Ptr buf;
    LogRd lr;
    OSErr err;
    if (t->chat || t->log.discard)
        return LOG_UNAVAILABLE;
    if (plen == 0 || plen > 200)
        return LOG_NONE;
    err = log_open_read(&t->log, &ref);
    if (err == fnfErr || err == dirNFErr)
        return LOG_NONE;                 /* nothing was written yet */
    if (err)
        return LOG_ERROR;
    if (GetEOF(ref, &size) != noErr) {
        FSClose(ref);
        return LOG_ERROR;
    }
    if (!(gHave && gHaveSid == t->sid)) {
        if (dir > 0) {
            FSClose(ref);
            return LOG_NONE;
        }
        gPos = size;                     /* start at the end of the log */
    }
    buf = NewPtr(CHUNK);
    if (!buf) {
        FSClose(ref);
        return LOG_ERROR;
    }
    lr.ref = ref;
    if (dir < 0)
        m = find_file_backward(rd, &lr, size, (gHave && gHaveSid == t->sid) ? gPos : size,
                               pat, plen, buf, CHUNK);
    else
        m = find_file_forward(rd, &lr, size, gPos + plen, pat, plen, buf, CHUNK);
    DisposePtr(buf);
    if (m == -2)
        res = LOG_ERROR;
    else if (m < 0)
        res = LOG_NONE;
    else {
        res = show(t, size, m, plen, ref);
        if (res == LOG_FOUND) {
            gHave = true;
            gHaveSid = t->sid;
            gPos = m;
        }
    }
    FSClose(ref);
    return res;
}

WindowPtr logwin_ptr(void) { return gWin; }

Boolean logwin_is(WindowPtr w)
{
    return w && w == gWin;
}

short logwin_sid(void) { return gWin ? gSid : 0; }

Boolean logwin_update(WindowPtr w)
{
    GrafPtr save;
    if (!logwin_is(w))
        return false;
    GetPort(&save);
    SetPort(w);
    BeginUpdate(w);
    EraseRect(&w->portRect);
    TEUpdate(&w->portRect, gTE);
    EndUpdate(w);
    SetPort(save);
    return true;
}

void logwin_activate(WindowPtr w, Boolean on)
{
    if (!logwin_is(w))
        return;
    SetPort(w);
    if (on)
        TEActivate(gTE);
    else
        TEDeactivate(gTE);
}

void logwin_click(WindowPtr w, EventRecord *ev)
{
    Point p = ev->where;
    if (!logwin_is(w))
        return;
    SetPort(w);
    GlobalToLocal(&p);
    TEClick(p, (ev->modifiers & shiftKey) != 0, gTE);
}

/* Up, Down, Page Up and Page Down scroll; the text cannot be changed. */
void logwin_key(WindowPtr w, EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    short code = (short)((ev->message & keyCodeMask) >> 8);
    short page = (short)(gView.bottom - gView.top);
    short step = 0, lh;
    if (!logwin_is(w))
        return;
    lh = (*gTE)->lineHeight;
    if (ch == 0x1E)
        step = lh;
    else if (ch == 0x1F)
        step = (short)-lh;
    else if (code == 0x74)
        step = (short)(page - lh);
    else if (code == 0x79)
        step = (short)-(page - lh);
    else {
        SysBeep(1);
        return;
    }
    SetPort(w);
    {
        long total = (long)TEGetHeight((*gTE)->nLines, 1, gTE);
        long scrolled = (long)((*gTE)->viewRect.top - (*gTE)->destRect.top);
        long max = total - (gView.bottom - gView.top);
        if (max < 0)
            max = 0;
        if (step > 0 && scrolled - step < 0)
            step = (short)scrolled;
        if (step < 0 && scrolled - step > max)
            step = (short)(scrolled - max);
        if (step)
            TEScroll(0, step, gTE);
    }
}

void logwin_copy(void)
{
    short a, b;
    char state;
    if (!gWin)
        return;
    a = (*gTE)->selStart;
    b = (*gTE)->selEnd;
    if (b <= a)
        return;
    ZeroScrap();
    state = HGetState((*gTE)->hText);
    HLock((*gTE)->hText);
    PutScrap(b - a, 'TEXT', *(*gTE)->hText + a);
    HSetState((*gTE)->hText, state);
}

void logwin_close(void)
{
    if (!gWin)
        return;
    TEDispose(gTE);
    DisposeWindow(gWin);
    gTE = NULL;
    gWin = NULL;
    gHave = false;
}

void logwin_show_tab(short sid)
{
    if (!gWin)
        return;
    if (gSid == sid && gHidden) {
        ShowWindow(gWin);
        gHidden = false;
    } else if (gSid != sid && !gHidden) {
        HideWindow(gWin);
        gHidden = true;
    }
}

void logwin_tab_closed(short sid)
{
    if (gWin && gSid == sid)
        logwin_close();
}
