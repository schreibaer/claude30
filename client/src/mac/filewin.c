#include "filewin.h"
#include "files.h"
#include "modem.h"
#include "resources.h"
#include "window.h"
#include "toolbox.h"
#include <Dialogs.h>
#include <Fonts.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <TextEdit.h>
#include <TextUtils.h>
#include <ToolUtils.h>
#include <stdio.h>
#include <string.h>

#define MONACO        4
#define FONT_SIZE     9
#define SCROLL_UNIT   4          /* editable windows scroll in pixels / 4 */
#define TEXT_LIMIT    31000L     /* typing past this beeps */
#define SLICE_BYTES   24000L     /* read-only windows keep about this much */
#define SLICE_LINES   1200L      /* ... and at most this many lines */
#define EDGE_LINES    50         /* fetch more when this close to the slice's edge */
#define CLIP_COLS     500        /* read-only lines are clipped here */
#define WIDE_DEST     3200       /* read-only text never wraps */
#define STATUS_H      14
#define WIN_W         480
#define WIN_H         290

#ifndef zoomDocProc
#define zoomDocProc   8          /* document window with a zoom box */
#endif

enum { FW_LOADING, FW_READY, FW_SAVING };
enum { FETCH_NONE, FETCH_REPLACE, FETCH_APPEND, FETCH_PREPEND };
enum { SAVE_IDLE, SAVE_RUNNING, SAVE_OK, SAVE_FAILED, SAVE_CANCELLED };

typedef struct {
    WindowPtr w;                 /* NULL: slot free */
    short sid;
    char project[TAB_NAME_MAX + 1];
    char path[256];
    const char *name;            /* last component of path */
    char label[96];              /* title text: the name, or "Changes" / "<name> diff" */
    Boolean virt;                /* :changes or :diff/..., a snapshot the bridge makes */
    Boolean changes;             /* the :changes list: lines open diffs */
    unsigned long click_tick;    /* the last click in the list, for the double-click */
    long click_start;            /* the text offset of that line, or -1 */
    TEHandle te;
    ControlHandle bar;
    Rect view, status_rect;
    short line_h;
    Boolean editable;            /* mode e on a files=rw bridge */
    char mode[24];
    char token[24];
    Boolean dirty, offline, host_changed, clipped, hidden;
    short phase;
    int xfer;                    /* the load, page or save transfer, or -1 */
    int check;                   /* a token check, or -1 */
    long total;                  /* lines in the file (as sent) */
    /* loading an editable file */
    Handle load;
    long load_len, next_line;
    long keep_top;               /* scroll place to restore after a reload */
    /* read-only slice */
    long slice_first, slice_n, top, target;
    short fetch;
    /* saving */
    short save_state;
    short blocks, blocks_done;
    Boolean conflict;            /* waiting for Cancel or Replace */
    char status[128];
    /* deferred alerts */
    Boolean open_failed;
    char alert[256];
} FileWin;

static FileWin gWins[MAX_FILEWINS];
static ControlActionUPP gBarUPP;
static FileWin *gTracking;       /* the window whose scroll bar is tracked */

/* ---- small helpers ------------------------------------------------------- */

static FileWin *find(WindowPtr w)
{
    short i;
    if (!w)
        return NULL;
    for (i = 0; i < MAX_FILEWINS; i++)
        if (gWins[i].w == w)
            return &gWins[i];
    return NULL;
}

static short index_of(FileWin *f)
{
    return (short)(f - gWins);
}

static void c2p(const char *s, Str255 p)
{
    short n = (short)strlen(s);
    if (n > 255)
        n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, s, n);
}

static Boolean starts(const char *t, long n, const char *s)
{
    long k = (long)strlen(s);
    return n >= k && memcmp(t, s, k) == 0;
}

/* Plain words for a refusal the bridge sent. */
static void plain_reason(const char *t, long n, char *out, short max)
{
    if (starts(t, n, "not found"))
        strcpy(out, "The file no longer exists.");
    else if (starts(t, n, "binary"))
        strcpy(out, "It is a binary file, which Claude30 cannot show.");
    else if (starts(t, n, "hidden"))
        strcpy(out, "It is hidden: secrets, .git and ignored files stay on the host.");
    else if (starts(t, n, "outside project"))
        strcpy(out, "It is outside the project.");
    else if (starts(t, n, "bad name"))
        strcpy(out, "Its name cannot be sent over the link.");
    else if (starts(t, n, "read-only"))
        strcpy(out, "The file cannot be saved: it opened read-only.");
    else if (starts(t, n, "too big"))
        strcpy(out, "The text is too big to save.");
    else if (starts(t, n, "damaged"))
        strcpy(out, "The text arrived damaged, so nothing was written. Try again.");
    else if (starts(t, n, "control"))
        strcpy(out, "The text holds control characters, so nothing was written.");
    else if (starts(t, n, "busy"))
        strcpy(out, "The bridge is busy with other saves. Try again.");
    else if (starts(t, n, "files off"))
        strcpy(out, "The bridge does not allow file access (--files off).");
    else if (starts(t, n, "not git"))
        strcpy(out, "This project is not a git repository, or git is missing on the host.");
    else if (starts(t, n, "changed"))
        strcpy(out, "The snapshot is gone. Revert to Saved takes a new one.");
    else if (starts(t, n, "failed timeout"))
        strcpy(out, "Git took too long on the host.");
    else if (starts(t, n, "failed "))
        snprintf(out, max, "The host reported an error: %.*s", (int)(n - 7), t + 7);
    else
        snprintf(out, max, "%.*s", (int)n, t);
}

static const char *ro_reason(const char *mode)
{
    if (strcmp(mode, "r:big") == 0)
        return "too big to edit";
    if (strcmp(mode, "r:long") == 0)
        return "over 32,000 lines; the first 32,000 are shown";
    if (strcmp(mode, "r:encoding") == 0)
        return "not UTF-8, shown as Latin-1";
    if (strcmp(mode, "r:chars") == 0)
        return "it has characters the Mac cannot show";
    if (strcmp(mode, "r:control") == 0)
        return "it has control characters";
    if (strcmp(mode, "r:endings") == 0)
        return "its line endings are mixed";
    if (strcmp(mode, "r:ro") == 0)
        return "the bridge allows reading only";
    if (strcmp(mode, "r:changes") == 0 || strcmp(mode, "r:diff") == 0)
        return "a snapshot";
    return "it cannot be edited here";
}

/* ---- drawing ------------------------------------------------------------- */

static void set_title(FileWin *f)
{
    char s[300];
    Str255 p;
    snprintf(s, sizeof(s), "%s%s \xD1 %s", f->dirty ? "\xA5 " : "", f->label, f->project);
    c2p(s, p);
    SetWTitle(f->w, p);
}

static void draw_status(FileWin *f)
{
    GrafPtr save;
    char s[200];
    GetPort(&save);
    SetPort(f->w);
    EraseRect(&f->status_rect);
    MoveTo(f->status_rect.left, f->status_rect.top);
    LineTo(f->status_rect.right, f->status_rect.top);
    if (f->status[0])
        strcpy(s, f->status);
    else if (f->offline)
        strcpy(s, "Offline: Save works again once the tab reconnects.");
    else if (f->host_changed)
        strcpy(s, "Changed on the host.");
    else if (f->virt && f->phase == FW_READY)
        strcpy(s, f->changes ? "Snapshot. Double-click a file for its diff. Revert to Saved refreshes."
                             : "Snapshot. Revert to Saved refreshes.");
    else if (!f->editable && f->mode[0])
        snprintf(s, sizeof(s), "Read-only: %s. Lines %ld-%ld of %ld%s.", ro_reason(f->mode),
                 f->slice_n ? f->top + 1 : 0, f->top + 1 + ((f->view.bottom - f->view.top) / f->line_h),
                 f->total, f->clipped ? "; long lines are clipped" : "");
    else
        s[0] = 0;
    TextFont(3);
    TextSize(FONT_SIZE);
    MoveTo(f->status_rect.left + 4, f->status_rect.bottom - 3);
    DrawText(s, 0, (short)strlen(s));
    TextFont(MONACO);
    SetPort(save);
}

static void set_status(FileWin *f, const char *s)
{
    if (strcmp(s, f->status) == 0)
        return;
    snprintf(f->status, sizeof(f->status), "%.120s", s);
    draw_status(f);
}

static void layout(FileWin *f)
{
    Rect r = f->w->portRect;
    SetRect(&f->view, r.left + 4, r.top + 2, r.right - 16, r.bottom - STATUS_H - 1);
    SetRect(&f->status_rect, r.left, r.bottom - STATUS_H, r.right - 15, r.bottom);
}

/* ---- scrolling ------------------------------------------------------------ */

static short visible_lines(FileWin *f)
{
    short n = (short)((f->view.bottom - f->view.top) / f->line_h);
    return n > 0 ? n : 1;
}

static long scrolled_px(FileWin *f)
{
    return (long)((*f->te)->viewRect.top - (*f->te)->destRect.top);
}

static long max_scroll_px(FileWin *f)
{
    long h = (*f->te)->nLines > 0 ? TEGetHeight((*f->te)->nLines, 1, f->te) : 0;
    long m = h - (f->view.bottom - f->view.top);
    return m > 0 ? m : 0;
}

static void sync_bar(FileWin *f)
{
    long maxv;
    if (f->editable) {
        maxv = (max_scroll_px(f) + SCROLL_UNIT - 1) / SCROLL_UNIT;
        if (maxv > 32000)
            maxv = 32000;
        SetControlMaximum(f->bar, (short)maxv);
        SetControlValue(f->bar, (short)(scrolled_px(f) / SCROLL_UNIT));
    } else {
        maxv = f->total - visible_lines(f);
        if (maxv < 0)
            maxv = 0;
        if (maxv > 32000)
            maxv = 32000;
        SetControlMaximum(f->bar, (short)maxv);
        SetControlValue(f->bar, (short)(f->top > 32000 ? 32000 : f->top));
    }
}

static void scroll_px_to(FileWin *f, long px)
{
    long maxpx = max_scroll_px(f), delta;
    if (px > maxpx)
        px = maxpx;
    if (px < 0)
        px = 0;
    delta = scrolled_px(f) - px;
    while (delta) {
        long step = delta > 30000 ? 30000 : delta < -30000 ? -30000 : delta;
        TEScroll(0, (short)step, f->te);
        delta -= step;
    }
}

static void request_piece(FileWin *f, long first, long count, short fetch);

/* Read-only: shows file line `line` at the top, fetching what is missing. */
static void ro_set_top(FileWin *f, long line)
{
    long maxtop = f->total - visible_lines(f);
    char s[64];
    if (maxtop < 0)
        maxtop = 0;
    if (line > maxtop)
        line = maxtop;
    if (line < 0)
        line = 0;
    f->top = line;
    /* The top line being loaded is enough: the edge fetches below fill
       the rest of the view. (Asking for the whole view could never be met
       when long lines fill the byte budget of a piece.) */
    if (line >= f->slice_first
            && (line < f->slice_first + f->slice_n
                || f->slice_first + f->slice_n >= f->total)) {
        scroll_px_to(f, (line - f->slice_first) * f->line_h);
        if (f->fetch == FETCH_NONE && f->xfer < 0) {
            if (line - f->slice_first < EDGE_LINES && f->slice_first > 0) {
                long first = f->slice_first - 200;
                if (first < 0)
                    first = 0;
                request_piece(f, first, f->slice_first - first, FETCH_PREPEND);
            } else if (f->slice_first + f->slice_n - (line + visible_lines(f)) < EDGE_LINES
                       && f->slice_first + f->slice_n < f->total) {
                request_piece(f, f->slice_first + f->slice_n, 200, FETCH_APPEND);
            }
        }
        if (f->fetch != FETCH_REPLACE)
            f->status[0] = 0;
    } else {
        /* A jump: the old text stays until the new piece arrives. */
        f->target = line;
        if (f->fetch != FETCH_REPLACE) {
            if (f->xfer >= 0) {
                proto_xfer_cancel(session_proto(), f->xfer);
                f->xfer = -1;
            }
            request_piece(f, line, 200, FETCH_REPLACE);   /* starts at the target */
        }
        snprintf(s, sizeof(s), "Loading line %ld\xC9", line + 1);
        strcpy(f->status, s);
    }
    sync_bar(f);
    draw_status(f);
}

static pascal void bar_action(ControlHandle c, short part)
{
    FileWin *f = gTracking;
    long page, delta = 0;
    (void)c;
    if (!f)
        return;
    if (f->editable) {
        page = (f->view.bottom - f->view.top) - f->line_h;
        switch (part) {
        case inUpButton:   delta = -f->line_h; break;
        case inDownButton: delta = f->line_h; break;
        case inPageUp:     delta = -page; break;
        case inPageDown:   delta = page; break;
        default: return;
        }
        scroll_px_to(f, scrolled_px(f) + delta);
        sync_bar(f);
    } else {
        page = visible_lines(f) - 1;
        switch (part) {
        case inUpButton:   delta = -1; break;
        case inDownButton: delta = 1; break;
        case inPageUp:     delta = -page; break;
        case inPageDown:   delta = page; break;
        default: return;
        }
        ro_set_top(f, f->top + delta);
    }
}

/* ---- text into the record ------------------------------------------------- */

/* Copies a read-only piece with long lines clipped. Returns a new pointer
   (caller disposes) and its length, or NULL. */
static Ptr clip_lines(FileWin *f, const char *t, long n, long *out_len)
{
    Ptr p = NewPtr(n + 1);
    long i, col = 0, o = 0;
    if (!p)
        return NULL;
    for (i = 0; i < n; i++) {
        if (t[i] == '\r') {
            col = 0;
            p[o++] = '\r';
        } else if (col < CLIP_COLS) {
            p[o++] = t[i];
            col++;
        } else if (col == CLIP_COLS) {
            p[o - 1] = (char)0xC9;   /* ellipsis */
            col++;
            f->clipped = true;
        }
    }
    *out_len = o;
    return p;
}

static void quiet(FileWin *f, Boolean on)
{
    static RgnHandle saved;
    Rect empty = { 0, 0, 0, 0 };
    if (!saved)
        saved = NewRgn();
    SetPort(f->w);
    if (on) {
        GetClip(saved);
        ClipRect(&empty);
    } else {
        SetClip(saved);
    }
}

/* Read-only: drops lines from one end until the slice fits its budget. */
static void ro_trim(FileWin *f, Boolean from_front)
{
    TEHandle te = f->te;
    long k, cut;
    while (((*te)->teLength > SLICE_BYTES || f->slice_n > SLICE_LINES) && f->slice_n > 400) {
        k = f->slice_n > 600 ? 200 : f->slice_n - 400;
        if (from_front) {
            cut = (*te)->lineStarts[k];
            TESetSelect(0, cut, te);
            TEDelete(te);
            OffsetRect(&(*te)->destRect, 0, (short)(k * f->line_h));
            f->slice_first += k;
        } else {
            cut = (*te)->lineStarts[f->slice_n - k] - 1;
            TESetSelect(cut, (*te)->teLength, te);
            TEDelete(te);
        }
        f->slice_n -= k;
    }
}

static void ro_piece(FileWin *f, const FileXfer *x, const char *t, long n)
{
    long len;
    Ptr p = clip_lines(f, t, n, &len);
    TEHandle te = f->te;
    short fetch = f->fetch;
    if (!p) {
        set_status(f, "Out of memory: the text could not be shown.");
        return;
    }
    f->fetch = FETCH_NONE;
    quiet(f, true);
    if (fetch == FETCH_REPLACE || f->slice_n == 0) {
        TESetText(p, len, te);
        (*te)->destRect = (*te)->viewRect;
        (*te)->destRect.right = (*te)->destRect.left + WIDE_DEST;
        TECalText(te);
        f->slice_first = x->h_first;
        f->slice_n = x->h_count;
        quiet(f, false);
        InvalRect(&f->view);
        f->status[0] = 0;
        ro_set_top(f, f->target);
    } else if (fetch == FETCH_APPEND && x->h_first == f->slice_first + f->slice_n) {
        TESetSelect((*te)->teLength, (*te)->teLength, te);
        if (f->slice_n > 0)
            TEInsert("\r", 1, te);
        TEInsert(p, len, te);
        f->slice_n += x->h_count;
        ro_trim(f, true);
        quiet(f, false);
        ro_set_top(f, f->top);
    } else if (fetch == FETCH_PREPEND && x->h_first + x->h_count == f->slice_first) {
        TESetSelect(0, 0, te);
        TEInsert("\r", 1, te);
        TESetSelect(0, 0, te);
        TEInsert(p, len, te);
        OffsetRect(&(*te)->destRect, 0, (short)-(x->h_count * f->line_h));
        f->slice_first = x->h_first;
        f->slice_n += x->h_count;
        ro_trim(f, false);
        quiet(f, false);
        ro_set_top(f, f->top);
    } else {
        quiet(f, false);         /* a piece that no longer fits: ask again */
        ro_set_top(f, f->top);
    }
    DisposePtr(p);
}

/* ---- transfers ------------------------------------------------------------ */

static void request_piece(FileWin *f, long first, long count, short fetch)
{
    f->xfer = proto_read(session_proto(), f->sid, index_of(f), f->path, first, count, PIECE_BYTES);
    f->fetch = f->xfer >= 0 ? fetch : FETCH_NONE;
}

static void start_load(FileWin *f)
{
    f->phase = FW_LOADING;
    f->load_len = 0;
    f->next_line = 0;
    f->host_changed = false;
    set_status(f, "Loading\xC9");
    request_piece(f, f->editable ? 0 : (f->top > 40 ? f->top - 40 : 0), PIECE_LINES, FETCH_REPLACE);
    if (f->xfer < 0)
        set_status(f, "The file could not be requested: the tab is not connected.");
}

static void check_token(FileWin *f)
{
    if (f->check >= 0 || f->phase != FW_READY || f->offline || !f->w || f->virt)
        return;     /* a snapshot never refreshes itself: a check would remake it */
    f->check = proto_read(session_proto(), f->sid, index_of(f), f->path, 0, 0, PIECE_BYTES);
}

/* The editable text is complete: show it. */
static void finish_edit_load(FileWin *f)
{
    char state;
    quiet(f, true);
    state = HGetState(f->load);
    HLock(f->load);
    TESetText(*f->load, f->load_len, f->te);
    HSetState(f->load, state);
    (*f->te)->destRect = (*f->te)->viewRect;
    TECalText(f->te);
    TESetSelect(0, 0, f->te);
    quiet(f, false);
    scroll_px_to(f, f->keep_top);
    f->phase = FW_READY;
    f->dirty = false;
    set_title(f);
    f->status[0] = 0;
    sync_bar(f);
    InvalRect(&f->w->portRect);
    DisposeHandle(f->load);
    f->load = NULL;
}

static void edit_piece(FileWin *f, const FileXfer *x, const char *t, long n)
{
    long need;
    char s[64];
    if (f->next_line > 0 && strcmp(x->token, f->token) != 0) {
        strcpy(f->token, x->token);  /* changed while loading: start again */
        f->load_len = 0;
        f->next_line = 0;
        request_piece(f, 0, PIECE_LINES, FETCH_REPLACE);
        return;
    }
    need = f->load_len + n + 1;
    if (!f->load)
        f->load = NewHandle(need > 4096 ? need : 4096);
    else if (GetHandleSize(f->load) < need) {
        SetHandleSize(f->load, need * 2);   /* grow geometrically */
        if (GetHandleSize(f->load) < need)
            SetHandleSize(f->load, need);   /* tight heap: just enough */
    }
    if (!f->load || GetHandleSize(f->load) < need) {
        set_status(f, "Out of memory: the file could not be loaded.");
        f->phase = FW_READY;
        f->editable = false;
        return;
    }
    if (f->next_line > 0)
        (*f->load)[f->load_len++] = '\r';
    memcpy(*f->load + f->load_len, t, n);
    f->load_len += n;
    f->next_line = x->h_first + x->h_count;
    if (f->next_line < f->total && x->h_count > 0) {
        snprintf(s, sizeof(s), "Loading\xC9 %ld of %ld lines", f->next_line, f->total);
        set_status(f, s);
        request_piece(f, f->next_line, PIECE_LINES, FETCH_REPLACE);
    } else {
        finish_edit_load(f);
    }
}

static void on_piece(FileWin *f, const FileXfer *x, const char *t, long n)
{
    /* A snapshot window only ever shows one snapshot: a later piece that
       carries another token (the bridge took a new one) is not spliced in. */
    if (f->virt && f->phase == FW_READY && f->fetch != FETCH_NONE
            && strcmp(x->token, f->token) != 0) {
        f->fetch = FETCH_NONE;
        set_status(f, "The host changed since this snapshot. Revert to Saved refreshes.");
        return;
    }
    if (f->phase == FW_LOADING && f->next_line == 0 && (f->fetch == FETCH_REPLACE)) {
        /* The first piece decides how the window works. */
        strcpy(f->token, x->token);
        strcpy(f->mode, x->mode);
        f->total = x->h_total;
        f->editable = strcmp(x->mode, "e") == 0
                   && proto_files_mode(session_proto()) == FILES_RW;
        /* Read-only text is positioned by hand, never by TextEdit. */
        TEAutoView(f->editable, f->te);
        if (!f->editable) {
            (*f->te)->destRect.right = (*f->te)->destRect.left + WIDE_DEST;
            f->phase = FW_READY;
            f->slice_n = 0;
            ro_piece(f, x, t, n);
            InvalRect(&f->w->portRect);
            return;
        }
    }
    if (f->phase == FW_LOADING && f->editable) {
        f->fetch = FETCH_NONE;
        edit_piece(f, x, t, n);
        return;
    }
    if (!f->editable)
        ro_piece(f, x, t, n);
}

static void on_check(FileWin *f, const FileXfer *x)
{
    if (strcmp(x->token, f->token) == 0)
        return;
    if (!f->dirty && f->phase == FW_READY) {
        f->keep_top = f->editable ? scrolled_px(f) : 0;
        f->target = f->top;
        start_load(f);
        f->host_changed = true;
        set_status(f, "Changed on the host: reloading\xC9");
    } else {
        f->host_changed = true;
        draw_status(f);
    }
}

void filewin_event(short sid, EventKind k, const char *t, long n)
{
    const FileXfer *x = proto_xfer(session_proto());
    int slot = proto_xfer_slot(session_proto());
    FileWin *f;
    char s[160];
    if (!x || x->owner < 0 || x->owner >= MAX_FILEWINS)
        return;
    f = &gWins[x->owner];
    if (!f->w || f->sid != sid || (slot != f->xfer && slot != f->check))
        return;
    if (slot == f->check) {
        f->check = -1;
        if (k == EV_PIECE)
            on_check(f, x);
        return;
    }
    switch (k) {
    case EV_PIECE:
        f->xfer = -1;
        on_piece(f, x, t, n);
        if (f->host_changed && f->phase == FW_READY && !f->dirty)
            set_status(f, "Changed on the host: reloaded.");
        break;
    case EV_READ_FAILED:
        f->xfer = -1;
        f->fetch = FETCH_NONE;
        plain_reason(t, n, s, sizeof(s));
        if (f->phase == FW_LOADING && f->total == 0 && f->next_line == 0 && !f->offline
                && !starts(t, n, "The link was lost")) {
            f->open_failed = true;      /* filewin_deferred closes it */
            {
            char nm[64];
            snprintf(nm, sizeof(nm), "%.60s", f->label);
            snprintf(f->alert, sizeof(f->alert), "%s could not be opened. %.180s", nm, s);
        }
        } else {
            snprintf(f->status, sizeof(f->status), "Could not load: %.100s", s);
            draw_status(f);
            if (f->phase == FW_LOADING && !f->editable)
                f->phase = FW_READY;
        }
        break;
    case EV_SAVE_PROGRESS:
        f->blocks_done++;
        snprintf(s, sizeof(s), "Saving %d of %d\xC9", f->blocks_done + 1 > f->blocks
                 ? f->blocks : f->blocks_done + 1, f->blocks);
        set_status(f, s);
        break;
    case EV_SAVED:
        f->xfer = -1;
        f->phase = FW_READY;
        f->save_state = SAVE_OK;
        f->conflict = false;
        strncpy(f->token, t, sizeof(f->token) - 1);
        f->token[n < (long)sizeof(f->token) - 1 ? n : (long)sizeof(f->token) - 1] = 0;
        f->dirty = false;
        f->host_changed = false;
        set_title(f);
        set_status(f, "Saved.");
        break;
    case EV_SAVE_CONFLICT:
        f->conflict = true;
        set_status(f, "The file changed on the host.");
        break;
    case EV_SAVE_FAILED:
        f->xfer = -1;
        f->phase = FW_READY;
        f->save_state = SAVE_FAILED;
        f->conflict = false;
        plain_reason(t, n, s, sizeof(s));
        {
            char nm[64];
            snprintf(nm, sizeof(nm), "%.60s", f->label);
            snprintf(f->alert, sizeof(f->alert), "%s was not saved. %.180s", nm, s);
        }
        set_status(f, "Not saved.");
        break;
    default:
        break;
    }
}

/* ---- windows --------------------------------------------------------------- */

static void dispose(FileWin *f)
{
    Proto *p = session_proto();
    if (f->xfer >= 0)
        proto_xfer_cancel(p, f->xfer);
    if (f->check >= 0)
        proto_xfer_cancel(p, f->check);
    if (gTracking == f)
        gTracking = NULL;
    TEDispose(f->te);
    DisposeWindow(f->w);
    if (f->load)
        DisposeHandle(f->load);
    memset(f, 0, sizeof(*f));
    f->xfer = f->check = -1;
}

static void alert_text(const char *s)
{
    Str255 p;
    c2p(s, p);
    ParamText(p, "\p", "\p", "\p");
    Alert(kNoteAlert, NULL);
}

void filewin_open(Tab *t, const char *path)
{
    short i, slot = -1, open = 0;
    FileWin *f;
    Rect r;
    FontInfo fi;
    Str255 title;
    const char *slash;

    for (i = 0; i < MAX_FILEWINS; i++) {
        if (gWins[i].w) {
            open++;
            if (gWins[i].sid == t->sid && strcmp(gWins[i].path, path) == 0) {
                SelectWindow(gWins[i].w);
                if (gWins[i].virt)          /* a snapshot: open again takes a new one */
                    filewin_revert(gWins[i].w);
                return;
            }
        } else if (slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        alert_text("Six file windows are open. Close one first.");
        return;
    }
    if (!gBarUPP)
        gBarUPP = NewControlActionUPP(bar_action);
    f = &gWins[slot];
    memset(f, 0, sizeof(*f));
    f->xfer = f->check = -1;
    f->sid = t->sid;
    snprintf(f->project, sizeof(f->project), "%s", t->project);
    snprintf(f->path, sizeof(f->path), "%s", path);
    slash = strrchr(f->path, '/');
    f->name = slash ? slash + 1 : f->path;
    f->click_start = -1;
    f->virt = f->path[0] == ':';
    f->changes = strcmp(f->path, CHANGES_PATH) == 0;
    if (f->changes)
        strcpy(f->label, "Changes");
    else if (f->virt)
        snprintf(f->label, sizeof(f->label), "%.80s diff", f->name);
    else
        snprintf(f->label, sizeof(f->label), "%.90s", f->name);

    SetRect(&r, 16 + open * 16, 44 + open * 16, 16 + open * 16 + WIN_W, 44 + open * 16 + WIN_H);
    if (r.bottom > qd.screenBits.bounds.bottom - 4)
        OffsetRect(&r, 0, (short)(qd.screenBits.bounds.bottom - 4 - r.bottom));
    if (r.right > qd.screenBits.bounds.right - 4)
        OffsetRect(&r, (short)(qd.screenBits.bounds.right - 4 - r.right), 0);
    c2p(f->label, title);
    f->w = NewWindow(NULL, &r, title, false, zoomDocProc, (WindowPtr)-1, true, 0);
    if (!f->w) {
        alert_text("There is not enough memory for another window.");
        return;
    }
    SetPort(f->w);
    TextFont(MONACO);
    TextSize(FONT_SIZE);
    GetFontInfo(&fi);
    f->line_h = (short)(fi.ascent + fi.descent + fi.leading);
    layout(f);
    f->te = TENew(&f->view, &f->view);
    if (!f->te) {
        DisposeWindow(f->w);
        f->w = NULL;
        alert_text("There is not enough memory for another window.");
        return;
    }
    TEAutoView(true, f->te);
    r = f->w->portRect;
    SetRect(&r, r.right - 15, r.top - 1, r.right + 1, r.bottom - 14);
    f->bar = NewControl(f->w, &r, "\p", true, 0, 0, 0, scrollBarProc, 0);
    set_title(f);
    ShowWindow(f->w);
    SelectWindow(f->w);
    start_load(f);
}

Boolean filewin_is(WindowPtr w)
{
    return find(w) != NULL;
}

Boolean filewin_update(WindowPtr w)
{
    FileWin *f = find(w);
    GrafPtr save;
    if (!f)
        return false;
    GetPort(&save);
    SetPort(w);
    BeginUpdate(w);
    EraseRect(&w->portRect);
    TEUpdate(&f->view, f->te);
    DrawControls(w);
    DrawGrowIcon(w);
    draw_status(f);
    EndUpdate(w);
    SetPort(save);
    return true;
}

void filewin_activate(WindowPtr w, Boolean on)
{
    FileWin *f = find(w);
    if (!f)
        return;
    SetPort(w);
    if (on) {
        TEActivate(f->te);
        HiliteControl(f->bar, 0);
        check_token(f);
    } else {
        TEDeactivate(f->te);
        HiliteControl(f->bar, 255);
    }
    DrawGrowIcon(w);
}

/* The :changes list: a click selects the whole line, a second click on it
   (or Return) opens that file's diff. Lines that name no file only select. */
static void open_selected_diff(FileWin *f)
{
    char *t = (char *)*(*f->te)->hText;
    long a = (*f->te)->selStart, b = (*f->te)->selEnd;
    char path[200], full[256];
    Tab *tab = tabs_by_sid(f->sid);
    if (f->offline || f->phase != FW_READY) {
        SysBeep(1);
        return;
    }
    if (!tab || b <= a || !files_changes_path(t + a, b - a, path, sizeof(path))) {
        SysBeep(1);
        return;
    }
    snprintf(full, sizeof(full), "%s%s", DIFF_PREFIX, path);
    filewin_open(tab, full);
}

/* 0x02F0 is the DoubleTime low-memory global; GetDblTime is not in the
   Retro68 library. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
static unsigned long dbl_time(void)
{
    return (unsigned long)*(volatile long *)0x02F0;
}
#pragma GCC diagnostic pop

static void changes_click(FileWin *f, Point p)
{
    TEHandle te = f->te;
    long off = TEGetOffset(p, te), a, b, len = (*te)->teLength;
    char *t;
    if (len == 0)
        return;
    t = (char *)*(*te)->hText;
    if (off > len)
        off = len;
    a = off;
    while (a > 0 && t[a - 1] != '\r')
        a--;
    b = off;
    while (b < len && t[b] != '\r')
        b++;
    TESetSelect(a, b, te);
    if (f->click_start == a && TickCount() - f->click_tick <= dbl_time()) {
        f->click_start = -1;
        open_selected_diff(f);
        return;
    }
    f->click_start = a;
    f->click_tick = TickCount();
}

void filewin_click(WindowPtr w, EventRecord *ev)
{
    FileWin *f = find(w);
    Point p = ev->where;
    ControlHandle c;
    short part;
    if (!f)
        return;
    SetPort(w);
    GlobalToLocal(&p);
    part = FindControl(p, w, &c);
    if (c == f->bar && part) {
        gTracking = f;
        if (part == inThumb) {
            TrackControl(c, p, NULL);
            if (f->editable)
                scroll_px_to(f, (long)GetControlValue(c) * SCROLL_UNIT);
            else
                ro_set_top(f, GetControlValue(c));
        } else {
            TrackControl(c, p, gBarUPP);
        }
        gTracking = NULL;
        return;
    }
    if (PtInRect(p, &f->view) && f->changes && f->phase == FW_READY) {
        changes_click(f, p);
        return;
    }
    if (PtInRect(p, &f->view)) {
        TEClick(p, (ev->modifiers & shiftKey) != 0, f->te);
        if (f->editable)
            sync_bar(f);
    }
}

void filewin_key(WindowPtr w, EventRecord *ev)
{
    FileWin *f = find(w);
    char ch = (char)(ev->message & charCodeMask);
    Boolean arrow = ch >= 0x1C && ch <= 0x1F;
    if (!f)
        return;
    SetPort(w);
    if (f->changes && f->phase == FW_READY && (ch == '\r' || ch == 3)) {
        open_selected_diff(f);
        return;
    }
    if (!f->editable || f->phase != FW_READY) {
        if (!f->editable && f->phase == FW_READY && (ch == 0x1E || ch == 0x1F))
            ro_set_top(f, f->top + (ch == 0x1E ? -1 : 1));
        else if (!arrow)
            SysBeep(1);
        return;
    }
    if (ch == 3)
        ch = '\r';              /* keypad Enter */
    if (!arrow && ch != '\b' && ch != '\t' && ch != '\r'
            && ((unsigned char)ch < 0x20 || ch == 0x7F)) {
        SysBeep(1);             /* Home, PgDn, Help, Del... would land in the file */
        return;
    }
    if (!arrow && ch != '\b' && (*f->te)->teLength - ((*f->te)->selEnd - (*f->te)->selStart)
            >= TEXT_LIMIT) {
        SysBeep(1);
        return;
    }
    TEKey(ch, f->te);
    if (!arrow && !f->dirty) {
        f->dirty = true;
        set_title(f);
    }
    sync_bar(f);
}

static void resized(FileWin *f)
{
    Rect r;
    long keep = f->editable ? scrolled_px(f) : 0;
    layout(f);
    (*f->te)->viewRect = f->view;
    (*f->te)->destRect = f->view;
    if (!f->editable)
        (*f->te)->destRect.right = f->view.left + WIDE_DEST;
    TECalText(f->te);
    r = f->w->portRect;
    MoveControl(f->bar, r.right - 15, r.top - 1);
    SizeControl(f->bar, 16, (short)(r.bottom - 14 - r.top + 1));
    if (f->editable) {
        scroll_px_to(f, keep);
        sync_bar(f);
    } else {
        OffsetRect(&(*f->te)->destRect, 0, (short)-((f->top - f->slice_first) * f->line_h));
        ro_set_top(f, f->top);
    }
    InvalRect(&f->w->portRect);
}

void filewin_grow(WindowPtr w, Point where)
{
    FileWin *f = find(w);
    Rect limits;
    long sz;
    if (!f)
        return;
    SetRect(&limits, 200, 100, qd.screenBits.bounds.right, qd.screenBits.bounds.bottom);
    sz = GrowWindow(w, where, &limits);
    if (sz) {
        SizeWindow(w, LoWord(sz), HiWord(sz), true);
        SetPort(w);
        resized(f);
    }
}

void filewin_zoom(WindowPtr w, short part, Point where)
{
    FileWin *f = find(w);
    if (!f || !TrackBox(w, where, part))
        return;
    SetPort(w);
    EraseRect(&w->portRect);
    ZoomWindow(w, part, false);
    resized(f);
}

void filewin_idle(void)
{
    FileWin *f = find(FrontWindow());
    if (f && f->editable)
        TEIdle(f->te);
}

/* ---- saving --------------------------------------------------------------- */

static Boolean start_save(FileWin *f)
{
    char state;
    Handle h;
    long len;
    if (!f->editable || f->phase != FW_READY || f->offline
            || proto_files_mode(session_proto()) != FILES_RW)
        return false;
    h = (Handle)TEGetText(f->te);
    len = (*f->te)->teLength;
    state = HGetState(h);
    HLock(h);
    f->xfer = proto_save(session_proto(), f->sid, index_of(f), f->path, f->token, *h, len);
    HSetState(h, state);
    if (f->xfer < 0) {
        set_status(f, "The save could not start: the tab is not ready.");
        return false;
    }
    f->blocks = (short)session_proto()->xfer[f->xfer].nblocks;
    f->blocks_done = 0;
    f->phase = FW_SAVING;
    f->save_state = SAVE_RUNNING;
    f->alert[0] = 0;
    set_status(f, "Saving\xC9");
    return true;
}

/* Asks Cancel or Replace for a save the host file outran. */
static void resolve_conflict(FileWin *f)
{
    Str255 p;
    f->conflict = false;
    c2p(f->name, p);
    ParamText(p, "\p", "\p", "\p");
    if (CautionAlert(kConflictAlert, NULL) == 2 && proto_save_force(session_proto(), f->xfer)) {
        set_status(f, "Replacing\xC9");
        return;
    }
    if (f->xfer >= 0)
        proto_xfer_cancel(session_proto(), f->xfer);
    f->xfer = -1;
    f->phase = FW_READY;
    f->save_state = SAVE_CANCELLED;
    f->host_changed = true;
    set_status(f, "Not saved: the file changed on the host.");
}

/* Saves and waits, for the close and disconnect paths. True when saved. */
static Boolean save_and_wait(FileWin *f)
{
    EventRecord ev;
    if (!start_save(f))
        return false;
    while (f->save_state == SAVE_RUNNING) {
        session_pump();
        if (f->conflict)
            resolve_conflict(f);
        if (WaitNextEvent(updateMask | activMask, &ev, 1, NULL)) {
            if (ev.what == updateEvt)
                app_update((WindowPtr)ev.message);
        }
        if (session_link_state() == ST_IDLE && f->save_state == SAVE_RUNNING)
            f->save_state = SAVE_FAILED;
    }
    if (f->save_state == SAVE_FAILED && f->alert[0]) {
        alert_text(f->alert);
        f->alert[0] = 0;
    }
    return f->save_state == SAVE_OK;
}

void filewin_save(WindowPtr w)
{
    FileWin *f = find(w);
    if (f && !start_save(f))
        SysBeep(1);
}

void filewin_revert(WindowPtr w)
{
    FileWin *f = find(w);
    Str255 p;
    if (!f || !filewin_can_revert(w))
        return;
    if (f->virt) {              /* take a new snapshot, from the top */
        f->click_start = -1;
        f->top = f->target = 0;
        f->slice_n = 0;
        f->keep_top = 0;
        start_load(f);
        return;
    }
    c2p(f->name, p);
    ParamText(p, "\p", "\p", "\p");
    if (CautionAlert(kRevertAlert, NULL) != 1)
        return;
    f->keep_top = scrolled_px(f);
    start_load(f);
}

Boolean filewin_can_save(WindowPtr w)
{
    FileWin *f = find(w);
    return f && f->editable && f->dirty && f->phase == FW_READY && !f->offline
        && proto_files_mode(session_proto()) == FILES_RW;
}

Boolean filewin_can_revert(WindowPtr w)
{
    FileWin *f = find(w);
    return f && f->phase == FW_READY && !f->offline && ((f->editable && f->dirty) || f->virt);
}

Boolean filewin_editable(WindowPtr w)
{
    FileWin *f = find(w);
    return f && f->editable && f->phase == FW_READY;
}

short filewin_sid(WindowPtr w)
{
    FileWin *f = find(w);
    return f ? f->sid : 0;
}

/* Save, Don't Save or Cancel for one dirty window. False on Cancel. */
static Boolean settle_one(FileWin *f)
{
    Str255 p;
    short hit;
    if (!f->dirty)
        return true;
    if (f->hidden) {
        ShowWindow(f->w);
        f->hidden = false;
    }
    SelectWindow(f->w);
    filewin_update(f->w);
    c2p(f->name, p);
    ParamText(p, "\p", "\p", "\p");
    if (f->offline || !f->editable || f->phase != FW_READY
            || proto_files_mode(session_proto()) != FILES_RW) {
        hit = CautionAlert(kCantSaveAlert, NULL);   /* 1 Don't Save, 2 Cancel */
        return hit == 1;
    }
    hit = CautionAlert(kSaveChangesAlert, NULL);    /* 1 Save, 2 Cancel, 3 Don't Save */
    if (hit == 2)
        return false;
    if (hit == 3)
        return true;
    return save_and_wait(f);
}

Boolean filewin_close(WindowPtr w)
{
    FileWin *f = find(w);
    if (!f)
        return false;
    if (!settle_one(f))
        return false;
    dispose(f);
    return true;
}

Boolean filewin_settle(short sid, Boolean closing)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++) {
        FileWin *f = &gWins[i];
        if (!f->w || (sid && f->sid != sid))
            continue;
        if (!settle_one(f))
            return false;
    }
    if (closing)
        for (i = 0; i < MAX_FILEWINS; i++)
            if (gWins[i].w && (!sid || gWins[i].sid == sid))
                dispose(&gWins[i]);
    return true;
}

void filewin_show_tab(short sid)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++) {
        FileWin *f = &gWins[i];
        if (!f->w)
            continue;
        if (f->sid == sid && f->hidden) {
            ShowWindow(f->w);
            f->hidden = false;
        } else if (f->sid != sid && !f->hidden) {
            HideWindow(f->w);
            f->hidden = true;
        }
    }
}

/* ---- glue ----------------------------------------------------------------- */

void filewin_turn_ended(short sid)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++)
        if (gWins[i].w && gWins[i].sid == sid)
            check_token(&gWins[i]);
}

void filewin_online(short sid, Boolean on)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++) {
        FileWin *f = &gWins[i];
        if (!f->w || (sid && f->sid != sid))
            continue;
        if (on) {
            if (!f->offline)
                continue;
            f->offline = false;
            f->status[0] = 0;
            draw_status(f);
            if (f->phase == FW_LOADING || (f->phase == FW_READY && !f->editable && f->slice_n == 0))
                start_load(f);
            else
                check_token(f);
        } else {
            /* The core already failed the transfers; forget their slots. */
            f->xfer = f->check = -1;
            f->fetch = FETCH_NONE;
            if (f->phase == FW_SAVING) {
                f->phase = FW_READY;
                f->save_state = SAVE_FAILED;
            }
            f->offline = true;
            f->status[0] = 0;
            draw_status(f);
        }
    }
}

void filewin_deferred(void)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++) {
        FileWin *f = &gWins[i];
        if (!f->w)
            continue;
        if (f->open_failed) {
            char msg[256];
            strcpy(msg, f->alert);
            dispose(f);
            alert_text(msg);
            continue;
        }
        if (f->conflict)
            resolve_conflict(f);
        if (f->save_state == SAVE_FAILED && f->alert[0]) {
            char msg[256];
            strcpy(msg, f->alert);
            f->alert[0] = 0;
            f->save_state = SAVE_IDLE;
            alert_text(msg);
        }
    }
}

short filewin_count(void)
{
    short i, n = 0;
    for (i = 0; i < MAX_FILEWINS; i++)
        if (gWins[i].w)
            n++;
    return n;
}

WindowPtr filewin_at(short k)
{
    short i;
    for (i = 0; i < MAX_FILEWINS; i++)
        if (gWins[i].w && k-- == 0)
            return gWins[i].w;
    return NULL;
}

/* ---- Edit menu ------------------------------------------------------------- */

/* Copies the selection to the desk scrap as plain text. TEToScrap is glue
   that Retro68 lacks, so the scrap is written directly. */
static void copy_selection(TEHandle te)
{
    short a = (*te)->selStart, b = (*te)->selEnd;
    char state;
    if (b <= a)
        return;
    ZeroScrap();
    state = HGetState((*te)->hText);
    HLock((*te)->hText);
    PutScrap(b - a, 'TEXT', *(*te)->hText + a);
    HSetState((*te)->hText, state);
}

void filewin_edit(WindowPtr w, short item)
{
    FileWin *f = find(w);
    long offset, len;
    Handle h;
    if (!f)
        return;
    SetPort(w);
    switch (item) {
    case kCopyItem:
        copy_selection(f->te);
        return;
    case kSelectAllItem:
        TESetSelect(0, 32767, f->te);
        return;
    }
    if (!f->editable || f->phase != FW_READY) {
        SysBeep(1);
        return;
    }
    switch (item) {
    case kCutItem:
        if ((*f->te)->selEnd <= (*f->te)->selStart)
            return;
        copy_selection(f->te);
        TEDelete(f->te);
        break;
    case kPasteItem:
        h = NewHandle(0);
        if (!h)
            return;
        len = GetScrap(h, 'TEXT', &offset);
        if (len <= 0
                || (*f->te)->teLength - ((*f->te)->selEnd - (*f->te)->selStart) + len > TEXT_LIMIT) {
            if (len > 0)
                SysBeep(1);
            DisposeHandle(h);
            return;
        }
        TEDelete(f->te);
        HLock(h);
        {
            /* No control characters into a host file: LF becomes CR, the
               rest are dropped (the bridge refuses them on save). */
            char *c = *h;
            long i, k = 0;
            for (i = 0; i < len; i++) {
                unsigned char b = (unsigned char)c[i];
                if (b == '\n')
                    b = '\r';
                if (b >= 0x20 ? b != 0x7F : (b == '\t' || b == '\r'))
                    c[k++] = (char)b;
            }
            len = k;
        }
        TEInsert(*h, len, f->te);
        DisposeHandle(h);
        break;
    case kClearItem:
        if ((*f->te)->selEnd <= (*f->te)->selStart)
            return;
        TEDelete(f->te);
        break;
    default:
        return;
    }
    if (!f->dirty) {
        f->dirty = true;
        set_title(f);
    }
    sync_bar(f);
}
