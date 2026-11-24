/* Claude30: a native System 7 client for se30bridge.
   Toolbox managers used across the app: QuickDraw, Font Manager, Window
   Manager, Menu Manager, TextEdit, Dialog Manager, Control Manager, Event
   Manager, Device Manager (Serial Driver), File Manager, Resource Manager,
   Folder Manager, Process Manager, Scrap Manager, Memory Manager. No Color
   QuickDraw, no floating point, no TCP/IP. */
#include "linktest.h"
#include "log.h"
#include "modem.h"
#include "prefs.h"
#include "resources.h"
#include "serial.h"
#include "window.h"
#include "toolbox.h"
#include <Devices.h>
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <ToolUtils.h>
#include <Windows.h>

#define QUIT_WAIT_TICKS (20 * 60)

/* osEvt message fields (Inside Macintosh: Processes); not in every header set. */
#define kSuspendResumeMessage 0x01
#define kResumeFlag           0x01

static Boolean gQuit;
static MenuHandle gFile, gEdit, gSession;

static void init_toolbox(void)
{
    MaxApplZone();
    MoreMasters();
    MoreMasters();
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
}

static void init_menus(void)
{
    Handle bar = GetNewMBar(kMenuBar);
    SetMenuBar(bar);
    AppendResMenu(GetMenuHandle(kAppleMenu), 'DRVR');
    gFile = GetMenuHandle(kFileMenu);
    gEdit = GetMenuHandle(kEditMenu);
    gSession = GetMenuHandle(kSessionMenu);
    DrawMenuBar();
}

static void enable(MenuHandle m, short item, Boolean on)
{
    if (on)
        EnableItem(m, item);
    else
        DisableItem(m, item);
}

static void adjust_menus(void)
{
    SessState s = session_state();
    Boolean front = FrontWindow() == window_ptr();
    enable(gSession, kConnectItem, s == ST_IDLE);
    enable(gSession, kDisconnectItem, s != ST_IDLE && s != ST_HANGUP);
    enable(gSession, kStopItem, s == ST_BUSY);
    enable(gSession, kLinkTestItem, s == ST_IDLE);
    enable(gEdit, kUndoItem, !front);
    enable(gEdit, kCutItem, true);
    enable(gEdit, kCopyItem, true);
    enable(gEdit, kPasteItem, true);
    enable(gEdit, kClearItem, true);
}

static void quit_app(void)
{
    unsigned long until;
    EventRecord ev;
    if (session_online()) {
        session_disconnect();
        until = TickCount() + QUIT_WAIT_TICKS;
        while (session_online() && TickCount() < until) {
            session_pump();
            WaitNextEvent(updateMask, &ev, 1, NULL);
            if (ev.what == updateEvt && (WindowPtr)ev.message == window_ptr())
                window_update();
        }
    }
    serial_close();
    log_close();
    gQuit = true;
}

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);
    Str255 name;
    switch (menu) {
    case kAppleMenu:
        if (item == kAboutItem) {
            Alert(kAboutAlert, NULL);
        } else {
            GetMenuItemText(GetMenuHandle(kAppleMenu), item, name);
            OpenDeskAcc(name);
        }
        break;
    case kFileMenu:
        if (item == kQuitItem)
            quit_app();
        break;
    case kEditMenu:
        if (!SystemEdit(item - 1)) {
            if (item == kSettingsItem)
                prefs_dialog();
            else
                window_edit(item);
        }
        break;
    case kSessionMenu:
        switch (item) {
        case kConnectItem:    session_connect(); break;
        case kDisconnectItem: session_disconnect(); break;
        case kStopItem:       session_stop(); break;
        case kLinkTestItem:   linktest_run(); break;
        }
        break;
    }
    HiliteMenu(0);
}

static void do_mouse(EventRecord *ev)
{
    WindowPtr w;
    short part = FindWindow(ev->where, &w);
    switch (part) {
    case inMenuBar:
        adjust_menus();
        do_menu(MenuSelect(ev->where));
        break;
    case inSysWindow:
        SystemClick(ev, w);
        break;
    case inDrag:
        DragWindow(w, ev->where, &qd.screenBits.bounds);
        break;
    case inContent:
        if (w != FrontWindow())
            SelectWindow(w);
        else if (w == window_ptr() && window_click(ev))
            session_send_input();
        break;
    }
}

static void do_key(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    if (ev->modifiers & cmdKey) {
        if (ch == '.') {
            session_stop();
            return;
        }
        if (ev->what == keyDown) {
            adjust_menus();
            do_menu(MenuKey(ch));
        }
        return;
    }
    if (FrontWindow() == window_ptr() && window_key(ev) == kKeySend)
        session_send_input();
}

int main(void)
{
    EventRecord ev;
    init_toolbox();
    init_menus();
    prefs_load();
    session_init();
    window_create();

    while (!gQuit) {
        session_pump();
        session_deferred();
        window_idle();
        if (!WaitNextEvent(everyEvent, &ev, session_online() ? 1 : 15, NULL))
            continue;
        switch (ev.what) {
        case mouseDown:
            do_mouse(&ev);
            break;
        case keyDown:
        case autoKey:
            do_key(&ev);
            break;
        case updateEvt:
            if ((WindowPtr)ev.message == window_ptr())
                window_update();
            break;
        case activateEvt:
            if ((WindowPtr)ev.message == window_ptr())
                window_activate((ev.modifiers & activeFlag) != 0);
            break;
        case osEvt:
            if (((ev.message >> 24) & 0xFF) == kSuspendResumeMessage) {
                window_activate((ev.message & kResumeFlag) != 0);
            }
            break;
        case kHighLevelEvent:
            break;
        }
    }
    return 0;
}
