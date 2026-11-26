/* Project picker: a modal dialog listing the bridge's projects with the List
   Manager (standard LDEF). It asks the bridge for a fresh list when it
   opens and keeps the serial pump running. Projects already open in a tab
   are marked and cannot be opened twice.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter), List
   Manager (LNew, LAddRow, LSetCell, LClick, LGetSelect, LUpdate, LDispose),
   Control Manager. */
#ifndef PICKER_H
#define PICKER_H

#include <MacTypes.h>

typedef enum { PICK_NONE, PICK_PROJECT, PICK_CHAT } PickResult;
/* PICK_PROJECT comes with the chosen project's name. The New Chat button is
   shown only when chat_ok (the bridge offers chats). */
PickResult picker_run(char *name, short max, Boolean chat_ok);
/* The bridge sent its project list again. */
void picker_projects_changed(void);

#endif
