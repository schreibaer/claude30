#include "prefs.h"
#include "serial.h"
#include "resources.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Files.h>
#include <Resources.h>
#include <TextUtils.h>
#include <stddef.h>
#include <string.h>

Prefs gPrefs;

static const unsigned char kPrefsName[] = "\pClaude30 Prefs";

/* Version 1 ended after the dial string. */
#define PREFS_V1_SIZE (3 * sizeof(short) + sizeof(Str63))
#define PREFS_V2_SIZE ((long)offsetof(Prefs, compact_input))
#define PREFS_V3_SIZE ((long)offsetof(Prefs, hide_scrollbar))
#define PREFS_V4_SIZE ((long)offsetof(Prefs, tokens))
#define PREFS_V5_SIZE ((long)offsetof(Prefs, models))
#define PREFS_V6_SIZE ((long)offsetof(Prefs, streams))

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
    if (h && GetHandleSize(h) >= (long)PREFS_V1_SIZE && ((Prefs *)*h)->version >= 1) {
        short i;
        if (((Prefs *)*h)->version == PREFS_VERSION && GetHandleSize(h) == sizeof(Prefs)) {
            memcpy(&gPrefs, *h, sizeof(Prefs));
        } else if (((Prefs *)*h)->version == 6 && GetHandleSize(h) >= PREFS_V6_SIZE) {
            memcpy(&gPrefs, *h, PREFS_V6_SIZE);   /* version 6: streaming off */
        } else if (((Prefs *)*h)->version == 5 && GetHandleSize(h) >= PREFS_V5_SIZE) {
            memcpy(&gPrefs, *h, PREFS_V5_SIZE);   /* version 5: every tab at the bridge default */
        } else if (((Prefs *)*h)->version == 4 && GetHandleSize(h) >= PREFS_V4_SIZE) {
            memcpy(&gPrefs, *h, PREFS_V4_SIZE);   /* version 4: no resume tokens */
        } else if (((Prefs *)*h)->version == 3 && GetHandleSize(h) >= PREFS_V3_SIZE) {
            memcpy(&gPrefs, *h, PREFS_V3_SIZE);   /* version 3: scroll bar shown */
            gPrefs.hide_scrollbar = 0;
        } else if (((Prefs *)*h)->version == 2 && GetHandleSize(h) >= PREFS_V2_SIZE) {
            memcpy(&gPrefs, *h, PREFS_V2_SIZE);   /* version 2: input expanded */
            gPrefs.compact_input = 0;
        } else {
            memcpy(&gPrefs, *h, PREFS_V1_SIZE);   /* version 1: no tabs */
            gPrefs.ntabs = 0;
        }
        gPrefs.version = PREFS_VERSION;
        if (gPrefs.ntabs < 0 || gPrefs.ntabs > PREFS_TABS)
            gPrefs.ntabs = 0;
        for (i = 0; i < gPrefs.ntabs; i++)
            if (gPrefs.tabs[i][0] > 63)
                gPrefs.tabs[i][0] = 63;
        for (i = 0; i < PREFS_TABS; i++) {
            gPrefs.tokens[i][16] = 0;
            gPrefs.models[i][31] = 0;
            gPrefs.efforts[i][7] = 0;
            gPrefs.streams[i] = gPrefs.streams[i] != 0;
        }
        if (gPrefs.speed < kSpeed57600 || gPrefs.speed > kSpeed19200)
            gPrefs.speed = kSpeed57600;
        if (gPrefs.port != kPortPrinter)
            gPrefs.port = kPortModem;
        if (gPrefs.dial[0] > 63)
            gPrefs.dial[0] = 63;
        gPrefs.compact_input = gPrefs.compact_input != 0;
        gPrefs.hide_scrollbar = gPrefs.hide_scrollbar != 0;
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

void prefs_set_tabs(const char *const *names, const char *const *tokens,
                    const char *const *models, const char *const *efforts,
                    const char *streams, short n)
{
    Str63 tabs[PREFS_TABS];
    char toks[PREFS_TABS][17];
    char mods[PREFS_TABS][32];
    char effs[PREFS_TABS][8];
    char strs[PREFS_TABS];
    short i, k;
    if (n > PREFS_TABS)
        n = PREFS_TABS;
    memset(tabs, 0, sizeof(tabs));
    memset(toks, 0, sizeof(toks));
    memset(mods, 0, sizeof(mods));
    memset(effs, 0, sizeof(effs));
    memset(strs, 0, sizeof(strs));
    for (i = 0; i < n; i++) {
        k = (short)strlen(names[i]);
        if (k > 63)
            k = 63;
        tabs[i][0] = (unsigned char)k;
        memcpy(tabs[i] + 1, names[i], k);
        k = (short)strlen(tokens[i]);
        if (k > 16)
            k = 16;
        memcpy(toks[i], tokens[i], k);
        k = (short)strlen(models[i]);
        if (k > 31)
            k = 31;
        memcpy(mods[i], models[i], k);
        k = (short)strlen(efforts[i]);
        if (k > 7)
            k = 7;
        memcpy(effs[i], efforts[i], k);
        strs[i] = streams[i] != 0;
    }
    if (n == gPrefs.ntabs && memcmp(tabs, gPrefs.tabs, sizeof(tabs)) == 0
            && memcmp(toks, gPrefs.tokens, sizeof(toks)) == 0
            && memcmp(mods, gPrefs.models, sizeof(mods)) == 0
            && memcmp(effs, gPrefs.efforts, sizeof(effs)) == 0
            && memcmp(strs, gPrefs.streams, sizeof(strs)) == 0)
        return;                  /* unchanged: no disk write */
    memcpy(gPrefs.tabs, tabs, sizeof(tabs));
    memcpy(gPrefs.tokens, toks, sizeof(toks));
    memcpy(gPrefs.models, mods, sizeof(mods));
    memcpy(gPrefs.efforts, effs, sizeof(effs));
    memcpy(gPrefs.streams, strs, sizeof(strs));
    gPrefs.ntabs = n;
    prefs_save();
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

static void set_check(DialogPtr d, short item, Boolean on)
{
    short type;
    Handle h;
    Rect r;
    GetDialogItem(d, item, &type, &h, &r);
    SetControlValue((ControlHandle)h, on);
}

Boolean prefs_dialog(void)
{
    DialogPtr d = GetNewDialog(kSettingsDialog, NULL, (WindowPtr)-1);
    short hit = 0, type, port = gPrefs.port, speed = gPrefs.speed;
    short hide = gPrefs.hide_scrollbar;
    Handle h;
    Rect r;
    Str255 s;

    if (!d)
        return false;
    set_radio(d, kSetModem, kSetPrinter, kSetModem + port);
    set_radio(d, kSet57600, kSet19200, kSet57600 + speed);
    set_check(d, kSetHideBar, hide != 0);
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
        } else if (hit == kSetHideBar) {
            hide = !hide;
            set_check(d, kSetHideBar, hide != 0);
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
        gPrefs.hide_scrollbar = hide;
        prefs_save();
    }
    DisposeDialog(d);
    return hit == kSetOK;
}
