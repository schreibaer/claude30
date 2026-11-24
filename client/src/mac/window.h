/* The conversation window: a styled TextEdit transcript with a scroll bar,
   a multi-line input area with a Send button, and a one-line status area.
   Toolbox: Window Manager, TextEdit (TEStyleNew, TEStyleInsert, TEDelete,
   TEGetHeight, TEClick, TEKey, TEIdle, TECalText), Control Manager (scroll
   bar and button), QuickDraw (GetFontInfo, DrawText), Scrap Manager.
   Full screen moves and sizes the window (MoveWindow, SizeWindow) and hides
   the menu bar through the MBarHeight and GrayRgn low-memory globals, the
   usual System 7 technique; it needs no newer System. */
#ifndef WINDOW_H
#define WINDOW_H

#include <MacTypes.h>
#include <Events.h>
#include <Windows.h>
#include "batch.h"

enum { kKeyNone, kKeySend, kKeyHandled };

void window_create(void);
/* Full screen: the window covers the whole screen and the menu bar is
   hidden (menus stay reachable by Command keys). Leaving it, or quitting or
   switching to another application, puts both back. */
void window_set_full_screen(Boolean on);
Boolean window_full_screen(void);
WindowPtr window_ptr(void);
void window_update(void);
void window_activate(Boolean active);
/* A mouse-down in the window's content. Returns true if Send was clicked. */
Boolean window_click(EventRecord *ev);
/* A key for the window. Returns kKeySend when the user asked to send. */
short window_key(EventRecord *ev);
void window_idle(void);

/* Appends the batch's runs to the transcript, trimming old text to the log. */
void window_append(Batch *b);
/* Reports and clears trouble writing the log since the last call: returns
   the File Manager error (noErr if none) and sets *lost to the bytes that had
   to leave the window without reaching the log. */
OSErr window_log_trouble(long *lost);
/* True when the transcript is empty (nothing appended yet). */
Boolean window_transcript_empty(void);

void window_set_status(const char *s);
void window_set_send_enabled(Boolean on);

/* The input text; valid until the next call that changes the input. */
Handle window_input_text(long *len);
void window_clear_input(void);

/* Edit menu commands. */
void window_edit(short item);

#endif
