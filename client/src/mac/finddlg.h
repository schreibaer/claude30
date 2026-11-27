/* The Find dialog: one edit field, Find and Cancel.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter proc,
   GetDialogItem, SetDialogItemText, SelectDialogItemText). */
#ifndef FINDDLG_H
#define FINDDLG_H

#include <MacTypes.h>

/* Asks for text to find, starting with text. True when the person chose Find
   with a non-empty string, which is stored in text (max bytes, with the NUL). */
Boolean finddlg_run(char *text, short max);

#endif
