/* Session glue: owns the protocol core, the serial port and the transcript
   batch, and turns core events into window updates, dialogs and alerts.
   Toolbox: Event Manager (TickCount), Dialog Manager (alerts), plus the
   serial, window, permission dialog and log modules. */
#ifndef MODEM_H
#define MODEM_H

#include <MacTypes.h>
#include "protocol.h"

typedef struct {
    Boolean active;
    unsigned long started, ends;   /* ticks */
    long bytes, lines, turns, errors;
    Boolean failed;
    char failure[80];
} LinkStats;

void session_init(void);
void session_connect(void);
void session_disconnect(void);
/* Reads the port, runs the core and draws. Called on every event-loop pass
   and from inside the permission dialog. */
void session_pump(void);
/* Dialogs and alerts the core asked for; called from the main loop only. */
void session_deferred(void);

/* Sends the input area's text as a prompt. Returns true if it was sent. */
Boolean session_send_input(void);
void session_stop(void);
/* Adds an information line to the transcript. */
void session_note(const char *text);

SessState session_state(void);
Boolean session_online(void);

/* Link test: dial, then send a fixed prompt to the demo bridge repeatedly,
   counting bytes, lines and serial errors. */
void session_linktest_start(long minutes);
void session_linktest_stop(void);
LinkStats *session_linkstats(void);

#endif
