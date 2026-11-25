/* Tabs: one conversation per tab, up to six, each with its own transcript,
   input, batch, log file, pending permission request and marks. A tab keeps
   its session id (1 to 6) for its whole life, so a redial reopens it with
   the same id. The order of tabs in the strip is separate from their ids.
   Plain data and bookkeeping; window.c draws the strip, modem.c drives the
   sessions. Toolbox: TextEdit handles only (created by window.c). */
#ifndef TABS_H
#define TABS_H

#include <MacTypes.h>
#include <TextEdit.h>
#include "agentline.h"
#include "batch.h"
#include "log.h"
#include "permdlg.h"
#include "promptmarks.h"

#define MAX_TABS 6
#define TAB_NAME_MAX 63

typedef struct Tab {
    Boolean used;
    short sid;                   /* session id, 1..6 */
    char project[TAB_NAME_MAX + 1];
    TEHandle trans;              /* transcript, styled */
    TEHandle input;              /* input area */
    Batch batch;                 /* runs waiting to be drawn */
    LogFile log;
    PermReq perm;
    Boolean want_perm;           /* a request waits for its dialog */
    Boolean running;             /* its turn is running (strip mark) */
    Boolean fresh;               /* output arrived while in the background */
    Boolean unavailable;         /* the bridge refused or ended it */
    Boolean from_picker;         /* made by New Tab: a refused open removes it */
    Boolean refused;             /* ... and it was refused: remove it */
    char token[17];              /* resume token of its conversation ("" = none) */
    char model[32];              /* its Claude model ("" = the bridge default) */
    char effort[8];              /* low, medium, high, xhigh or max ("" = default) */
    Boolean token_sent;          /* opened asking to resume: its <n>Y says how it went */
    Boolean opened_once;         /* a conversation ran in it this launch */
    Boolean reopened;            /* opened; waiting for its first > */
    Boolean log_trouble;         /* its log could not be written */
    OSErr log_err;               /* last log error, until reported */
    long lost_bytes;             /* trimmed without reaching the log */
    char last_tool[64];
    PromptMarks marks;           /* where its prompts start in the transcript */
    AgentLines agents;           /* its subagents' lines */
    int agent_pending;           /* slot whose line the next append draws, or -1 */
} Tab;

/* Adds a tab for project with the given session id, after the front tab
   (or at the end). Returns NULL if the id is in use or memory ran out. */
Tab *tabs_add(const char *project, short sid);
/* Removes the tab; the front tab becomes the one to its left, else the new
   first tab. Frees its batch, log and request (not its TextEdit records:
   call window_tab_free first). */
void tabs_remove(Tab *t);

short tabs_count(void);
Tab *tabs_at(short index);       /* by position in the strip, 0-based */
short tabs_index(const Tab *t);
Tab *tabs_by_sid(short sid);
Tab *tabs_by_project(const char *project);
Tab *tabs_front(void);
void tabs_set_front(Tab *t);

#endif
