/* Session log: text cut from the top of the transcript is appended to a TEXT
   file in "Claude30 Logs" beside the application, one file per session.
   Toolbox: Process Manager (GetProcessInformation), File Manager
   (FSMakeFSSpec, DirCreate, FSpCreate, FSpOpenDF, SetFPos, FSWrite, FSClose),
   OS Utilities (GetDateTime, SecondsToDate). */
#ifndef LOG_H
#define LOG_H

#include <MacTypes.h>

/* Starts a new session file name; the file is created on the first write. */
void log_new_session(void);
/* Appends bytes (Mac Roman, CR line breaks). Returns noErr on success. */
OSErr log_append(const char *text, long len);
void log_close(void);

#endif
