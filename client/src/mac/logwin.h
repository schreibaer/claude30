/* The Log window: a read-only window that shows about 20 lines of a tab's log
   file around a Find match. One is open at a time and it is reused. It
   belongs to the tab it searched, hides while another tab is in front and
   closes with its tab.
   Toolbox: File Manager (FSpOpenDF with read permission, SetFPos, FSRead,
   GetEOF), Window Manager, TextEdit (TENew, TESetText, TESetSelect, TEScroll). */
#ifndef LOGWIN_H
#define LOGWIN_H

#include <MacTypes.h>
#include <Events.h>
#include <Windows.h>
#include "tabs.h"

enum { LOG_FOUND, LOG_NONE, LOG_UNAVAILABLE, LOG_ERROR };

/* A new Find: the next log search starts from the end of the log. */
void logwin_reset(void);
/* Searches tab t's log toward older (dir < 0) or newer text from the last log
   match, or from the end of the log after a reset. Shows the match in the Log
   window. LOG_NONE: no further match. LOG_UNAVAILABLE: a chat, which keeps no log. */
short logwin_search(Tab *t, const char *pat, short dir);

WindowPtr logwin_ptr(void);              /* NULL when closed */
Boolean logwin_is(WindowPtr w);
Boolean logwin_update(WindowPtr w);
void logwin_activate(WindowPtr w, Boolean on);
void logwin_click(WindowPtr w, EventRecord *ev);
void logwin_key(WindowPtr w, EventRecord *ev);
void logwin_copy(void);
void logwin_close(void);
short logwin_sid(void);
/* Shows the Log window if it belongs to tab sid, hides it otherwise. */
void logwin_show_tab(short sid);
void logwin_tab_closed(short sid);

#endif
