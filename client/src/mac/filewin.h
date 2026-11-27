/* File windows: one document window per open project file, owned by a tab.
   Editable files (up to 28 KB and 2,000 lines) load in checked pieces and
   save back in checked blocks; larger or unconvertible files open read-only
   and page through a sliding slice of lines as the person scrolls. A status
   strip at the bottom says what the window is doing. Windows of a tab hide
   while another tab is in front.
   Toolbox: Window Manager (NewWindow, GrowWindow, ZoomWindow, TrackBox,
   SetWTitle, HideWindow, ShowWindow), TextEdit (TENew, TESetText, TEKey,
   TECut/TECopy/TEPaste via the scrap, TEScroll), Control Manager (scroll
   bar), Dialog Manager (alerts), QuickDraw. */
#ifndef FILEWIN_H
#define FILEWIN_H

#include <MacTypes.h>
#include <Events.h>
#include <Windows.h>
#include "protocol.h"
#include "tabs.h"

#define MAX_FILEWINS 6

/* Opens path of tab t in a new window, or brings its window forward. */
void filewin_open(Tab *t, const char *path);

Boolean filewin_is(WindowPtr w);
Boolean filewin_update(WindowPtr w);
void filewin_activate(WindowPtr w, Boolean on);
void filewin_click(WindowPtr w, EventRecord *ev);
void filewin_key(WindowPtr w, EventRecord *ev);
void filewin_grow(WindowPtr w, Point where);
void filewin_zoom(WindowPtr w, short part, Point where);
void filewin_idle(void);

/* Cmd-W: asks about unsaved edits. False if the person cancelled. */
Boolean filewin_close(WindowPtr w);
void filewin_edit(WindowPtr w, short item);
void filewin_save(WindowPtr w);
void filewin_revert(WindowPtr w);
Boolean filewin_can_save(WindowPtr w);
Boolean filewin_can_revert(WindowPtr w);
Boolean filewin_editable(WindowPtr w);
short filewin_sid(WindowPtr w);

/* Asks Save, Don't Save or Cancel for each dirty window of session sid
   (0: every window) and runs the saves. With closing set the windows close
   afterwards. False if the person cancelled. */
Boolean filewin_settle(short sid, Boolean closing);
/* Shows the windows of session sid and hides the others. */
void filewin_show_tab(short sid);

/* Glue from modem.c. */
void filewin_event(short sid, EventKind k, const char *text, long len);
void filewin_turn_ended(short sid);
/* The session reopened (on) or the link to it is gone (off; sid 0: all). */
void filewin_online(short sid, Boolean on);
void filewin_deferred(void);

/* Find in the window's text: dir > 0 searches down from the caret, dir < 0 up
   from it. True when a match is selected. filewin_find_partial: the window
   holds only some lines of a read-only file, so a miss says so. */
Boolean filewin_find(WindowPtr w, const char *pat, short dir);
Boolean filewin_find_partial(WindowPtr w);

/* For the Window menu. */
short filewin_count(void);
WindowPtr filewin_at(short i);

#endif
