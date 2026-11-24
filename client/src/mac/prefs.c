#include "prefs.h"
#include "serial.h"
#include "resources.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Files.h>
#include <Resources.h>
#include <TextUtils.h>
#include <string.h>

Prefs gPrefs;

static const unsigned char kPrefsName[] = "\pClaude30 Prefs";

/* Folder Manager selectors; Multiversal has FindFolder but not these names. */
#define kOnSystemDiskVol  ((short)0x8000)
#define kCreateFolderNow  true

static void prefs_defaults(void)
{
    memset(&gPrefs, 0, sizeof(gPrefs));
    gPrefs.version = PREFS_VERSION;
    gPrefs.port = kPortModem;
    gPrefs.speed = kSpeed57600;
    memcpy(gPrefs.dial, "\pATDTbridge", 11);
}

static OSErr prefs_spec(FSSpec *spec)
{
    short vRef;
    long dirID;
    OSErr err = FindFolder(kOnSystemDiskVol, kPreferencesFolderType, kCreateFolderNow, &vRef, &dirID);
    if (err)
        return err;
    err = FSMakeFSSpec(vRef, dirID, kPrefsName, spec);
    return err == fnfErr ? noErr : err;
}

void prefs_load(void)
{
    FSSpec spec;
    short ref, saved = CurResFile();
    Handle h;

    prefs_defaults();
    if (prefs_spec(&spec) != noErr)
        return;
    ref = FSpOpenResFile(&spec, fsRdPerm);
    if (ref == -1)
        return;
    h = Get1Resource(kPrefsType, kPrefsID);
    if (h && GetHandleSize(h) == sizeof(Prefs)
            && ((Prefs *)*h)->version == PREFS_VERSION) {
        memcpy(&gPrefs, *h, sizeof(Prefs));
        if (gPrefs.speed < kSpeed57600 || gPrefs.speed > kSpeed19200)
            gPrefs.speed = kSpeed57600;
        if (gPrefs.port != kPortPrinter)
            gPrefs.port = kPortModem;
        if (gPrefs.dial[0] > 63)
            gPrefs.dial[0] = 63;
    }
    CloseResFile(ref);
    UseResFile(saved);
}

void prefs_save(void)
{
    FSSpec spec;
    short ref, saved = CurResFile();
    Handle h;

    if (prefs_spec(&spec) != noErr)
        return;
    ref = FSpOpenResFile(&spec, fsRdWrPerm);
    if (ref == -1) {
        FSpCreateResFile(&spec, kCreator, 'pref', smSystemScript);
        ref = FSpOpenResFile(&spec, fsRdWrPerm);
        if (ref == -1)
            return;
    }
    h = Get1Resource(kPrefsType, kPrefsID);
    if (h) {
        SetHandleSize(h, sizeof(Prefs));
        if (MemError() == noErr) {
            memcpy(*h, &gPrefs, sizeof(Prefs));
            ChangedResource(h);
        }
    } else {
        h = NewHandle(sizeof(Prefs));
        if (h) {
            memcpy(*h, &gPrefs, sizeof(Prefs));
            AddResource(h, kPrefsType, kPrefsID, "\p");
        }
    }
    UpdateResFile(ref);
    CloseResFile(ref);
    UseResFile(saved);
}

void prefs_dial_cstr(char *out, short max)
{
    short n = gPrefs.dial[0];
    if (n > max - 1)
        n = max - 1;
    memcpy(out, gPrefs.dial + 1, n);
    out[n] = 0;
}

/* ---- Settings dialog ---------------------------------------------------- */

static void set_radio(DialogPtr d, short first, short last, short on)
{
    short i, type;
    Handle h;
    Rect r;
    for (i = first; i <= last; i++) {
        GetDialogItem(d, i, &type, &h, &r);
        SetControlValue((ControlHandle)h, i == on);
    }
}

Boolean prefs_dialog(void)
{
    DialogPtr d = GetNewDialog(kSettingsDialog, NULL, (WindowPtr)-1);
    short hit = 0, type, port = gPrefs.port, speed = gPrefs.speed;
    Handle h;
    Rect r;
    Str255 s;

    if (!d)
        return false;
    set_radio(d, kSetModem, kSetPrinter, kSetModem + port);
    set_radio(d, kSet57600, kSet19200, kSet57600 + speed);
    GetDialogItem(d, kSetDial, &type, &h, &r);
    SetDialogItemText(h, gPrefs.dial);
    SelectDialogItemText(d, kSetDial, 0, 32767);
    SetDialogDefaultItem(d, kSetOK);
    SetDialogCancelItem(d, kSetCancel);
    ShowWindow(d);

    for (;;) {
        ModalDialog(NULL, &hit);
        if (hit == kSetOK || hit == kSetCancel)
            break;
        if (hit >= kSetModem && hit <= kSetPrinter) {
            port = hit - kSetModem;
            set_radio(d, kSetModem, kSetPrinter, hit);
        } else if (hit >= kSet57600 && hit <= kSet19200) {
            speed = hit - kSet57600;
            set_radio(d, kSet57600, kSet19200, hit);
        }
    }
    if (hit == kSetOK) {
        GetDialogItem(d, kSetDial, &type, &h, &r);
        GetDialogItemText(h, s);
        if (s[0] > 63)
            s[0] = 63;
        if (s[0] > 0)
            memcpy(gPrefs.dial, s, s[0] + 1);
        gPrefs.port = port;
        gPrefs.speed = speed;
        prefs_save();
    }
    DisposeDialog(d);
    return hit == kSetOK;
}
