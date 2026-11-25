/* Subagent lines: one line in the transcript per running subagent, rewritten
   in place as the bridge reports progress and replaced by a summary when it
   ends. This module parses the J+, J~ and J- payloads, composes each line's
   text (Mac Roman), cuts it to the window width, and keeps a small table of
   where each line sits in the transcript so the window can replace it.
   Portable C with no Toolbox calls, host-tested. */
#ifndef AGENTLINE_H
#define AGENTLINE_H

#define AL_SLOTS 8      /* lines that can be rewritten at once, per tab */
#define AL_PLAIN 8      /* subagents past that: shown as plain tool lines */
#define AL_TEXT  200    /* bytes of one composed line, indent included */

enum { AL_FREE = 0, AL_OPEN = 1, AL_ENDED = 2 };

typedef struct {
    int state;
    int k;                   /* the bridge's number for it */
    unsigned long seq;       /* order of use, for reusing the oldest ended slot */
    int drawn;               /* its line is in the transcript: off and len are valid */
    int gone;                /* ... and was trimmed away */
    long off, len;           /* where the line's text is (not its line break) */
    int steps;
    char type[24], task[80], tool[24];
} AgentSlot;

typedef struct {
    AgentSlot s[AL_SLOTS];
    struct { int k; char type[24]; } plain[AL_PLAIN];
    int nplain;
    unsigned long seq;
} AgentLines;

void al_init(AgentLines *a);

/* J+ "<k> <type> <task>". Returns the slot (>= 0) with the line's text in
   out; -2 when every slot is open, with the plain tool-line text in out; -1
   for a malformed payload. */
int al_start(AgentLines *a, const char *pay, long n, char *out, long cap);
/* J~ "<k> <steps> <tool> <summary>". Returns the slot, with the new text, or
   -1 when no open slot has that number. */
int al_step(AgentLines *a, const char *pay, long n, char *out, long cap);
/* J- "<k> <status> <steps> <summary>". Returns the slot, with its final text;
   -2 when it was one of the plain ones, with the plain end line in out; -1
   when nothing is open with that number. */
int al_end(AgentLines *a, const char *pay, long n, char *out, long cap);
/* The turn ended with lines open: the next open slot, now ended as stopped
   (text in out), or -1 when none is left. Call until it returns -1. */
int al_stop_next(AgentLines *a, char *out, long cap);

/* The first cut bytes left the transcript: shift the lines and forget the
   ones that went with them. */
void al_cut(AgentLines *a, long cut);
/* A line at off changed its length by delta: later lines move. */
void al_shift(AgentLines *a, long off, long delta);

/* Cuts s (len bytes, in place) so measure(s, len) <= maxw, ending it with an
   ellipsis when something went. The two-space indent stays. Returns the new
   length. */
long al_fit(char *s, long len, long maxw, long (*measure)(const char *, long));

#endif
