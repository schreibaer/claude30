/* The Markdown styler: turns Claude's prose into styled runs for the batcher
   (batch.h). Portable C with no Toolbox calls; the window maps the faces the
   runs carry (BF_* in batch.h) to Geneva and Monaco in 9 point.

   What it shows, per line of prose (every T payload is one line):
     **bold** __bold__ *italic* _italic_ ***both***   faces, marks removed
     `code` ``code`` ```code```                       Monaco, backticks removed
     [text](url) <https://...> https://...            underlined; the address of
                                                      a [text](url) is dropped
     \* \_ \` \# ...                                  the mark itself
     # Title .. ###### Title                          bold, level 1 also extended
                                                      and underlined, 2 extended
     - item * item + item                             a bullet; an indented item
                                                      gets an en dash
     > quote                                          "| quote" in italics
     --- *** ___ (3 or more, spaces allowed)          a rule of em dashes
     | a | b |                                        Monaco, as typed; a separator
                                                      row becomes |---+---|
   Anything that is not well-formed Markdown shows as typed: a mark with no
   partner on the line, snake_case, "2 * 3", ~~strike~~.

   A span is held back until its closing mark arrives, so a streamed
   paragraph shows the words before it at once and the styled phrase when it
   is complete. Held text never passes MD_HOLD_MAX bytes: past that the
   opening mark shows as typed. A paragraph that ends first (the next block,
   see batch.c) is drawn as typed. */
#ifndef MDSTYLE_H
#define MDSTYLE_H

#include "batch.h"

#define MD_HOLD_MAX 512

typedef struct MdStyle MdStyle;

/* Prose for the batch: cont = 0 starts a line, cont = 1 continues the last one
   (joined with one space). */
void md_text(Batch *b, const char *text, long len, int cont);
/* Ends the line that is open: everything held is drawn, as typed where no
   closing mark came. Does nothing when no prose is open. */
void md_flush(Batch *b);
/* Frees the styler's memory (batch_free calls it). */
void md_free(Batch *b);

#endif
