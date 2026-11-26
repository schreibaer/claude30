#include "picker.h"
#include "modem.h"
#include "resources.h"
#include "tabs.h"
#include "window.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Events.h>
/* List Manager: toolbox.h */
#include <Memory.h>
#include <Quickdraw.h>
#include <TextUtils.h>
#include <string.h>

#define OPEN_MARK " (open)"

static DialogPtr gDlg;
static ListHandle gList;
static Rect gListRect;
static Boolean gStale;           /* the bridge sent a new list */
static Boolean gChatOk;          /* the New Chat button is offered */

void picker_projects_changed(void)
{
    gStale = true;
}

static void set_item_enabled(short item, Boolean on)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(gDlg, item, &type, &h, &r);
    HiliteControl((ControlHandle)h, on ? 0 : 255);
}

static void set_message(const char *s)
{
    short type, n = (short)strlen(s);
    Handle h;
    Rect r;
    Str255 p;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    GetDialogItem(gDlg, kPickMessage, &type, &h, &r);
    SetDialogItemText(h, p);
}

/* The project name in row (without the open mark). Returns false if the row
   is a project already open in a tab. */
static Boolean row_name(short row, char *name, short max)
{
    Cell c;
    short len = max - 1;
    char buf[80];
    short k = (short)sizeof(buf) - 1;
    c.h = 0;
    c.v = row;
    LGetCell(buf, &k, c, gList);
    buf[k] = 0;
    if (k > (short)strlen(OPEN_MARK) && strcmp(buf + k - strlen(OPEN_MARK), OPEN_MARK) == 0) {
        buf[k - strlen(OPEN_MARK)] = 0;
        strncpy(name, buf, len);
        name[len] = 0;
        return false;
    }
    strncpy(name, buf, len);
    name[len] = 0;
    return true;
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

static void update_buttons(void)
{
    short row;
    char name[64];
    set_item_enabled(kPickOpen, selected(&row) && row_name(row, name, sizeof(name)));
}

static void fill_list(void)
{
    Proto *p = session_proto();
    short i, n = (short)proto_nprojects(p), avail = 0;
    char buf[80];
    Cell c;

    LSetDrawingMode(false, gList);
    LDelRow(0, 0, gList);
    if (n > 0)
        LAddRow(n, 0, gList);
    for (i = 0; i < n; i++) {
        const char *name = proto_project(p, i);
        strncpy(buf, name, 63);
        buf[63] = 0;
        if (tabs_by_project(name))
            strcat(buf, OPEN_MARK);
        else
            avail++;
        c.h = 0;
        c.v = i;
        LSetCell(buf, (short)strlen(buf), c, gList);
    }
    LSetDrawingMode(true, gList);
    InvalRect(&gListRect);
    set_item_enabled(kPickNewChat, tabs_count() < MAX_TABS);
    if (n == 0 || avail == 0)
        set_message(gChatOk ? "No projects to open. You can start a chat."
                            : "No projects available. Add folders with --projects on the host.");
    else if (tabs_count() >= MAX_TABS)
        set_message("Six tabs are open. Close one first.");
    else
        set_message(gChatOk ? "Open a project, or start a chat:" : "Open a project in a new tab:");
    update_buttons();
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

/* Selects the first row whose name starts with the typed letter. */
static void select_prefix(char ch)
{
    short i, rows = (*gList)->dataBounds.bottom;
    char name[64];
    Cell c;
    for (i = 0; i < rows; i++) {
        row_name(i, name, sizeof(name));
        if (name[0] == ch || (name[0] >= 'a' && name[0] - 32 == ch)
                || (name[0] >= 'A' && name[0] <= 'Z' && name[0] + 32 == ch)) {
            c.h = 0;
            c.v = 0;
            while (LGetSelect(true, &c, gList)) {
                LSetSelect(false, c, gList);
                c.v++;
            }
            c.v = i;
            LSetSelect(true, c, gList);
            LAutoScroll(gList);
            update_buttons();
            return;
        }
    }
}

static void move_selection(short delta)
{
    short row = -1, rows = (*gList)->dataBounds.bottom;
    Cell c;
    if (rows == 0)
        return;
    if (selected(&row)) {
        c.h = 0;
        c.v = row;
        LSetSelect(false, c, gList);
    }
    row = row < 0 ? 0 : row + delta;
    if (row < 0)
        row = 0;
    if (row >= rows)
        row = (short)(rows - 1);
    c.h = 0;
    c.v = row;
    LSetSelect(true, c, gList);
    LAutoScroll(gList);
    update_buttons();
}

static pascal Boolean filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    Point p;
    short row;
    char name[64];

    session_pump();
    SetPort(d);
    if (gStale) {
        gStale = false;
        fill_list();
    }
    if (session_link_state() != ST_ONLINE) {
        *item = kPickCancel;      /* the link went away */
        return true;
    }
    switch (ev->what) {
    case keyDown:
    case autoKey:
        ch = (char)(ev->message & charCodeMask);
        if ((ev->modifiers & cmdKey) && ch == '.') {
            *item = kPickCancel;
            return true;
        }
        if (ch == 0x1B) {
            *item = kPickCancel;
            return true;
        }
        if (ch == '\r' || ch == 3) {
            if (selected(&row) && row_name(row, name, sizeof(name))) {
                *item = kPickOpen;
                return true;
            }
            ev->what = nullEvent;
            return false;
        }
        if (ch == 0x1E)
            move_selection(-1);
        else if (ch == 0x1F)
            move_selection(1);
        else if (ch > ' ')
            select_prefix(ch);
        ev->what = nullEvent;
        return false;
    case mouseDown:
        p = ev->where;
        GlobalToLocal(&p);
        if (PtInRect(p, &(*gList)->rView) || PtInRect(p, &gListRect)) {
            Boolean dbl = LClick(p, ev->modifiers, gList);
            update_buttons();
            if (dbl && selected(&row) && row_name(row, name, sizeof(name))) {
                *item = kPickOpen;
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

PickResult picker_run(char *name, short max, Boolean chat_ok)
{
    short hit = 0, type, row;
    Handle h;
    Rect bounds = { 0, 0, 0, 1 }, view;
    Point cell = { 0, 0 };
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP;
    GrafPtr save;
    PickResult result = PICK_NONE;

    GetPort(&save);
    gDlg = GetNewDialog(kPickerDialog, NULL, (WindowPtr)-1);
    if (!gDlg)
        return PICK_NONE;
    SetPort(gDlg);
    TextFont(0);
    TextSize(0);

    GetDialogItem(gDlg, kPickList, &type, &h, &gListRect);
    drawUPP = NewUserItemUPP(draw_list);
    SetDialogItem(gDlg, kPickList, type, (Handle)drawUPP, &gListRect);
    view = gListRect;
    view.right -= 15;
    gList = LNew(&view, &bounds, cell, 0, gDlg, true, false, false, true);
    if (!gList) {
        DisposeDialog(gDlg);
        DisposeUserItemUPP(drawUPP);
        SetPort(save);
        return PICK_NONE;
    }
    (*gList)->selFlags = lOnlyOne;
    gStale = false;
    gChatOk = chat_ok;
    if (!chat_ok) {
        HideDialogItem(gDlg, kPickNewChat);
    }
    fill_list();
    proto_list_projects(session_proto());   /* fresh folders appear without redialing */
    SetDialogDefaultItem(gDlg, kPickOpen);
    SetDialogCancelItem(gDlg, kPickCancel);
    ShowWindow(gDlg);
    filterUPP = NewModalFilterUPP(filter);
    for (;;) {
        ModalDialog(filterUPP, &hit);
        if (hit == kPickCancel)
            break;
        if (hit == kPickOpen && selected(&row) && row_name(row, name, max)
                && tabs_count() < MAX_TABS) {
            result = PICK_PROJECT;
            break;
        }
        if (hit == kPickNewChat && chat_ok && tabs_count() < MAX_TABS) {
            result = PICK_CHAT;
            break;
        }
    }
    LDispose(gList);
    gList = NULL;
    DisposeDialog(gDlg);
    gDlg = NULL;
    DisposeModalFilterUPP(filterUPP);
    DisposeUserItemUPP(drawUPP);
    SetPort(save);
    return result;
}
