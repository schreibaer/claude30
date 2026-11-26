/* Session logs: text cut from the top of a tab's transcript is appended to a
   TEXT file in "Claude30 Logs" beside the application, one file per tab and
   conversation. Names fit HFS's 31 characters and stay unique per tab:
   "claude30-web 11-24 10.31.05 #2".
   Toolbox: Process Manager (GetProcessInformation), File Manager
   (FSMakeFSSpec, DirCreate, FSpCreate, FSpOpenDF, SetFPos, FSWrite, FSClose),
   OS Utilities (GetDateTime, SecondsToDate). */
#ifndef LOG_H
#define LOG_H

#include <MacTypes.h>

typedef struct {
    short ref;                   /* open data fork, or 0 */
    Str63 name;
    Boolean have_name;
    Boolean discard;             /* a chat: nothing is ever written */
} LogFile;

/* Starts a new file name for the project and tab digit; the file is created
   on the first write. */
void log_new_session(LogFile *lf, const char *project, short digit);
/* Appends bytes (Mac Roman, CR line breaks). Returns noErr only when every
   byte was written. */
OSErr log_append(LogFile *lf, const char *text, long len);
/* Closes the file; later appends reopen the same file. */
void log_close(LogFile *lf);

#endif
