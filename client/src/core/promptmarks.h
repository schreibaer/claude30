/* Where the person's prompts start in a tab's transcript, so Cmd-Up and
   Cmd-Down can jump between them. Offsets follow the transcript as old text
   is trimmed from its front. Portable C, host-tested. */
#ifndef PROMPTMARKS_H
#define PROMPTMARKS_H

#define PM_MAX 256

typedef struct {
    long off[PM_MAX];             /* ascending */
    int n;
} PromptMarks;

void pm_init(PromptMarks *m);
/* Records a prompt starting at off (at or after the last one). When full,
   the oldest is forgotten. */
void pm_add(PromptMarks *m, long off);
/* The first cut bytes left the transcript: shift, and forget the prompts
   that went with them. */
void pm_cut(PromptMarks *m, long cut);
/* The last prompt before pos, or -1. */
long pm_prev(const PromptMarks *m, long pos);
/* The first prompt after pos, or -1. */
long pm_next(const PromptMarks *m, long pos);

#endif
