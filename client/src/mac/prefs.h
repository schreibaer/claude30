/* Settings: port, speed and dial string, kept in a PREF resource in
   "Claude30 Prefs" in the Preferences folder.
   Toolbox: Folder Manager (FindFolder), File Manager (FSMakeFSSpec),
   Resource Manager (FSpCreateResFile, FSpOpenResFile, Get1Resource,
   AddResource, ChangedResource, UpdateResFile), Dialog Manager. */
#ifndef PREFS_H
#define PREFS_H

#include <MacTypes.h>

#define PREFS_VERSION 1

typedef struct {
    short version;
    short port;        /* kPortModem or kPortPrinter */
    short speed;       /* kSpeed57600, kSpeed38400 or kSpeed19200 */
    Str63 dial;        /* Pascal string, sent verbatim */
} Prefs;

extern Prefs gPrefs;

void prefs_load(void);
void prefs_save(void);
/* Shows the Settings dialog; returns true if the user clicked OK. */
Boolean prefs_dialog(void);
/* The dial string as a C string. */
void prefs_dial_cstr(char *out, short max);

#endif
