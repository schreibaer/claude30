/* The conversation window: a strip of tabs across the top, then the front
   tab's styled TextEdit transcript with a scroll bar, its multi-line input
   area with a Send button, and a one-line status area. Every tab owns its
   own transcript and input records; background records keep receiving and
   trimming text with drawing clipped away, and switching tabs swaps which
   records own the transcript and input areas.
   Toolbox: Window Manager, TextEdit (TEStyleNew, TEStyleInsert, TEDelete,
   TEGetHeight, TEClick, TEKey, TEIdle, TECalText), Control Manager (scroll
   bar and button), QuickDraw (GetFontInfo, DrawText, ClipRect, InvertRect),
   Text Utilities (TruncText), Scrap Manager.
   Full screen moves and sizes the window (MoveWindow, SizeWindow) and hides
   the menu bar through the MBarHeight and GrayRgn low-memory globals, the
   usual System 7 technique; it needs no newer System. */
#ifndef WINDOW_H
#define WINDOW_H

#include <MacTypes.h>
#include <Events.h>
#include <Windows.h>
#include "tabs.h"

enum { kKeyNone, kKeySend, kKeyHandled };
enum { kClickNone, kClickSend, kClickTab };

void window_create(void);
/* Full screen: the window covers the whole screen and the menu bar is
   hidden (menus stay reachable by Command keys). Leaving it, or quitting or
   switching to another application, puts both back. */
void window_set_full_screen(Boolean on);
Boolean window_full_screen(void);
WindowPtr window_ptr(void);
void window_update(void);
void window_activate(Boolean active);
/* Draws an update for any of the app's windows; false if it is not ours. */
Boolean app_update(WindowPtr w);

/* Creates a tab's transcript and input records. False if memory ran out. */
Boolean window_tab_init(Tab *t);
void window_tab_free(Tab *t);
/* Shows tab t (NULL: no tab; the transcript area says how to open one). */
void window_show_tab(Tab *t);
/* Redraws the tab strip after labels or marks changed. */
void window_draw_tabs(void);

/* A mouse-down in the window's content: kClickSend when Send was clicked,
   kClickTab with *tab set when a tab label was clicked. */
short window_click(EventRecord *ev, Tab **tab);
/* A key for the window. Returns kKeySend when the user asked to send. */
short window_key(EventRecord *ev);
void window_idle(void);

/* Appends the tab's batch to its transcript, trimming old text to its log. */
void window_append(Tab *t);
/* True when the tab's transcript is empty (nothing appended yet). */
Boolean window_transcript_empty(Tab *t);

void window_set_status(const char *s);
void window_set_send_enabled(Boolean on);

/* The front tab's input text; valid until the next call that changes it. */
Handle window_input_text(long *len);
void window_clear_input(void);

/* Edit menu commands for the front tab. */
void window_edit(short item);

#endif
