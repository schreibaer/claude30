#include "modeldlg.h"
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

#define NEFFORTS 6
/* The wire tokens, in the order of the radio buttons; "" is the default. */
static const char *const kEfforts[NEFFORTS] = { "", "low", "medium", "high", "xhigh", "max" };

static DialogPtr gDlg;
static ListHandle gList;
static Rect gListRect;
static short gEffort;            /* the radio button that is on, 0 to 5 */

static void set_text(short item, const char *s)
{
    short type, n = (short)strlen(s);
    Handle h;
    Rect r;
    Str255 p;
    if (n > 250)
        n = 250;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
    GetDialogItem(gDlg, item, &type, &h, &r);
    SetDialogItemText(h, p);
}

static void show_effort(short which)
{
    short i, type;
    Handle h;
    Rect r;
    gEffort = which;
    for (i = 0; i < NEFFORTS; i++) {
        GetDialogItem(gDlg, (short)(kModelEffort0 + i), &type, &h, &r);
        SetControlValue((ControlHandle)h, i == which);
    }
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

static void move_selection(short delta)
{
    short row = 0, rows = (*gList)->dataBounds.bottom;
    if (selected(&row))
        row += delta;
    if (row < 0)
        row = 0;
    if (row >= rows)
        row = (short)(rows - 1);
    select_row(row);
}

/* Row 0 is the bridge default, named when the bridge was started with one;
   the rest are the bridge's models. A current model the bridge no longer
   offers is not selectable: it shows as the default. Returns the row to
   select. */
static short fill_list(const char *model)
{
    Proto *p = session_proto();
    short i, n = (short)proto_nmodels(p), pick = 0;
    char buf[64];
    Cell c;

    LAddRow(n + 1, 0, gList);
    if (proto_default_model(p)[0])
        snprintf(buf, sizeof(buf), "Bridge default (%s)", proto_default_model(p));
    else
        snprintf(buf, sizeof(buf), "Bridge default");
    c.h = 0;
    c.v = 0;
    LSetCell(buf, (short)strlen(buf), c, gList);
    for (i = 0; i < n; i++) {
        c.v = (short)(i + 1);
        snprintf(buf, sizeof(buf), "%s", proto_model(p, i));
        LSetCell(buf, (short)strlen(buf), c, gList);
        if (model[0] && strcmp(model, proto_model(p, i)) == 0)
            pick = (short)(i + 1);
    }
    return pick;
}

static pascal void draw_list(DialogPtr d, short item)
{
    Rect r = gListRect;
    (void)item;
    InsetRect(&r, -1, -1);
    FrameRect(&r);
    r = (*gList)->rView;
    EraseRect(&r);
    LUpdate(d->visRgn, gList);
}

static pascal Boolean filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    Point p;

    session_pump();
    SetPort(d);
    if (session_link_state() != ST_ONLINE) {
        *item = kModelCancel;     /* the link went away */
        return true;
    }
    switch (ev->what) {
    case keyDown:
    case autoKey:
        ch = (char)(ev->message & charCodeMask);
        if (((ev->modifiers & cmdKey) && ch == '.') || ch == 0x1B) {
            *item = kModelCancel;
            return true;
        }
        if (ch == '\r' || ch == 3) {
            *item = kModelOK;
            return true;
        }
        if (ch == 0x1E)
            move_selection(-1);
        else if (ch == 0x1F)
            move_selection(1);
        ev->what = nullEvent;
        return false;
    case mouseDown:
        p = ev->where;
        GlobalToLocal(&p);
        if (PtInRect(p, &(*gList)->rView) || PtInRect(p, &gListRect)) {
            Boolean dbl = LClick(p, ev->modifiers, gList);
            if (dbl) {
                *item = kModelOK;
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

Boolean modeldlg_run(const char *project, const char *model, const char *effort,
                     char *outModel, char *outEffort)
{
    short hit = 0, type, row = 0, i, pick;
    Handle h;
    Rect bounds = { 0, 0, 0, 1 }, view;
    Point cell = { 0, 0 };
    ModalFilterUPP filterUPP;
    UserItemUPP drawUPP;
    GrafPtr save;
    Boolean ok = false;
    char title[96];

    GetPort(&save);
    gDlg = GetNewDialog(kModelDialog, NULL, (WindowPtr)-1);
    if (!gDlg)
        return false;
    SetPort(gDlg);
    TextFont(0);
    TextSize(0);

    GetDialogItem(gDlg, kModelList, &type, &h, &gListRect);
    drawUPP = NewUserItemUPP(draw_list);
    SetDialogItem(gDlg, kModelList, type, (Handle)drawUPP, &gListRect);
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
    pick = fill_list(model);
    select_row(pick);
    for (i = 0; i < NEFFORTS; i++)
        if (strcmp(effort, kEfforts[i]) == 0)
            row = i;
    show_effort(row);
    snprintf(title, sizeof(title), "Model and effort for %s:", project);
    set_text(kModelTitle, title);

    SetDialogDefaultItem(gDlg, kModelOK);
    SetDialogCancelItem(gDlg, kModelCancel);
    ShowWindow(gDlg);
    filterUPP = NewModalFilterUPP(filter);
    for (;;) {
        ModalDialog(filterUPP, &hit);
        if (hit == kModelCancel)
            break;
        if (hit >= kModelEffort0 && hit <= kModelEffort5) {
            show_effort((short)(hit - kModelEffort0));
        } else if (hit == kModelOK) {
            Proto *p = session_proto();
            short r = 0;
            selected(&r);
            outModel[0] = 0;
            if (r > 0)
                strncat(outModel, proto_model(p, (short)(r - 1)), 31);
            strcpy(outEffort, kEfforts[gEffort]);
            ok = true;
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
    return ok;
}
