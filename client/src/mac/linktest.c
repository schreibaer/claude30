#include "linktest.h"
#include "modem.h"
#include "prefs.h"
#include "resources.h"
#include "serial.h"
#include "window.h"
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Asks how long to run; returns minutes, or 0 if cancelled. */
static long ask_minutes(void)
{
    DialogPtr d = GetNewDialog(kLinkMinutesDialog, NULL, (WindowPtr)-1);
    short hit = 0, type;
    Handle h;
    Rect r;
    Str255 s;
    long minutes = 0;

    if (!d)
        return 0;
    GetDialogItem(d, kMinField, &type, &h, &r);
    SetDialogItemText(h, "\p60");
    SelectDialogItemText(d, kMinField, 0, 32767);
    SetDialogDefaultItem(d, kMinStart);
    SetDialogCancelItem(d, kMinCancel);
    ShowWindow(d);
    do {
        ModalDialog(NULL, &hit);
    } while (hit != kMinStart && hit != kMinCancel);
    if (hit == kMinStart) {
        GetDialogItemText(h, s);
        StringToNum(s, &minutes);
        if (minutes < 1)
            minutes = 1;
        if (minutes > 24 * 60)
            minutes = 24 * 60;
    }
    DisposeDialog(d);
    return minutes;
}

static void format_stats(char *out, short max, Boolean done)
{
    LinkStats *st = session_linkstats();
    unsigned long secs = (TickCount() - st->started) / 60;
    const char *state;
    if (done)
        state = st->failed ? st->failure : (st->errors ? "Finished with serial errors." : "Finished: no serial errors.");
    else
        state = session_link_state() == ST_DIALING || session_link_state() == ST_HANDSHAKE
              ? "Connecting\xC9" : "Running. Cmd-period or Stop ends the test.";
    snprintf(out, max,
             "Speed: %s on the %s port\r"
             "Elapsed: %lu:%02lu\r"
             "Bytes received: %ld\r"
             "Lines received: %ld\r"
             "Turns: %ld\r"
             "Serial errors: %ld\r\r%s",
             serial_speed_name(gPrefs.speed), gPrefs.port == kPortPrinter ? "printer" : "modem",
             secs / 60, secs % 60, st->bytes, st->lines, st->turns, st->errors, state);
}

static void draw_stats(DialogPtr d, Boolean done)
{
    char text[400];
    short type;
    Handle h;
    Rect r;
    SetPort(d);
    GetDialogItem(d, kLinkInfo, &type, &h, &r);
    format_stats(text, sizeof(text), done);
    TextFont(3);
    TextSize(9);
    EraseRect(&r);
    TETextBox(text, (long)strlen(text), &r, teFlushLeft);
    TextFont(0);
    TextSize(0);
}

void linktest_run(void)
{
    long minutes;
    DialogPtr d;
    EventRecord ev;
    DialogPtr which;
    short item, type;
    Handle h;
    Rect r;
    unsigned long lastDraw = 0;
    Boolean done = false, quit = false;
    char summary[160];

    if (session_online())
        return;
    minutes = ask_minutes();
    if (minutes <= 0)
        return;
    d = GetNewDialog(kLinkTestDialog, NULL, (WindowPtr)-1);
    if (!d)
        return;
    ShowWindow(d);
    session_linktest_start(minutes);

    while (!quit) {
        session_pump();
        if (!done && !session_online()) {
            done = true;
            GetDialogItem(d, kLinkStop, &type, &h, &r);
            SetControlTitle((ControlHandle)h, "\pClose");
            draw_stats(d, true);
        }
        if (!done && TickCount() - lastDraw >= 30) {
            lastDraw = TickCount();
            draw_stats(d, false);
        }
        if (!WaitNextEvent(everyEvent, &ev, 1, NULL))
            continue;
        if (ev.what == keyDown && (ev.modifiers & cmdKey)
                && (char)(ev.message & charCodeMask) == '.') {
            if (done)
                quit = true;
            else
                session_linktest_stop();
            continue;
        }
        if (ev.what == updateEvt && (WindowPtr)ev.message != d) {
            app_update((WindowPtr)ev.message);
            continue;
        }
        if (ev.what == updateEvt && (WindowPtr)ev.message == d) {
            BeginUpdate(d);
            DrawDialog(d);
            draw_stats(d, done);
            EndUpdate(d);
            continue;
        }
        if (IsDialogEvent(&ev) && DialogSelect(&ev, &which, &item) && item == kLinkStop) {
            if (done)
                quit = true;
            else
                session_linktest_stop();
        }
    }
    {
        LinkStats *st = session_linkstats();
        unsigned long secs = (TickCount() - st->started) / 60;
        snprintf(summary, sizeof(summary),
                 "Link test at %s: %lu min, %ld bytes, %ld lines, %ld turns, %ld serial errors%s",
                 serial_speed_name(gPrefs.speed), secs / 60, st->bytes, st->lines, st->turns,
                 st->errors, st->failed ? " (ended early)" : "");
    }
    DisposeDialog(d);
    session_note(summary);
}
