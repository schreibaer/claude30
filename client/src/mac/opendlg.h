/* Open Project File: a modal dialog that browses the front tab's project
   folder, folders first, with a path line and a parent button, and returns
   the chosen file. It keeps the serial pump running while a listing loads,
   and says so when a tab waits for a permission answer.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter), List
   Manager (LNew, LAddRow, LSetCell, LClick, LGetSelect, LSetSelect,
   LAutoScroll, LUpdate, LDispose), Control Manager, QuickDraw. */
#ifndef OPENDLG_H
#define OPENDLG_H

#include <MacTypes.h>
#include "protocol.h"
#include "tabs.h"

/* Returns true with the chosen file's path, relative to the project. */
Boolean opendlg_run(Tab *t, char *path, short max);
/* Listing events from modem.c. */
void opendlg_event(short sid, EventKind k, const char *text, long len);

#endif
