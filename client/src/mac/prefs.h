/* Settings: port, speed, dial string and the open tabs' projects (in strip
   order), kept in a PREF resource in "Claude30 Prefs" in the Preferences
   folder. Version 1 prefs load with no remembered tabs, version 2 with
   the input box expanded, version 3 with the scroll bar shown, version 4
   with no resume tokens, version 5 with every tab at the bridge's model
   and effort, version 6 with Stream Answers off in every tab, version 7
   with the cost shown.
   Toolbox: Folder Manager (FindFolder), File Manager (FSMakeFSSpec),
   Resource Manager (FSpCreateResFile, FSpOpenResFile, Get1Resource,
   AddResource, ChangedResource, UpdateResFile), Dialog Manager. */
#ifndef PREFS_H
#define PREFS_H

#include <MacTypes.h>

#define PREFS_VERSION 8
#define PREFS_TABS    6

typedef struct {
    short version;
    short port;        /* kPortModem or kPortPrinter */
    short speed;       /* kSpeed57600, kSpeed38400, kSpeed19200 or kSpeed115200 */
    Str63 dial;        /* Pascal string, sent verbatim */
    short ntabs;       /* remembered tabs */
    Str63 tabs[PREFS_TABS];  /* their project names, in strip order */
    short compact_input;     /* the input box is collapsed to one line */
    short hide_scrollbar;    /* the transcript has no scroll bar */
    char tokens[PREFS_TABS][17]; /* the tabs' resume tokens, C strings, same order */
    char models[PREFS_TABS][32]; /* the tabs' models, C strings ("" = bridge default) */
    char efforts[PREFS_TABS][8]; /* the tabs' efforts, C strings ("" = bridge default) */
    char streams[PREFS_TABS];    /* 1 = Stream Answers on in the tab */
    short hide_cost;             /* the status line leaves out the dollar figure */
} Prefs;

extern Prefs gPrefs;

void prefs_load(void);
void prefs_save(void);
/* Shows the Settings dialog; returns true if the user clicked OK. */
Boolean prefs_dialog(void);
/* Remembers the open tabs' projects, resume tokens, models, efforts and
   Stream Answers flags (C strings, same order) and saves the prefs when they changed. */
void prefs_set_tabs(const char *const *names, const char *const *tokens,
                    const char *const *models, const char *const *efforts,
                    const char *streams, short n);
/* The dial string as a C string. */
void prefs_dial_cstr(char *out, short max);

#endif
