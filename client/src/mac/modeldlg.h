/* Session > Model and Effort...: a modal dialog with the bridge's models in
   a List Manager list (the first row is the bridge default) and six radio
   buttons for the effort. It keeps the serial pump running and closes
   itself when the link goes away.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter), List
   Manager (LNew, LAddRow, LSetCell, LClick, LGetSelect, LUpdate, LDispose),
   Control Manager (radio buttons). */
#ifndef MODELDLG_H
#define MODELDLG_H

#include <MacTypes.h>

/* Shows the dialog for a tab's current model and effort ("" is the bridge
   default). Returns true if OK was pressed, with the chosen values in
   model (at least 32 bytes) and effort (at least 8 bytes). */
Boolean modeldlg_run(const char *project, const char *model, const char *effort,
                     char *outModel, char *outEffort);

#endif
