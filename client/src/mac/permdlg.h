/* Permission dialog: shows the tool, the summary and the whole body (diff,
   file or command) in a scrolling line list drawn with QuickDraw, so no
   TextEdit size limit applies. Long body lines wrap onto marked rows.
   Only Cmd-Y, Cmd-N, Cmd-A and Cmd-period answer; Return, plain keys and
   held keys do nothing. Keys and clicks made before the dialog opened are
   discarded, and the buttons ignore clicks for 0.75 s after it opens.
   Each tab collects its own request in a PermReq, so a request arriving in
   one tab never changes the dialog open for another.
   Toolbox: Dialog Manager (GetNewDialog, ModalDialog with a filter,
   SetDialogItem user item), Control Manager, QuickDraw, Memory Manager,
   Event Manager (FlushEvents). */
#ifndef PERMDLG_H
#define PERMDLG_H

#include <MacTypes.h>

enum { kPermResultAllow = 'y', kPermResultDeny = 'n', kPermResultAlways = 'a',
       kPermResultStop = 's', kPermResultClosed = 0 };

typedef struct {
    Handle body;                 /* all body lines, back to back */
    Handle line_ends;            /* long offsets: end of each body line */
    long body_len, nlines;
    Str255 title;
    Boolean close_requested;
    Boolean incomplete;          /* some of the request could not be kept */
} PermReq;

void permdlg_begin(PermReq *q, const char *title, long len);
void permdlg_line(PermReq *q, const char *text, long len);
/* Part of the request was lost (out of memory): the dialog says so and
   offers only Deny. */
void permdlg_mark_incomplete(PermReq *q);
/* Asks an open dialog to close without an answer (link lost, tab closed). */
void permdlg_request_close(PermReq *q);
/* Runs the dialog; returns one of the kPermResult values. The serial pump
   keeps running while it is open. */
short permdlg_run(PermReq *q);
void permdlg_free(PermReq *q);

#endif
