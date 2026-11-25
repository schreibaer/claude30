#include "fold.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MR_BULLET     '\xA5'
#define MR_ELLIPSIS   '\xC9'
#define MARKER_MAX    160

void fold_init(FoldSet *fs)
{
    memset(fs, 0, sizeof(*fs));
    fs->cur = -1;
    fs->pend = -1;
    fs->next_id = 1;
}

void fold_free(FoldSet *fs)
{
    int i;
    for (i = 0; i < fs->n; i++)
        free(fs->f[i].body);
    free(fs->f);
    fold_init(fs);
}

static void reset_scan(FoldSet *fs)
{
    fs->kind = FK_NONE;
    fs->count = 0;
    fs->start = 0;
    fs->pend = -1;
    fs->cur = -1;
    fs->nnames = 0;
    fs->more_names = 0;
}

static long meas(const FoldOps *o, int kind, const char *s, long n)
{
    return o->measure ? o->measure(o->ctx, kind, s, n) : n;
}

/* ---- marker text -------------------------------------------------------- */

/* "[+] Code, 47 lines: first line", with the last part cut to fit. */
static long make_marker(const Fold *f, int open, const FoldOps *o, char *out)
{
    char head[48];
    long hl, tl = (long)strlen(f->tail), k;
    if (f->kind == FK_CODE)
        snprintf(head, sizeof(head), "[%c] Code, %ld lines", open ? '-' : '+', f->lines);
    else
        snprintf(head, sizeof(head), "[%c] %ld tools", open ? '-' : '+', f->lines);
    hl = (long)strlen(head);
    memcpy(out, head, hl);
    if (tl == 0)
        return hl;
    for (k = tl; k > 0; k--) {
        long m = hl;
        if (m + 2 + k + 1 >= MARKER_MAX)
            continue;
        out[m++] = ':';
        out[m++] = ' ';
        memcpy(out + m, f->tail, k);
        m += k;
        if (k < tl)
            out[m++] = MR_ELLIPSIS;
        if (!o->width || meas(o, f->kind, out, m) <= o->width)
            return m;
    }
    return hl;
}

/* The first line with something on it, for a code fold's marker. */
static void code_tail(const char *body, long n, char *out)
{
    long i = 0, k = 0;
    out[0] = 0;
    while (i < n) {
        long e = i, s;
        while (e < n && body[e] != '\r')
            e++;
        /* Past the line number batch.c puts in front ("  1 "), then the
           code's own indent. */
        s = i;
        while (s < e && body[s] == ' ')
            s++;
        while (s < e && body[s] >= '0' && body[s] <= '9')
            s++;
        while (s < e && body[s] == ' ')
            s++;
        if (s < e) {
            while (s < e && k < FOLD_TAIL - 1) {
                char c = body[s++];
                out[k++] = (unsigned char)c < 0x20 ? ' ' : c;
            }
            break;
        }
        i = e + 1;
    }
    out[k] = 0;
}

static void tally_tool(FoldSet *fs, const char *line, long n)
{
    long i = 2, e;
    char name[16];
    int k;
    if (n < 3)
        return;
    e = i;
    while (e < n && line[e] != ' ' && e - i < 15)
        e++;
    memcpy(name, line + i, e - i);
    name[e - i] = 0;
    for (k = 0; k < fs->nnames; k++)
        if (strcmp(fs->names[k], name) == 0) {
            fs->ncount[k]++;
            return;
        }
    if (fs->nnames < FOLD_NAMES) {
        strcpy(fs->names[fs->nnames], name);
        fs->ncount[fs->nnames++] = 1;
    } else {
        fs->more_names = 1;
    }
}

static void tool_tail(const FoldSet *fs, char *out)
{
    long m = 0;
    int k;
    for (k = 0; k < fs->nnames; k++) {
        char part[40];
        long pl = fs->ncount[k] > 1
            ? snprintf(part, sizeof(part), "%s%s %ld", k ? ", " : "", fs->names[k], fs->ncount[k])
            : snprintf(part, sizeof(part), "%s%s", k ? ", " : "", fs->names[k]);
        if (m + pl >= FOLD_TAIL - 3)
            break;
        memcpy(out + m, part, pl);
        m += pl;
    }
    if (fs->more_names && m + 3 < FOLD_TAIL) {
        out[m++] = ',';
        out[m++] = ' ';
        out[m++] = MR_ELLIPSIS;
    }
    out[m] = 0;
}

/* ---- storage ------------------------------------------------------------ */

static Fold *add_slot(FoldSet *fs)
{
    if (fs->n == fs->cap) {
        int ncap = fs->cap ? fs->cap * 2 : 16;
        Fold *nf;
        if (ncap > FOLD_MAX)
            ncap = FOLD_MAX;
        nf = (Fold *)realloc(fs->f, ncap * sizeof(Fold));
        if (!nf)
            return NULL;
        fs->f = nf;
        fs->cap = ncap;
    }
    return &fs->f[fs->n];
}

static int body_reserve(Fold *f, long need)
{
    if (need > f->body_cap) {
        long ncap = f->body_cap * 2 > need ? f->body_cap * 2 : need;
        char *nb;
        if (ncap > FOLD_MAX_BODY)
            ncap = FOLD_MAX_BODY;
        nb = (char *)realloc(f->body, ncap);
        if (!nb)
            return 0;
        f->body = nb;
        f->body_cap = ncap;
    }
    return 1;
}

/* ---- reading the stream ------------------------------------------------- */

static int reached(const FoldSet *fs)
{
    return fs->kind == FK_CODE ? fs->count > FOLD_CODE_LINES : fs->count >= FOLD_TOOL_RUN;
}

/* The block or run has grown long enough: its text so far becomes a fold.
   Returns how much the transcript got shorter or longer. */
static long create(FoldSet *fs, const FoldOps *o)
{
    long start = fs->start, len = fs->pend - fs->start, mlen;
    char marker[MARKER_MAX];
    Fold *f;
    char *body;
    if (len <= 0 || len > FOLD_MAX_BODY || fs->n >= FOLD_MAX || fs->bytes + len > FOLD_MAX_BYTES) {
        fs->cur = -2;
        return 0;
    }
    f = add_slot(fs);
    body = (char *)malloc(len);
    if (!f || !body) {
        free(body);
        fs->cur = -2;
        return 0;
    }
    memset(f, 0, sizeof(*f));
    o->read(o->ctx, start, len, body);
    f->kind = fs->kind;
    f->id = fs->next_id++;
    f->off = start;
    f->lines = fs->count;
    f->growing = 1;
    f->body = body;
    f->body_len = f->body_cap = len;
    if (f->kind == FK_CODE)
        code_tail(body, len, f->tail);
    else
        tool_tail(fs, f->tail);
    mlen = make_marker(f, 0, o, marker);
    f->len = mlen;
    fs->bytes += len;
    fs->cur = fs->n++;
    o->replace(o->ctx, start, len, marker, mlen, f->kind);
    return mlen - len;
}

/* More lines of a growing fold's block arrived after its marker. */
static long extend(FoldSet *fs, const FoldOps *o)
{
    Fold *f = &fs->f[fs->cur];
    long mend = f->off + f->len, add = fs->pend - mend, mlen;
    char marker[MARKER_MAX];
    if (add <= 0)
        return 0;
    if (f->body_len + add > FOLD_MAX_BODY || fs->bytes + add > FOLD_MAX_BYTES
            || !body_reserve(f, f->body_len + add)) {
        /* Too much to keep: the fold stops here and the rest of the block
           stays in the transcript as plain lines after it. */
        f->growing = 0;
        fs->cur = -2;
        return 0;
    }
    o->read(o->ctx, mend, add, f->body + f->body_len);
    f->body_len += add;
    fs->bytes += add;
    f->lines = fs->count;
    if (f->kind == FK_TOOL)
        tool_tail(fs, f->tail);
    mlen = make_marker(f, 0, o, marker);
    o->replace(o->ctx, f->off, f->len + add, marker, mlen, f->kind);
    add = mlen - (f->len + add);
    f->len = mlen;
    return add;
}

/* Brings the transcript up to date with the lines read since the last call. */
static void flush(FoldSet *fs, const FoldOps *o, long *shift)
{
    if (fs->kind != FK_NONE && fs->pend >= 0) {
        if (fs->cur >= 0)
            *shift += extend(fs, o);
        else if (fs->cur == -1 && reached(fs))
            *shift += create(fs, o);
    }
    fs->pend = -1;
}

/* The block or run is over. */
static void finalize(FoldSet *fs)
{
    if (fs->cur >= 0)
        fs->f[fs->cur].growing = 0;
    reset_scan(fs);
}

void fold_append(FoldSet *fs, const FoldOps *o, RunStyle style,
                 const char *text, long len, long base)
{
    long shift = 0, i = 0;
    int kind = style == RS_CODE ? FK_CODE : style == RS_TOOL ? FK_TOOL : FK_NONE;
    fs->pend = -1;
    if (len <= 0)
        return;
    if (kind == FK_NONE) {
        flush(fs, o, &shift);
        finalize(fs);
        return;
    }
    for (;;) {
        long j = i;
        while (j < len && text[j] != '\r')
            j++;
        if (j > i) {
            if (i == 0 && fs->kind == kind) {
                /* more of the line the last run ended in */
                fs->pend = base + shift + j;
            } else {
                int k = kind;
                if (kind == FK_TOOL && text[i] != MR_BULLET)
                    k = FK_NONE;       /* a permission line or other text */
                if (k == FK_NONE) {
                    flush(fs, o, &shift);
                    finalize(fs);
                } else {
                    if (fs->kind != k) {
                        flush(fs, o, &shift);
                        finalize(fs);
                        fs->kind = k;
                        fs->start = base + shift + i;
                    }
                    fs->count++;
                    if (k == FK_TOOL)
                        tally_tool(fs, text + i, j - i);
                    fs->pend = base + shift + j;
                }
            }
        }
        if (j >= len)
            break;
        i = j + 1;
    }
    flush(fs, o, &shift);
}

/* ---- opening and closing ------------------------------------------------ */

static void move_after(FoldSet *fs, int idx, long delta)
{
    int j;
    for (j = 0; j < fs->n; j++)
        if (j != idx && fs->f[j].off > fs->f[idx].off)
            fs->f[j].off += delta;
    if (fs->kind != FK_NONE && fs->start > fs->f[idx].off)
        fs->start += delta;
}

int fold_toggle(FoldSet *fs, const FoldOps *o, int idx)
{
    Fold *f;
    char marker[MARKER_MAX];
    long mlen, oldlen, delta;
    if (idx < 0 || idx >= fs->n)
        return FT_NOTHING;
    f = &fs->f[idx];
    if (f->growing)
        return FT_BUSY;
    if (!f->open) {
        long tl;
        char *tmp;
        mlen = make_marker(f, 1, o, marker);
        tl = mlen + 1 + f->body_len;
        tmp = (char *)malloc(tl);
        if (!tmp)
            return FT_MEMORY;
        memcpy(tmp, marker, mlen);
        tmp[mlen] = '\r';
        memcpy(tmp + mlen + 1, f->body, f->body_len);
        oldlen = f->len;
        o->replace(o->ctx, f->off, oldlen, tmp, tl, f->kind);
        free(tmp);
        delta = tl - oldlen;
        f->open_len = 1 + f->body_len;
        fs->bytes -= f->body_len;
        free(f->body);
        f->body = NULL;
        f->body_len = f->body_cap = 0;
        f->len = mlen;
        f->open = 1;
        move_after(fs, idx, delta);
        return FT_OPENED;
    } else {
        long blen = f->open_len - 1;
        char *body;
        if (blen > FOLD_MAX_BODY || fs->bytes + blen > FOLD_MAX_BYTES)
            return FT_TOO_BIG;
        body = (char *)malloc(blen > 0 ? blen : 1);
        if (!body)
            return FT_MEMORY;
        o->read(o->ctx, f->off + f->len + 1, blen, body);
        f->body = body;
        f->body_len = f->body_cap = blen;
        fs->bytes += blen;
        mlen = make_marker(f, 0, o, marker);
        oldlen = f->len + f->open_len;
        o->replace(o->ctx, f->off, oldlen, marker, mlen, f->kind);
        delta = mlen - oldlen;
        f->len = mlen;
        f->open = 0;
        f->open_len = 0;
        move_after(fs, idx, delta);
        return FT_CLOSED;
    }
}

/* ---- finding folds ------------------------------------------------------ */

int fold_marker_at(const FoldSet *fs, long pos)
{
    int i;
    for (i = 0; i < fs->n; i++)
        if (pos >= fs->f[i].off && pos <= fs->f[i].off + fs->f[i].len)
            return i;
    return -1;
}

int fold_containing(const FoldSet *fs, long pos)
{
    int i;
    for (i = 0; i < fs->n; i++) {
        const Fold *f = &fs->f[i];
        if (pos >= f->off && pos <= f->off + f->len + (f->open ? f->open_len : 0))
            return i;
    }
    return -1;
}

int fold_last_in(const FoldSet *fs, long from, long to)
{
    int i;
    for (i = fs->n - 1; i >= 0; i--)
        if (fs->f[i].off >= from && fs->f[i].off <= to)
            return i;
    return -1;
}

int fold_by_id(const FoldSet *fs, long id)
{
    int i;
    for (i = 0; i < fs->n; i++)
        if (fs->f[i].id == id)
            return i;
    return -1;
}

/* ---- the transcript changed under the folds ----------------------------- */

void fold_shift(FoldSet *fs, long off, long delta)
{
    int i;
    for (i = 0; i < fs->n; i++)
        if (fs->f[i].off > off)
            fs->f[i].off += delta;
    if (fs->kind != FK_NONE && fs->start > off)
        fs->start += delta;
}

int fold_log(const FoldSet *fs, long cut, const FoldLog *lg)
{
    long pos = 0;
    int i, r;
    for (i = 0; i < fs->n; i++) {
        const Fold *f = &fs->f[i];
        if (f->off + f->len > cut)
            break;
        if (f->open)
            continue;               /* its lines are in the transcript already */
        if (f->off > pos && (r = lg->range(lg->ctx, pos, f->off - pos)) != 0)
            return r;
        if (f->body_len > 0 && (r = lg->bytes(lg->ctx, f->body, f->body_len)) != 0)
            return r;
        pos = f->off + f->len;
    }
    if (cut > pos)
        return lg->range(lg->ctx, pos, cut - pos);
    return 0;
}

void fold_cut(FoldSet *fs, long cut)
{
    int i, k = 0, cur = fs->cur;
    if (cut <= 0)
        return;
    for (i = 0; i < fs->n; i++) {
        Fold *f = &fs->f[i];
        if (f->off < cut) {
            if (f->body)
                fs->bytes -= f->body_len;
            free(f->body);
            if (i == fs->cur)
                cur = -3;
            continue;
        }
        f->off -= cut;
        if (i == fs->cur)
            cur = k;
        fs->f[k++] = *f;
    }
    fs->n = k;
    if (fs->kind != FK_NONE) {
        if (cur == -3 || fs->start < cut)
            reset_scan(fs);
        else {
            fs->start -= cut;
            fs->cur = cur;
        }
    }
}
