/* Settings: port, speed, dial string and the open tabs' projects (in strip
   order), kept in a PREF resource in "Claude30 Prefs" in the Preferences
   folder. Version 1 prefs load with no remembered tabs, version 2 with
   the input box expanded, version 3 with the scroll bar shown, version 4
   with no resume tokens.
   Toolbox: Folder Manager (FindFolder), File Manager (FSMakeFSSpec),
   Resource Manager (FSpCreateResFile, FSpOpenResFile, Get1Resource,
   AddResource, ChangedResource, UpdateResFile), Dialog Manager. */
#ifndef PREFS_H
#define PREFS_H

#include <MacTypes.h>

#define PREFS_VERSION 5
#define PREFS_TABS    6

typedef struct {
    short version;
    short port;        /* kPortModem or kPortPrinter */
    short speed;       /* kSpeed57600, kSpeed38400 or kSpeed19200 */
    Str63 dial;        /* Pascal string, sent verbatim */
    short ntabs;       /* remembered tabs */
    Str63 tabs[PREFS_TABS];  /* their project names, in strip order */
    short compact_input;     /* the input box is collapsed to one line */
    short hide_scrollbar;    /* the transcript has no scroll bar */
    char tokens[PREFS_TABS][17]; /* the tabs' resume tokens, C strings, same order */
} Prefs;

extern Prefs gPrefs;

void prefs_load(void);
void prefs_save(void);
/* Shows the Settings dialog; returns true if the user clicked OK. */
Boolean prefs_dialog(void);
/* Remembers the open tabs' projects and resume tokens (C strings, same
   order) and saves the prefs when they changed. */
void prefs_set_tabs(const char *const *names, const char *const *tokens, short n);
/* The dial string as a C string. */
void prefs_dial_cstr(char *out, short max);

#endif
