/* Transcript batcher: turns protocol events into styled runs of text that the
   window appends once per event-loop pass, and plans how much old text to cut
   when the transcript nears the TextEdit limit. Portable C, host-tested. */
#ifndef BATCH_H
#define BATCH_H

#include "protocol.h"

typedef enum {
    RS_PROSE,        /* Claude's prose */
    RS_CODE,         /* code and diffs */
    RS_PROMPT,       /* what the user sent */
    RS_TOOL,         /* a tool Claude used */
    RS_INFO,         /* bridge information */
    RS_ERROR,        /* a tool error */
    RS_RESULT,       /* turn status and cost */
    RS_INTERRUPTED,  /* the user stopped the turn */
    RS_NOISE,        /* serial errors: output above may be damaged */
    RS_DIVIDER,      /* new conversation */
    RS_COUNT
} RunStyle;

typedef struct {
    RunStyle style;
    long start;
    long len;
} Run;

/* Hard limits that keep any single insert well inside TextEdit's 32 KB. */
#define BATCH_MAX_RUN   8000L   /* bytes per run */
#define BATCH_MAX_LINE  4000L   /* a longer line gets a line break inserted */

/* Trim policy. */
#define TRIM_AT_BYTES   28672L
#define TRIM_AT_LINES   6000L
#define TRIM_KEEP_BYTES 20480L
#define TRIM_KEEP_LINES 4000L

typedef struct {
    char *text;
    long len, cap;
    Run *runs;
    int nruns, rcap;
    int need_break;     /* the transcript is not empty: next block starts a line */
    long line_len;      /* bytes since the last line break */
    int oom;            /* memory ran out; some text was dropped */
} Batch;

void batch_init(Batch *b);
void batch_free(Batch *b);
/* Forget the runs after they were drawn; keeps line state. */
void batch_clear(Batch *b);
/* Starts the next block on a fresh line, e.g. after the transcript was
   emptied (call with 0) or when text already exists (call with 1). */
void batch_set_need_break(Batch *b, int need);

void batch_event(Batch *b, EventKind kind, const char *text, long len);
void batch_prompt(Batch *b, const char *text, long len);
/* A marker line in the given style; text is the whole line. */
void batch_marker(Batch *b, RunStyle style, const char *text);

/* How many bytes to cut from the front of the transcript, at a line break.
   display_lines is the number of lines the window shows (soft-wrapped), or 0
   to count line breaks. incoming is the size of text about to be appended,
   which counts toward the byte limits. Returns 0 when no trim is needed. */
long batch_trim_cut(const char *text, long len, long display_lines, long incoming);

#endif
