/* Session glue: owns the protocol core and the serial port, routes core
   events by session id to their tabs (transcript batches, permission
   requests, marks), to the Open dialog and to file windows, and runs the
   dialogs and alerts the core asked for from the main loop.
   Toolbox: Event Manager (TickCount), Dialog Manager (alerts), plus the
   serial, window, tabs, permission dialog, picker, file window and log
   modules. */
#ifndef MODEM_H
#define MODEM_H

#include <MacTypes.h>
#include "protocol.h"
#include "tabs.h"

typedef struct {
    Boolean active;
    unsigned long started, ends;   /* ticks */
    long bytes, lines, turns, errors;
    Boolean failed;
    char failure[80];
} LinkStats;

void session_init(void);
void session_connect(void);
/* Disconnect: dirty file windows are settled first (the caller asks). */
void session_disconnect(void);
/* Reads the port, runs the core and draws. Called on every event-loop pass
   and from inside modal dialogs. */
void session_pump(void);
/* Dialogs and alerts the core asked for; called from the main loop only. */
void session_deferred(void);

Proto *session_proto(void);
LinkState session_link_state(void);
Boolean session_online(void);    /* a call is up or being set up */
/* The tab's session takes prompts and file requests. */
Boolean session_tab_ready(const Tab *t);
/* Its turn is running or waiting for permission. */
Boolean session_tab_busy(const Tab *t);
/* The tab waits for a permission answer. */
Boolean session_tab_waiting(const Tab *t);

/* Sends the front tab's input as a prompt. Returns true if it was sent. */
Boolean session_send_input(void);
/* Cmd-period: stops the front tab's turn. */
void session_stop(void);
/* Adds an information line to the front tab. */
void session_note(const char *text);

/* New Tab: runs the project picker and opens the chosen project. */
void session_new_tab(void);
/* Close Tab: confirms a busy tab, settles its file windows, then ends its
   conversation. Returns false if the person cancelled. */
Boolean session_close_tab(Tab *t);
void session_select_tab(Tab *t);
/* The project of the first tab waiting for a permission answer, or NULL. */
const char *session_waiting_project(void);

/* Link test: dial, open a session in the first project, then send a fixed
   prompt to the demo bridge repeatedly, counting bytes, lines and serial
   errors. */
void session_linktest_start(long minutes);
void session_linktest_stop(void);
LinkStats *session_linkstats(void);

#endif
