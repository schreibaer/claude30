#include "finddlg.h"
#include "modem.h"
#include "resources.h"
#include "window.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Events.h>
#include <Quickdraw.h>
#include <TextUtils.h>
#include <string.h>

static pascal Boolean filter(DialogPtr d, EventRecord *ev, short *item)
{
    char ch;
    session_pump();
    SetPort(d);
    switch (ev->what) {
    case keyDown:
    case autoKey:
        ch = (char)(ev->message & charCodeMask);
        if (((ev->modifiers & cmdKey) && ch == '.') || ch == 0x1B) {
            *item = kFindCancel;
            return true;
        }
        if (ch == '\r' || ch == 3) {
            *item = kFindOK;
            return true;
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

Boolean finddlg_run(char *text, short max)
{
    DialogPtr d;
    short hit = 0, type, n;
    Handle h;
    Rect r;
    Str255 p;
    ModalFilterUPP filterUPP;
    GrafPtr save;
    Boolean ok = false;

    GetPort(&save);
    d = GetNewDialog(kFindDialog, NULL, (WindowPtr)-1);
    if (!d)
        return false;
    SetPort(d);
    TextFont(0);
    TextSize(0);
    n = (short)strlen(text);
    if (n > 60)
        n = 60;
    p[0] = (unsigned char)n;
    memcpy(p + 1, text, n);
    GetDialogItem(d, kFindText, &type, &h, &r);
    SetDialogItemText(h, p);
    SelectDialogItemText(d, kFindText, 0, 32767);
    SetDialogDefaultItem(d, kFindOK);
    SetDialogCancelItem(d, kFindCancel);
    ShowWindow(d);
    filterUPP = NewModalFilterUPP(filter);
    do
        ModalDialog(filterUPP, &hit);
    while (hit != kFindOK && hit != kFindCancel);
    if (hit == kFindOK) {
        GetDialogItemText(h, p);
        if (p[0] > 0 && p[0] < max) {
            memcpy(text, p + 1, p[0]);
            text[p[0]] = 0;
            ok = true;
        }
    }
    DisposeDialog(d);
    DisposeModalFilterUPP(filterUPP);
    SetPort(save);
    return ok;
}
