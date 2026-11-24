#include "opendlg.h"
#include "files.h"
#include "modem.h"
#include "resources.h"
#include "window.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Events.h>
/* List Manager: toolbox.h */
#include <Memory.h>
#include <Quickdraw.h>
#include <TextUtils.h>
#include <stdio.h>
#include <string.h>

#define MAX_ENTRIES 500          /* the bridge cuts a listing here too */
#define PATH_MAX_LEN 255

static DialogPtr gDlg;
static ListHandle gList;
static Rect gListRect;
static Tab *gTab;
static Boolean gRunning;         /* the dialog is up: listing events are ours */
static Boolean gLoading;
static Boolean gCut;
static Boolean gFailed;
static char gPath[PATH_MAX_LEN + 1];
static char gWhy[128];
static short gCount;
static Boolean gIsDir[MAX_ENTRIES];
static Handle gNames;            /* names back to back, NUL after each */
static long gNamesLen;
static long gNameOff[MAX_ENTRIES];
static Boolean gDirty;           /* rows changed: redraw */
static const char *gShownWait;   /* the waiting project the dialog shows */

/* ---- helpers ------------------------------------------------------------- */

static void set_text(short item, const char *s)
{
    short type, n = (short)strlen(s);
    Handle h;
    Rect r;
    Str255 p;
    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    GetDialogItem(gDlg, item, &type, &h, &r);
    SetDialogItemText(h, p);
}

static void set_enabled(short item, Boolean on)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(gDlg, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, on ? 0 : 255);
}

static const char *name_at(short row)
{
    return *gNames + gNameOff[row];
}

static void format_size(long size, char *out, short max)
{
    if (size < 1024)
        snprintf(out, max, "%ld bytes", size);
    else if (size < 1024L * 1024)
        snprintf(out, max, "%ld KB", (size + 1023) / 1024);
    else
        snprintf(out, max, "%ld.%ld MB", size / (1024L * 1024),
                 (size % (1024L * 1024)) * 10 / (1024L * 1024));
}

static Boolean selected(short *row)
{
    Cell c;
    c.h = c.v = 0;
    if (!LGetSelect(true, &c, gList))
        return false;
    *row = c.v;
    return true;
}

static void update_controls(void)
{
    short row;
    char s[TAB_NAME_MAX + PATH_MAX_LEN + 4];
    set_enabled(kOpenOpen, !gLoading && selected(&row));
    set_enabled(kOpenUp, !gLoading && gPath[0] != 0);
    snprintf(s, sizeof(s), "%s/%s", gTab->project, gPath);
    set_text(kOpenPath, s);
}

static void show_message(void)
{
    if (gLoading)
        set_text(kOpenMessage, "Loading\xC9");
    else if (gFailed)
        set_text(kOpenMessage, gWhy);
    else if (gCount == 0)
        set_text(kOpenMessage, "Empty folder");
    else if (gCut)
        set_text(kOpenMessage, "Only the first 500 entries are shown.");
    else
        set_text(kOpenMessage, "");
}

/* Starts listing the folder in gPath. */
static void list_folder(void)
{
    gCount = 0;
    gNamesLen = 0;
    gCut = false;
    gFailed = false;
    LSetDrawingMode(false, gList);
    LDelRow(0, 0, gList);
    if ((*gList)->visible.top != 0)
        LScroll(0, (short)-(*gList)->visible.top, gList);   /* new folder starts at the top */
    LSetDrawingMode(true, gList);
    InvalRect(&gListRect);
    if (proto_list(session_proto(), gTab->sid, gPath)) {
        gLoading = true;
    } else {
        gLoading = false;
        gFailed = true;
        strcpy(gWhy, "The tab is not connected.");
    }
    show_message();
    update_controls();
}

/* Turns the bridge's refusal words into plain words. */
static void plain_reason(const char *t, long n, char *out, short max)
{
    if (n == 15 && memcmp(t, "outside project", 15) == 0)
        strcpy(out, "That folder is outside the project.");
    else if (n == 6 && memcmp(t, "hidden", 6) == 0)
        strcpy(out, "That folder is hidden.");
    else if (n == 9 && memcmp(t, "not found", 9) == 0)
        strcpy(out, "That folder no longer exists.");
    else if (n == 9 && memcmp(t, "files off", 9) == 0)
        strcpy(out, "The bridge does not allow file access (--files off).");
    else
        snprintf(out, max, "The folder could not be listed: %.*s", (int)n, t);
}

void opendlg_event(short sid, EventKind k, const char *t, long n)
{
    int is_dir;
    long size, nlen;
    const char *name;
    char cell[300], sz[24];
    Cell c;

    if (!gRunning || !gTab || sid != gTab->sid)
        return;
    switch (k) {
    case EV_LIST_BEGIN:
        break;
    case EV_LIST_ENTRY:
        if (gCount >= MAX_ENTRIES || !files_parse_entry(t, n, &is_dir, &size, &name, &nlen))
            break;
        if (nlen > 250)
            nlen = 250;
        if (!gNames)
            gNames = NewHandle(4096);
        if (!gNames)
            break;
        if (GetHandleSize(gNames) < gNamesLen + nlen + 1) {
            SetHandleSize(gNames, gNamesLen + nlen + 1 + 4096);
            if (MemError())
                break;
        }
        memcpy(*gNames + gNamesLen, name, nlen);
        (*gNames)[gNamesLen + nlen] = 0;
        gNameOff[gCount] = gNamesLen;
        gNamesLen += nlen + 1;
        gIsDir[gCount] = is_dir != 0;
        if (is_dir) {
            snprintf(cell, sizeof(cell), "%.*s/", (int)nlen, name);
        } else {
            format_size(size, sz, sizeof(sz));
            snprintf(cell, sizeof(cell), "%.*s   %s", (int)nlen, name, sz);
        }
        LSetDrawingMode(false, gList);
        LAddRow(1, gCount, gList);
        c.h = 0;
        c.v = gCount;
        LSetCell(cell, (short)strlen(cell), c, gList);
        gCount++;
        gDirty = true;
        break;
    case EV_LIST_END:
        gLoading = false;
        gCut = n > 0;
        gDirty = true;
        break;
    case EV_LIST_FAILED:
        gLoading = false;
        gFailed = true;
        plain_reason(t, n, gWhy, sizeof(gWhy));
        gDirty = true;
        break;
    default:
        break;
    }
}

static pascal void draw_list(DialogPtr d, short item)
{
    Rect r = gListRect;
    (void)item;
    InsetRect(&r, -1, -1);
    FrameRect(&r);
    /* LUpdate draws only rows that exist and InvalRect erases nothing, so
       rows from a longer earlier listing would stay on screen. */
    r = (*gList)->rView;
    EraseRect(&r);
    LUpdate(d->visRgn, gList);
}

static void select_row(short row)
{
    Cell c;
    c.h = 0;
    c.v = 0;
    while (LGetSelect(true, &c, gList)) {
        LSetSelect(false, c, gList);
        c.v++;
    }
    c.v = row;
    LSetSelect(true, c, gList);
    LAutoScroll(gList);
}

/* Typing selects by name prefix, as in Standard File. */
static void select_prefix(const char *typed, short n)
{
    short i, k;
    for (i = 0; i < gCount; i++) {
        const char *nm = name_at(i);
        for (k = 0; k < n && nm[k]; k++) {
            char a = nm[k], b = typed[k];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (b >= 'A' && b <= 'Z')
                b += 32;
            if (a != b)
                break;
        }
        if (k == n) {
            select_row(i);
            return;
        }
    }
}

/* Goes into a folder or reports a file choice. Returns true for a file. */
static Boolean choose(short row)
{
    const char *nm = name_at(row);
    long n = (long)strlen(gPath), k = (long)strlen(nm);
    if (n + k + 2 > PATH_MAX_LEN) {
        SysBeep(1);
        return false;
    }
    if (gIsDir[row]) {
        if (n)
            gPath[n++] = '/';
        memcpy(gPath + n, nm, k + 1);
        list_folder();
        return false;
    }
    return true;
}

static void go_up(void)
{
    char *slash = strrchr(gPath, '/');
    if (slash)
        *slash = 0;
    else
        gPath[0] = 0;
    list_folder();
}

static void show_waiting(void)
{
    const char *w = session_waiting_project();
    char s[120];
    if (w == gShownWait)
        return;
    gShownWait = w;
    if (w)
        snprintf(s, sizeof(s), "Claude is waiting for permission in %s.", w);
    else
        s[0] = 0;
    set_text(kOpenWaiting, s);
}

static char gTyped[32];
static short gTypedLen;
static unsigned long gTypedAt;

static pascal Boolean filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    Point p;
    short row;

    session_pump();
    SetPort(d);
    show_waiting();
    if (!gLoading && !gFailed && !session_tab_ready(gTab)) {
        gFailed = true;
        strcpy(gWhy, "The connection to this tab was lost.");
        gDirty = true;
    }
    if (gDirty) {
        gDirty = false;
        LSetDrawingMode(true, gList);
        InvalRect(&gListRect);
        show_message();
        update_controls();
    }
    switch (ev->what) {
    case keyDown:
    case autoKey:
        ch = (char)(ev->message & charCodeMask);
        if (((ev->modifiers & cmdKey) && ch == '.') || ch == 0x1B) {
            *item = kOpenCancel;
            return true;
        }
        if (ch == '\r' || ch == 3) {
            if (!gLoading && selected(&row))
                *item = kOpenOpen;
            else
                ev->what = nullEvent;
            return ev->what != nullEvent;
        }
        if ((ev->modifiers & cmdKey) && ch == 0x1E) {   /* Cmd-up: parent */
            if (gPath[0] && !gLoading)
                go_up();
        } else if (ch == 0x1E || ch == 0x1F) {
            if (gCount) {
                row = selected(&row) ? (short)(row + (ch == 0x1E ? -1 : 1)) : 0;
                if (row < 0)
                    row = 0;
                if (row >= gCount)
                    row = (short)(gCount - 1);
                select_row(row);
            }
        } else if (ch > ' ' && !(ev->modifiers & cmdKey)) {
            if (TickCount() - gTypedAt > 60)
                gTypedLen = 0;
            gTypedAt = TickCount();
            if (gTypedLen < (short)sizeof(gTyped))
                gTyped[gTypedLen++] = ch;
            select_prefix(gTyped, gTypedLen);
        }
        update_controls();
        ev->what = nullEvent;
        return false;
    case mouseDown:
        p = ev->where;
        GlobalToLocal(&p);
        if (PtInRect(p, &gListRect)) {
            Boolean dbl = LClick(p, ev->modifiers, gList);
            update_controls();
            if (dbl && !gLoading && selected(&row)) {
                *item = kOpenOpen;
                return true;
            }
            ev->what = nullEvent;
        }
        return false;
    case updateEvt:
        if ((DialogPtr)ev->message != d && app_update((WindowPtr)ev->message)) {
            SetPort(d);
            ev->what = nullEvent;
        }
        return false;
    case activateEvt:
        if ((DialogPtr)ev->message == d)
            LActivate((ev->modifiers & activeFlag) != 0, gList);
        return false;
    }
    return false;
}

Boolean opendlg_run(Tab *t, char *path, short max)
{
    short hit = 0, type, row;
    Handle h;
    Rect bounds = { 0, 0, 0, 1 }, view;
    Point cell = { 0, 0 };
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP;
    GrafPtr save;
    Boolean ok = false;

    if (!session_tab_ready(t))
        return false;
    GetPort(&save);
    gDlg = GetNewDialog(kOpenDialog, NULL, (WindowPtr)-1);
    if (!gDlg)
        return false;
    SetPort(gDlg);
    gTab = t;
    gPath[0] = 0;
    gShownWait = NULL;
    gTypedLen = 0;

    GetDialogItem(gDlg, kOpenList, &type, &h, &gListRect);
    drawUPP = NewUserItemUPP(draw_list);
    SetDialogItem(gDlg, kOpenList, type, (Handle)drawUPP, &gListRect);
    view = gListRect;
    view.right -= 15;
    gList = LNew(&view, &bounds, cell, 0, gDlg, true, false, false, true);
    if (!gList) {
        DisposeDialog(gDlg);
        DisposeUserItemUPP(drawUPP);
        SetPort(save);
        return false;
    }
    (*gList)->selFlags = lOnlyOne;
    gRunning = true;
    list_folder();
    SetDialogDefaultItem(gDlg, kOpenOpen);
    SetDialogCancelItem(gDlg, kOpenCancel);
    ShowWindow(gDlg);
    filterUPP = NewModalFilterUPP(filter);
    for (;;) {
        ModalDialog(filterUPP, &hit);
        if (hit == kOpenCancel)
            break;
        if (hit == kOpenUp && gPath[0] && !gLoading)
            go_up();
        if (hit == kOpenOpen && !gLoading && selected(&row) && choose(row)) {
            long n = (long)strlen(gPath);
            const char *nm = name_at(row);
            if (n + (long)strlen(nm) + 2 <= max) {
                strcpy(path, gPath);
                if (n)
                    strcat(path, "/");
                strcat(path, nm);
                ok = true;
                break;
            }
            SysBeep(1);
        }
    }
    gRunning = false;
    LDispose(gList);
    gList = NULL;
    DisposeDialog(gDlg);
    gDlg = NULL;
    DisposeModalFilterUPP(filterUPP);
    DisposeUserItemUPP(drawUPP);
    SetPort(save);
    return ok;
}
