/* Permission dialog: shows the tool, the summary and the whole body (diff,
   file or command) in a scrolling line list drawn with QuickDraw, so no
   TextEdit size limit applies. Long body lines wrap onto marked rows.
   Only Cmd-Y, Cmd-N, Cmd-A and Cmd-period answer; Return and plain keys do
   nothing, and keys typed before the dialog opened are discarded.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter,
   SetDialogItem user item), Control Manager, QuickDraw, Memory Manager,
   Event Manager (FlushEvents). */
#ifndef PERMDLG_H
#define PERMDLG_H

#include <MacTypes.h>

enum { kPermResultAllow = 'y', kPermResultDeny = 'n', kPermResultAlways = 'a',
       kPermResultStop = 's', kPermResultClosed = 0 };

void permdlg_begin(const char *title, long len);
void permdlg_line(const char *text, long len);
/* Asks an open dialog to close without an answer (link lost). */
void permdlg_request_close(void);
/* Runs the dialog; returns one of the kPermResult values. The serial pump
   keeps running while it is open. */
short permdlg_run(void);

#endif
