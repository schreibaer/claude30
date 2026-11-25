/* Claude30: a native System 7 client for se30bridge.
   Toolbox managers used across the app: QuickDraw, Font Manager, Window
   Manager, Menu Manager, TextEdit, Dialog Manager, Control Manager, List
   Manager, Event Manager, Device Manager (Serial Driver), File Manager,
   Resource Manager, Folder Manager, Process Manager, Scrap Manager, Memory
   Manager, Text Utilities (TruncString). No Color QuickDraw, no floating
   point, no TCP/IP. */
#include "filewin.h"
#include "files.h"
#include "linktest.h"
#include "log.h"
#include "modem.h"
#include "opendlg.h"
#include "prefs.h"
#include "resources.h"
#include "serial.h"
#include "tabs.h"
#include "window.h"
#include "toolbox.h"
#include <Devices.h>
#include <Dialogs.h>
#include <Events.h>
#include <Fonts.h>
#include <Menus.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <Windows.h>
#include <string.h>

#define QUIT_WAIT_TICKS (20 * 60)

/* osEvt message fields (Inside Macintosh: Processes); not in every header set. */
#define kSuspendResumeMessage 0x01
#define kResumeFlag           0x01

static Boolean gQuit;
static MenuHandle gFile, gEdit, gSession, gWindow;
static Boolean gOpenShown = true;    /* File > Open Project File is in the menu */

static void init_toolbox(void)
{
    MaxApplZone();
    MoreMasters();
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
    gWindow = GetMenuHandle(kWindowMenu);
    DrawMenuBar();
}

static void enable(MenuHandle m, short item, Boolean on)
{
    if (on)
        EnableItem(m, item);
    else
        DisableItem(m, item);
}

/* File menu item numbers move up by one while Open Project File is hidden. */
static short file_item(short logical)
{
    return gOpenShown || logical == kOpenFileItem ? logical : (short)(logical - 1);
}

static short file_logical(short item)
{
    return gOpenShown ? item : (short)(item + 1);
}

/* Hides File > Open Project File under files=off (R13). */
static void show_open_item(Boolean show)
{
    if (show == gOpenShown)
        return;
    if (show)
        InsertMenuItem(gFile, "\pOpen Project File\xC9/O", 0);
    else
        DeleteMenuItem(gFile, kOpenFileItem);
    gOpenShown = show;
}

/* The Window menu: the tabs (Cmd-1 to Cmd-6), then the file windows. */
static void build_window_menu(void)
{
    short i, n = CountMItems(gWindow), k;
    Str255 s;
    Tab *t;
    WindowPtr w, front = FrontWindow();
    for (i = n; i >= 1; i--)
        DeleteMenuItem(gWindow, i);
    for (i = 0; i < tabs_count(); i++) {
        t = tabs_at(i);
        k = (short)strlen(t->project);
        s[0] = (unsigned char)k;
        memcpy(s + 1, t->project, k);
        AppendMenu(gWindow, "\px");          /* text set below: no metacharacters */
        SetMenuItemText(gWindow, (short)(i + 1), s);
        SetItemCmd(gWindow, (short)(i + 1), (char)('1' + i));
        CheckItem(gWindow, (short)(i + 1), t == tabs_front() && front == window_ptr());
    }
    if (filewin_count() > 0) {
        if (tabs_count() > 0)
            AppendMenu(gWindow, "\p(-");
        for (i = 0; i < filewin_count(); i++) {
            w = filewin_at(i);
            GetWTitle(w, s);
            AppendMenu(gWindow, "\px");
            n = CountMItems(gWindow);
            SetMenuItemText(gWindow, n, s);
            CheckItem(gWindow, n, w == front);
        }
    }
    if (CountMItems(gWindow) == 0) {
        AppendMenu(gWindow, "\pNo Windows");
        DisableItem(gWindow, 1);
    }
}

static void adjust_menus(void)
{
    LinkState s = session_link_state();
    WindowPtr front = FrontWindow();
    Boolean fw = filewin_is(front);
    Tab *t = tabs_front();
    Proto *p = session_proto();
    Boolean online = s == ST_ONLINE;

    show_open_item(!online || proto_files_mode(p) != FILES_OFF);
    if (gOpenShown) {
        Tab *owner = fw ? tabs_by_sid(filewin_sid(front)) : t;
        enable(gFile, kOpenFileItem, online && session_tab_ready(owner));
    }
    enable(gFile, file_item(kCloseItem), fw || (front == window_ptr() && t != NULL));
    enable(gFile, file_item(kSaveItem), fw && filewin_can_save(front));
    enable(gFile, file_item(kRevertItem), fw && filewin_can_revert(front));
    enable(gFile, file_item(kFullScreenItem), filewin_count() == 0);
    CheckItem(gFile, file_item(kCompactItem), window_input_compact());
    enable(gFile, file_item(kQuitItem), true);

    enable(gSession, kConnectItem, s == ST_IDLE);
    enable(gSession, kDisconnectItem, (s != ST_IDLE && s != ST_HANGUP) || session_redial_pending());
    enable(gSession, kStopItem, t && session_tab_busy(t));
    enable(gSession, kPrevPromptItem, t != NULL && front == window_ptr());
    enable(gSession, kNextPromptItem, t != NULL && front == window_ptr());
    enable(gSession, kNewTabItem, online && tabs_count() < MAX_TABS);
    enable(gSession, kCloseTabItem, t != NULL);
    enable(gSession, kModelItem, session_model_available());
    enable(gSession, kLinkTestItem, s == ST_IDLE);

    enable(gEdit, kUndoItem, !fw && front != window_ptr());
    enable(gEdit, kCutItem, !fw || filewin_editable(front));
    enable(gEdit, kCopyItem, true);
    enable(gEdit, kPasteItem, !fw || filewin_editable(front));
    enable(gEdit, kClearItem, !fw || filewin_editable(front));
    build_window_menu();
}

static void pump_until_offline(void)
{
    unsigned long until = TickCount() + QUIT_WAIT_TICKS;
    EventRecord ev;
    while (session_online() && TickCount() < until) {
        session_pump();
        if (WaitNextEvent(updateMask, &ev, 1, NULL) && ev.what == updateEvt)
            app_update((WindowPtr)ev.message);
    }
}

static void quit_app(void)
{
    if (!filewin_settle(0, true))
        return;                      /* Cancel keeps the app running */
    window_set_full_screen(false);   /* the Finder needs its menu bar back */
    if (session_online()) {
        session_disconnect();
        pump_until_offline();
    }
    serial_close();
    {
        short i;
        for (i = 0; i < tabs_count(); i++) {
            window_append(tabs_at(i));
            log_close(&tabs_at(i)->log);
        }
    }
    gQuit = true;
}

static void disconnect(void)
{
    if (session_redial_pending()) {  /* nothing to save over a link that is down */
        session_disconnect();
        return;
    }
    if (!filewin_settle(0, false))   /* saves run before hanging up */
        return;
    session_disconnect();
}

static void open_project_file(void)
{
    WindowPtr front = FrontWindow();
    Tab *t = filewin_is(front) ? tabs_by_sid(filewin_sid(front)) : tabs_front();
    char path[256];
    if (!t || !session_tab_ready(t))
        return;
    if (opendlg_run(t, path, sizeof(path))) {
        session_select_tab(t);
        filewin_open(t, path);
    }
}

static void close_front(void)
{
    WindowPtr front = FrontWindow();
    if (filewin_is(front))
        filewin_close(front);
    else if (front == window_ptr())
        session_close_tab(tabs_front());
}

static void choose_window_item(short item)
{
    short n = tabs_count();
    if (item <= n) {
        session_select_tab(tabs_at((short)(item - 1)));
        return;
    }
    item -= n + (n > 0 ? 1 : 0);
    if (item >= 1 && item <= filewin_count()) {
        WindowPtr w = filewin_at((short)(item - 1));
        Tab *t = tabs_by_sid(filewin_sid(w));
        if (t && t != tabs_front())
            session_select_tab(t);
        SelectWindow(w);
    }
}

static void do_menu(long choice)
{
    short menu = HiWord(choice), item = LoWord(choice);
    WindowPtr front = FrontWindow();
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
        switch (file_logical(item)) {
        case kOpenFileItem: open_project_file(); break;
        case kCloseItem:    close_front(); break;
        case kSaveItem:     filewin_save(front); break;
        case kRevertItem:   filewin_revert(front); break;
        case kFullScreenItem:
            window_set_full_screen(!window_full_screen());
            CheckItem(gFile, file_item(kFullScreenItem), window_full_screen());
            break;
        case kCompactItem:  window_toggle_input(); break;
        case kQuitItem:     quit_app(); break;
        }
        break;
    case kEditMenu:
        if (!SystemEdit(item - 1)) {
            if (item == kSettingsItem) {
                if (prefs_dialog())
                    window_prefs_changed();
            } else if (filewin_is(front))
                filewin_edit(front, item);
            else
                window_edit(item);
        }
        break;
    case kSessionMenu:
        switch (item) {
        case kConnectItem:    session_connect(); break;
        case kDisconnectItem: disconnect(); break;
        case kStopItem:       session_stop(); break;
        case kPrevPromptItem: window_jump_prompt(-1); break;
        case kNextPromptItem: window_jump_prompt(1); break;
        case kNewTabItem:     session_new_tab(); break;
        case kCloseTabItem:   session_close_tab(tabs_front()); break;
        case kModelItem:      session_model_dialog(); break;
        case kLinkTestItem:   linktest_run(); break;
        }
        break;
    case kWindowMenu:
        choose_window_item(item);
        break;
    }
    HiliteMenu(0);
}

static void do_mouse(EventRecord *ev)
{
    WindowPtr w;
    Tab *tab;
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
    case inGrow:
        filewin_grow(w, ev->where);
        break;
    case inZoomIn:
    case inZoomOut:
        filewin_zoom(w, part, ev->where);
        break;
    case inGoAway:
        if (TrackGoAway(w, ev->where) && filewin_is(w))
            filewin_close(w);
        break;
    case inContent:
        if (w != FrontWindow()) {
            SelectWindow(w);
        } else if (w == window_ptr()) {
            switch (window_click(ev, &tab)) {
            case kClickSend: session_send_input(); break;
            case kClickTab:  session_select_tab(tab); break;
            }
        } else if (filewin_is(w)) {
            filewin_click(w, ev);
        }
        break;
    }
}

/* Virtual key code of the Delete (Backspace) key. */
#define kDeleteKeyCode 0x33

static void do_key(EventRecord *ev)
{
    char ch = (char)(ev->message & charCodeMask);
    WindowPtr front = FrontWindow();
    /* Transcript scroll keys, before the menus and the input box see them. */
    if (front == window_ptr() && window_scroll_key(ev))
        return;
    /* Cmd-Backspace empties the input box. Match the Delete key's code too,
       in case a keyboard layout maps it to another character under Cmd. */
    if ((ev->modifiers & cmdKey) && front == window_ptr()
        && (ch == '\b' || ((ev->message & keyCodeMask) >> 8) == kDeleteKeyCode)) {
        window_erase_input();
        return;
    }
    if (ev->modifiers & cmdKey) {
        if (ch == '.') {
            session_stop();          /* always the front tab, from any window */
            return;
        }
        if (ev->what == keyDown) {
            adjust_menus();
            do_menu(MenuKey(ch));
        }
        return;
    }
    if (front == window_ptr()) {
        if (window_key(ev) == kKeySend)
            session_send_input();
    } else if (filewin_is(front)) {
        filewin_key(front, ev);
    }
}

static void activate(WindowPtr w, Boolean on)
{
    if (w == window_ptr())
        window_activate(on);
    else
        filewin_activate(w, on);
}

int main(void)
{
    EventRecord ev;
    init_toolbox();
    init_menus();
    prefs_load();
    window_create();
    session_init();

    while (!gQuit) {
        session_pump();
        session_deferred();
        window_idle();
        filewin_idle();
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
            app_update((WindowPtr)ev.message);
            break;
        case activateEvt:
            activate((WindowPtr)ev.message, (ev.modifiers & activeFlag) != 0);
            break;
        case osEvt:
            if (((ev.message >> 24) & 0xFF) == kSuspendResumeMessage) {
                if (!(ev.message & kResumeFlag) && window_full_screen()) {
                    /* Other applications need the menu bar. */
                    window_set_full_screen(false);
                    CheckItem(gFile, file_item(kFullScreenItem), false);
                }
                activate(FrontWindow(), (ev.message & kResumeFlag) != 0);
            }
            break;
        case kHighLevelEvent:
            break;
        }
    }
    return 0;
}
