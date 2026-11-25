/* Folds: long code blocks and runs of tool lines collapse into one marker
   line in the transcript, which a click or Cmd-J opens in place and closes
   again.

     [+] Code, 47 lines: def parse(text):
     [+] 5 tools: Read 3, Grep, Bash

   TextEdit cannot hide text, so a closed fold's lines are kept here and only
   the marker line stays in the transcript; opening puts the lines back right
   after the marker, and the marker then reads "[-]". Nothing is dropped: when
   old text is cut from the top of the transcript, a closed fold's lines go to
   the log in place of its marker.

   The window tells this module about every run it appends (fold_append), so
   every transcript line is seen and any other kind of line ends a block or a
   run. The module edits the transcript only through the FoldOps the window
   supplies, which makes it testable on the host against a plain string.
   Portable C with no Toolbox calls, host-tested. */
#ifndef FOLD_H
#define FOLD_H

#include "batch.h"

#define FOLD_CODE_LINES  12        /* a block of more lines than this folds */
#define FOLD_TOOL_RUN    3         /* this many tool lines in a row fold */
#define FOLD_MAX         256       /* folds per tab */
#define FOLD_MAX_BODY    12288L    /* bytes one closed fold keeps */
#define FOLD_MAX_BYTES   49152L    /* bytes all closed folds of a tab keep */
#define FOLD_TAIL        80
#define FOLD_NAMES       8

enum { FK_NONE = 0, FK_CODE = 1, FK_TOOL = 2 };

typedef struct {
    void *ctx;
    long width;                    /* marker width limit in the measure's units; 0: none */
    /* Copies transcript text [off, off+len) to out. */
    void (*read)(void *ctx, long off, long len, char *out);
    /* Replaces transcript text [off, off+oldlen) with text, in the style of
       kind (FK_CODE or FK_TOOL). Everything else that tracks transcript
       offsets (prompt marks, subagent lines) at or after off+oldlen moves by
       tlen-oldlen. */
    void (*replace)(void *ctx, long off, long oldlen, const char *text, long tlen, int kind);
    /* Width of s in kind's font. NULL: one unit per byte. */
    long (*measure)(void *ctx, int kind, const char *s, long n);
} FoldOps;

typedef struct {
    int kind;
    long id;                       /* unique per tab, survives other folds going away */
    long off, len;                 /* the marker line's text */
    long lines;
    int open;                      /* its lines are in the transcript after the marker */
    int growing;                   /* its block or run is still arriving */
    long open_len;                 /* open: bytes after the marker (a CR and the lines) */
    char *body;                    /* closed or growing: the lines joined by CR */
    long body_len, body_cap;
    char tail[FOLD_TAIL];          /* the first code line, or the tool names */
} Fold;

typedef struct {
    Fold *f;
    int n, cap;
    long bytes;                    /* what the closed folds keep */
    long next_id;
    /* The block or run being read. */
    int kind;
    long count, start, pend;
    int cur;                       /* its growing fold; -1 none yet; -2 it stays plain */
    char names[FOLD_NAMES][16];
    long ncount[FOLD_NAMES];
    int nnames, more_names;
} FoldSet;

void fold_init(FoldSet *fs);
void fold_free(FoldSet *fs);

/* The window appended text [base, base+len), one run of style, to the end of
   the transcript. Collapses a block or run that has grown long enough and
   keeps a growing fold's marker up to date. */
void fold_append(FoldSet *fs, const FoldOps *o, RunStyle style,
                 const char *text, long len, long base);

enum { FT_NOTHING = 0, FT_OPENED = 1, FT_CLOSED = 2,
       FT_BUSY = -1,           /* its block is still arriving */
       FT_TOO_BIG = -2,        /* too large to fold again */
       FT_MEMORY = -3 };
/* Opens a closed fold or closes an open one. */
int fold_toggle(FoldSet *fs, const FoldOps *o, int idx);

/* The fold whose marker line contains pos (the marker's start to its end), or
   -1. */
int fold_marker_at(const FoldSet *fs, long pos);
/* The fold whose marker line or open lines contain pos, or -1. */
int fold_containing(const FoldSet *fs, long pos);
/* The last fold whose marker starts in [from, to], or -1. */
int fold_last_in(const FoldSet *fs, long from, long to);
/* A fold's index from its id, or -1 when it is gone. */
int fold_by_id(const FoldSet *fs, long id);

/* A line at off changed its length by delta: later folds move. */
void fold_shift(FoldSet *fs, long off, long delta);

/* Sends transcript text [0, cut) to the log, with each closed fold's lines in
   place of its marker. range writes transcript text [off, off+len); bytes
   writes other text. Each returns 0 on success. Stops at the first failure and
   returns it. Changes nothing. */
typedef struct {
    void *ctx;
    int (*range)(void *ctx, long off, long len);
    int (*bytes)(void *ctx, const char *text, long len);
} FoldLog;
int fold_log(const FoldSet *fs, long cut, const FoldLog *log);

/* The first cut bytes left the transcript: forget the folds whose marker went
   with them and move the rest. */
void fold_cut(FoldSet *fs, long cut);

#endif
