#include "files.h"
#include "crc32.h"
#include <stdlib.h>
#include <string.h>

enum {
    RD_WAIT,        /* G sent, waiting for H */
    RD_BODY,        /* H seen: B lines until Z */
    SV_WAIT_F,      /* F sent, waiting for K 0 */
    SV_SEND,        /* the current block is due (proto_files_step sends it) */
    SV_WAIT_BLOCK,  /* block sent, waiting for K or N resend */
    SV_WAIT_COMMIT, /* S. sent */
    SV_CONFLICT     /* the file changed on the host: the shell decides */
};

static const char kNoisyRead[] = "The link is too noisy to read this file.";
static const char kNoisySave[] = "The link is too noisy to save this file.";

/* ---- small helpers -------------------------------------------------------- */

static void sendn(Proto *p, const char *s, long n)
{
    if (n > 0) {
        p->last_tx = p->now;
        p->send(p->ctx, s, n);
    }
}

static void sends(Proto *p, const char *s)
{
    sendn(p, s, (long)strlen(s));
}

static char *put_long(char *o, unsigned long v)
{
    char tmp[12];
    int k = 0;
    do {
        tmp[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k)
        *o++ = tmp[--k];
    return o;
}

static char *put_hex(char *o, unsigned long v)
{
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; i--)
        *o++ = hex[(v >> (i * 4)) & 15];
    return o;
}

static char *put_str(char *o, const char *s)
{
    long n = (long)strlen(s);
    memcpy(o, s, n);
    return o + n;
}

/* The next space-separated word of t[*i..n): start and length. */
static long word(const char *t, long n, long *i, const char **w)
{
    long s;
    while (*i < n && t[*i] == ' ')
        (*i)++;
    s = *i;
    while (*i < n && t[*i] != ' ')
        (*i)++;
    *w = t + s;
    return *i - s;
}

static int word_long(const char *t, long n, long *i, long *out)
{
    const char *w;
    long k = word(t, n, i, &w), j, v = 0;
    if (k == 0 || k > 9)
        return 0;
    for (j = 0; j < k; j++) {
        if (w[j] < '0' || w[j] > '9')
            return 0;
        v = v * 10 + (w[j] - '0');
    }
    *out = v;
    return 1;
}

static int word_hex(const char *t, long n, long *i, unsigned long *out)
{
    const char *w;
    long k = word(t, n, i, &w), j;
    unsigned long v = 0;
    if (k == 0 || k > 8)
        return 0;
    for (j = 0; j < k; j++) {
        char c = w[j];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return 0;
        v = (v << 4) | (unsigned long)d;
    }
    *out = v;
    return 1;
}

static int is(const char *w, long k, const char *s)
{
    return k == (long)strlen(s) && memcmp(w, s, k) == 0;
}

static void copy_word(char *dst, long cap, const char *w, long k)
{
    if (k > cap - 1)
        k = cap - 1;
    memcpy(dst, w, k);
    dst[k] = 0;
}

/* ---- the transfer table -------------------------------------------------- */

static void emit_x(Proto *p, int slot, EventKind k, const char *t, long n)
{
    int save = p->cur_xfer;
    p->cur_xfer = slot;
    p->on_event(p->ctx, p->xfer[slot].sid, k, t ? t : "", t ? n : 0);
    p->cur_xfer = save;
}

static void free_x(Proto *p, int slot)
{
    FileXfer *x = &p->xfer[slot];
    int i;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].rx == slot)
            p->sess[i].rx = -1;
    free(x->text);
    x->text = NULL;
    x->tlen = 0;
    x->kind = XF_NONE;
    x->len = 0;
}

/* Reports a failure and frees the transfer. */
static void fail_x(Proto *p, int slot, const char *why, long n)
{
    EventKind k = p->xfer[slot].kind == XF_READ ? EV_READ_FAILED : EV_SAVE_FAILED;
    emit_x(p, slot, k, why, n);
    if (p->xfer[slot].kind != XF_NONE)
        free_x(p, slot);
}

static int new_slot(Proto *p, int sid, long owner, const char *path)
{
    int i;
    long k = (long)strlen(path);
    if (p->state != ST_ONLINE || sid < 1 || sid > MAX_SESS
            || p->sess[sid].state == SS_NONE || p->sess[sid].state == SS_CLOSING
            || k >= (long)sizeof(p->xfer[0].path) || p->files == FILES_OFF)
        return -1;
    for (i = 0; i < MAX_XFER; i++) {
        FileXfer *x = &p->xfer[i];
        if (x->kind == XF_NONE) {
            char *buf = x->buf;
            long cap = x->cap;
            memset(x, 0, sizeof(*x));
            x->buf = buf;
            x->cap = cap;
            x->sid = sid;
            x->owner = owner;
            memcpy(x->path, path, k + 1);
            return i;
        }
    }
    return -1;
}

static long next_num(Proto *p)
{
    long n = p->next_num++;
    if (p->next_num > 99999)
        p->next_num = 1;
    return n;
}

static int find_x(Proto *p, int sid, long num)
{
    int i;
    for (i = 0; i < MAX_XFER; i++)
        if (p->xfer[i].kind != XF_NONE && p->xfer[i].sid == sid && p->xfer[i].num == num)
            return i;
    return -1;
}

const FileXfer *proto_xfer(const Proto *p)
{
    return p->cur_xfer >= 0 ? &p->xfer[p->cur_xfer] : NULL;
}

int proto_xfer_slot(const Proto *p)
{
    return p->cur_xfer;
}

void proto_xfer_cancel(Proto *p, int slot)
{
    if (slot >= 0 && slot < MAX_XFER && p->xfer[slot].kind != XF_NONE)
        free_x(p, slot);
}

/* ---- listing ------------------------------------------------------------- */

int proto_list(Proto *p, int sid, const char *path)
{
    char buf[300];
    char *o = buf;
    if (p->state != ST_ONLINE || sid < 1 || sid > MAX_SESS || p->files == FILES_OFF
            || p->sess[sid].state == SS_NONE || p->sess[sid].state == SS_CLOSING
            || strlen(path) > 256)
        return 0;
    *o++ = (char)('0' + sid);
    o = put_str(o, "L ");
    o = put_str(o, path);
    *o++ = '\r';
    sendn(p, buf, o - buf);
    p->sess[sid].listing = 1;
    p->sess[sid].list_by = p->now + T_FILE;
    return 1;
}

int files_parse_entry(const char *t, long n, int *is_dir, long *size,
                      const char **name, long *name_len)
{
    long i = 0;
    const char *w;
    long k = word(t, n, &i, &w);
    if (!is(w, k, "d") && !is(w, k, "f"))
        return 0;
    *is_dir = w[0] == 'd';
    if (!word_long(t, n, &i, size))
        return 0;
    if (i >= n || t[i] != ' ')
        return 0;
    *name = t + i + 1;
    *name_len = n - i - 1;
    return *name_len > 0;
}

/* ---- reading ------------------------------------------------------------- */

static void send_get(Proto *p, int slot)
{
    FileXfer *x = &p->xfer[slot];
    char buf[340];
    char *o = buf;
    x->num = next_num(p);
    *o++ = (char)('0' + x->sid);
    o = put_str(o, "G ");
    o = put_long(o, (unsigned long)x->num);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->first);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->count);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->budget);
    *o++ = ' ';
    o = put_str(o, x->path);
    *o++ = '\r';
    sendn(p, buf, o - buf);
    x->phase = RD_WAIT;
    x->len = 0;
    x->lines_got = 0;
    x->noise_mark = p->noise_count;
    x->due = p->now + T_FILE;
}

int proto_read(Proto *p, int sid, long owner, const char *path,
               long first, long count, long budget)
{
    int slot = new_slot(p, sid, owner, path);
    if (slot < 0)
        return -1;
    p->xfer[slot].kind = XF_READ;
    p->xfer[slot].first = first < 0 ? 0 : first;
    p->xfer[slot].count = count < 0 ? 0 : count > PIECE_LINES ? PIECE_LINES : count;
    p->xfer[slot].budget = budget <= 0 || budget > PIECE_BYTES ? PIECE_BYTES : budget;
    send_get(p, slot);
    return slot;
}

/* Asks again with a fresh number, after a damaged or missing piece. */
static void read_retry(Proto *p, int slot, int spare_ack)
{
    FileXfer *x = &p->xfer[slot];
    int i;
    for (i = 1; i <= MAX_SESS; i++)
        if (p->sess[i].rx == slot)
            p->sess[i].rx = -1;
    if (++x->tries >= XFER_TRIES) {
        fail_x(p, slot, kNoisyRead, (long)sizeof(kNoisyRead) - 1);
        return;
    }
    if (spare_ack)
        sends(p, "!ACK\r");     /* a lost W would hold the bridge forever */
    send_get(p, slot);
}

static int append(FileXfer *x, const char *t, long n)
{
    if (x->len + n > x->cap) {
        long cap = x->cap ? x->cap : 1024;
        char *nb;
        while (cap < x->len + n)
            cap *= 2;
        nb = (char *)realloc(x->buf, cap);
        if (!nb)
            return 0;
        x->buf = nb;
        x->cap = cap;
    }
    memcpy(x->buf + x->len, t, n);
    x->len += n;
    return 1;
}

/* ---- saving -------------------------------------------------------------- */

/* The block starting at pos: whole lines up to about SAVE_BLOCK bytes. */
static long block_end(const char *t, long len, long pos)
{
    long e = pos, le;
    for (;;) {
        le = e;
        while (le < len && t[le] != '\r')
            le++;
        if (le - pos >= SAVE_BLOCK || le >= len)
            return le;
        e = le + 1;
    }
}

static void send_f(Proto *p, int slot)
{
    FileXfer *x = &p->xfer[slot];
    char buf[340];
    char *o = buf;
    *o++ = (char)('0' + x->sid);
    o = put_str(o, "F ");
    o = put_long(o, (unsigned long)x->num);
    *o++ = ' ';
    o = put_str(o, x->token);
    *o++ = ' ';
    o = put_str(o, x->path);
    *o++ = '\r';
    sendn(p, buf, o - buf);
    x->phase = SV_WAIT_F;
    x->due = p->now + T_FILE;
}

static void send_commit(Proto *p, int slot)
{
    FileXfer *x = &p->xfer[slot];
    char buf[64];
    char *o = buf;
    *o++ = (char)('0' + x->sid);
    o = put_str(o, "S. ");
    o = put_long(o, (unsigned long)x->num);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->tlen);
    *o++ = ' ';
    o = put_hex(o, crc32_buf(x->text, x->tlen));
    if (x->force)
        o = put_str(o, " force");
    *o++ = '\r';
    sendn(p, buf, o - buf);
    x->phase = SV_WAIT_COMMIT;
    x->due = p->now + T_FILE;
}

static void send_block(Proto *p, int slot)
{
    FileXfer *x = &p->xfer[slot];
    char buf[64], pre[4], line[300];
    char *o = buf;
    long i, s;

    *o++ = (char)('0' + x->sid);
    o = put_str(o, "S[ ");
    o = put_long(o, (unsigned long)x->num);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->seq);
    *o++ = '\r';
    sendn(p, buf, o - buf);

    pre[0] = (char)('0' + x->sid);
    pre[1] = 'S';
    pre[2] = ' ';
    s = x->pos;
    for (i = x->pos; ; i++) {
        if (i == x->end || x->text[i] == '\r') {
            if (i - s + 4 <= (long)sizeof(line)) {
                /* one serial write per line */
                memcpy(line, pre, 3);
                memcpy(line + 3, x->text + s, i - s);
                line[3 + i - s] = '\r';
                sendn(p, line, i - s + 4);
            } else {
                sendn(p, pre, 3);
                sendn(p, x->text + s, i - s);
                sendn(p, "\r", 1);
            }
            s = i + 1;
            if (i == x->end)
                break;
        }
    }

    o = buf;
    *o++ = (char)('0' + x->sid);
    o = put_str(o, "S] ");
    o = put_long(o, (unsigned long)x->num);
    *o++ = ' ';
    o = put_long(o, (unsigned long)x->seq);
    *o++ = ' ';
    o = put_long(o, (unsigned long)(x->end - x->pos));
    *o++ = ' ';
    o = put_hex(o, crc32_buf(x->text + x->pos, x->end - x->pos));
    *o++ = '\r';
    sendn(p, buf, o - buf);
    x->phase = SV_WAIT_BLOCK;
    x->due = p->now + T_FILE;
}

int proto_save(Proto *p, int sid, long owner, const char *path,
               const char *token, const char *text, long len)
{
    int slot;
    FileXfer *x;
    long pos, e, n = 0;
    if (p->files != FILES_RW || strlen(token) >= sizeof(p->xfer[0].token) || len < 0)
        return -1;
    slot = new_slot(p, sid, owner, path);
    if (slot < 0)
        return -1;
    x = &p->xfer[slot];
    x->text = (char *)malloc(len > 0 ? len : 1);
    if (!x->text)
        return -1;
    memcpy(x->text, text, len);
    x->tlen = len;
    x->kind = XF_SAVE;
    strcpy(x->token, token);
    for (pos = 0; ; pos = e + 1) {
        e = block_end(x->text, len, pos);
        n++;
        if (e >= len)
            break;
    }
    x->nblocks = (int)n;
    x->num = next_num(p);
    send_f(p, slot);
    return slot;
}

int proto_save_force(Proto *p, int slot)
{
    FileXfer *x;
    if (slot < 0 || slot >= MAX_XFER)
        return 0;
    x = &p->xfer[slot];
    if (x->kind != XF_SAVE || x->phase != SV_CONFLICT || p->state != ST_ONLINE)
        return 0;
    x->force = 1;
    x->tries = 0;
    send_commit(p, slot);
    return 1;
}

void proto_files_step(Proto *p)
{
    int i;
    if (p->state != ST_ONLINE)
        return;
    for (i = 0; i < MAX_XFER; i++)
        if (p->xfer[i].kind == XF_SAVE && p->xfer[i].phase == SV_SEND) {
            send_block(p, i);
            return;              /* one block per pass */
        }
}

/* ---- lines from the bridge ----------------------------------------------- */

static void on_h(Proto *p, int sid, const char *pay, long plen)
{
    long i = 0, num, first, count, total;
    const char *w;
    long k;
    int slot;
    FileXfer *x;
    p->sess[sid].rx = -1;
    if (!word_long(pay, plen, &i, &num) || (slot = find_x(p, sid, num)) < 0
            || p->xfer[slot].kind != XF_READ || p->xfer[slot].phase != RD_WAIT) {
        p->dropped++;            /* an earlier try, or garbled */
        return;
    }
    x = &p->xfer[slot];
    if (!word_long(pay, plen, &i, &first) || !word_long(pay, plen, &i, &count)
            || !word_long(pay, plen, &i, &total)) {
        read_retry(p, slot, 0);
        return;
    }
    x->h_first = first;
    x->h_count = count;
    x->h_total = total;
    k = word(pay, plen, &i, &w);
    copy_word(x->token, sizeof(x->token), w, k);
    k = word(pay, plen, &i, &w);
    copy_word(x->mode, sizeof(x->mode), w, k);
    x->phase = RD_BODY;
    x->len = 0;
    x->lines_got = 0;
    p->sess[sid].rx = slot;
}

static void on_z(Proto *p, int sid, const char *pay, long plen)
{
    long i = 0, num, bytes;
    unsigned long crc;
    int slot = p->sess[sid].rx;
    FileXfer *x;
    p->sess[sid].rx = -1;
    if (slot < 0 || !word_long(pay, plen, &i, &num) || p->xfer[slot].num != num) {
        p->dropped++;
        return;
    }
    x = &p->xfer[slot];
    if (!word_long(pay, plen, &i, &bytes) || !word_hex(pay, plen, &i, &crc)
            || x->lines_got != x->h_count || x->len != bytes
            || crc32_buf(x->buf, x->len) != crc || x->noise_mark != p->noise_count) {
        read_retry(p, slot, 0);
        return;
    }
    emit_x(p, slot, EV_PIECE, x->buf, x->len);
    if (x->kind != XF_NONE)
        free_x(p, slot);
}

static void on_k(Proto *p, int sid, const char *pay, long plen)
{
    long i = 0, num, seq;
    const char *w;
    long k;
    int slot;
    FileXfer *x;
    if (!word_long(pay, plen, &i, &num) || (slot = find_x(p, sid, num)) < 0
            || p->xfer[slot].kind != XF_SAVE)
        return;
    x = &p->xfer[slot];
    k = word(pay, plen, &i, &w);
    if (is(w, k, "saved")) {
        k = word(pay, plen, &i, &w);
        copy_word(x->token, sizeof(x->token), w, k);
        emit_x(p, slot, EV_SAVED, x->token, (long)strlen(x->token));
        if (x->kind != XF_NONE)
            free_x(p, slot);
        return;
    }
    i -= k;
    if (!word_long(pay, plen, &i, &seq))
        return;
    if (x->phase == SV_WAIT_F && seq == 0) {
        x->tries = 0;
        x->seq = 1;
        x->pos = 0;
        x->end = block_end(x->text, x->tlen, 0);
        x->phase = SV_SEND;
    } else if (x->phase == SV_WAIT_BLOCK && seq == x->seq) {
        x->tries = 0;
        emit_x(p, slot, EV_SAVE_PROGRESS, "", 0);
        if (x->kind == XF_NONE)
            return;
        if (x->end >= x->tlen) {
            send_commit(p, slot);
        } else {
            x->pos = x->end + 1;
            x->end = block_end(x->text, x->tlen, x->pos);
            x->seq++;
            x->phase = SV_SEND;
        }
    }
}

static void on_n(Proto *p, int sid, const char *pay, long plen)
{
    long i = 0, num, j, seq;
    const char *w, *rest;
    long k, rlen;
    int slot;
    FileXfer *x;
    k = word(pay, plen, &i, &w);
    while (i < plen && pay[i] == ' ')
        i++;
    rest = pay + i;
    rlen = plen - i;
    if (is(w, k, "-")) {
        if (p->sess[sid].listing) {
            p->sess[sid].listing = 0;
            p->on_event(p->ctx, sid, EV_LIST_FAILED, rest, rlen);
        }
        return;
    }
    j = 0;
    if (!word_long(w, k, &j, &num) || (slot = find_x(p, sid, num)) < 0)
        return;
    x = &p->xfer[slot];
    if (x->kind == XF_READ) {
        fail_x(p, slot, rest, rlen);
        return;
    }
    j = 0;
    if (x->phase == SV_WAIT_BLOCK && word_long(rest, rlen, &j, &seq) && seq == x->seq) {
        if (++x->tries >= XFER_TRIES)
            fail_x(p, slot, kNoisySave, (long)sizeof(kNoisySave) - 1);
        else
            x->phase = SV_SEND;  /* resend this block */
        return;
    }
    if (x->phase == SV_WAIT_COMMIT && is(rest, rlen, "changed")) {
        x->phase = SV_CONFLICT;
        emit_x(p, slot, EV_SAVE_CONFLICT, rest, rlen);
        return;
    }
    fail_x(p, slot, rest, rlen);
}

void files_line(Proto *p, int sid, const char *tag, long tlen, const char *pay, long plen)
{
    Session *s = &p->sess[sid];
#define TAG(x) (tlen == (long)sizeof(x) - 1 && memcmp(tag, x, tlen) == 0)
    if (TAG("L[")) {
        if (s->listing)
            p->on_event(p->ctx, sid, EV_LIST_BEGIN, pay, plen);
    } else if (TAG("L")) {
        if (s->listing)
            p->on_event(p->ctx, sid, EV_LIST_ENTRY, pay, plen);
    } else if (TAG("L]")) {
        if (s->listing) {
            s->listing = 0;
            p->on_event(p->ctx, sid, EV_LIST_END, pay, plen);
        }
    } else if (TAG("H")) {
        on_h(p, sid, pay, plen);
    } else if (TAG("B") || TAG("+")) {
        FileXfer *x = s->rx >= 0 ? &p->xfer[s->rx] : NULL;
        if (!x) {
            p->dropped++;
            return;
        }
        if (TAG("B")) {
            if (x->lines_got > 0 && !append(x, "\r", 1))
                x->lines_got = -1;   /* out of memory: the checks will fail */
            x->lines_got++;
        }
        if (x->lines_got > 0 && !append(x, pay, plen))
            x->lines_got = -1;
    } else if (TAG("Z")) {
        on_z(p, sid, pay, plen);
    } else if (TAG("K")) {
        on_k(p, sid, pay, plen);
    } else if (TAG("N")) {
        on_n(p, sid, pay, plen);
    }
#undef TAG
}

void files_tick(Proto *p)
{
    int i;
    static const char no_answer[] = "No answer from the bridge.";
    for (i = 1; i <= MAX_SESS; i++) {
        Session *s = &p->sess[i];
        if (s->listing && (long)(p->now - s->list_by) >= 0) {
            s->listing = 0;
            p->on_event(p->ctx, i, EV_LIST_FAILED, no_answer, (long)sizeof(no_answer) - 1);
        }
    }
    for (i = 0; i < MAX_XFER; i++) {
        FileXfer *x = &p->xfer[i];
        if (x->kind == XF_NONE || x->phase == SV_SEND || x->phase == SV_CONFLICT
                || (long)(p->now - x->due) < 0)
            continue;
        if (x->kind == XF_READ) {
            read_retry(p, i, 1);
            continue;
        }
        if (++x->tries >= XFER_TRIES) {
            fail_x(p, i, kNoisySave, (long)sizeof(kNoisySave) - 1);
            continue;
        }
        sends(p, "!ACK\r");
        if (x->phase == SV_WAIT_F)
            send_f(p, i);
        else if (x->phase == SV_WAIT_BLOCK)
            x->phase = SV_SEND;
        else
            send_commit(p, i);
    }
}

void files_fail(Proto *p, int sid, const char *why)
{
    int i;
    for (i = 1; i <= MAX_SESS; i++) {
        Session *s = &p->sess[i];
        if ((sid == 0 || sid == i) && s->listing) {
            s->listing = 0;
            p->on_event(p->ctx, i, EV_LIST_FAILED, why, (long)strlen(why));
        }
    }
    for (i = 0; i < MAX_XFER; i++)
        if (p->xfer[i].kind != XF_NONE && (sid == 0 || p->xfer[i].sid == sid))
            fail_x(p, i, why, (long)strlen(why));
}

/* ---- the :changes text ------------------------------------------------------ */

static int status_letter(char c)
{
    return c == ' ' || c == 'M' || c == 'A' || c == 'D' || c == 'R' || c == 'C'
        || c == 'U' || c == 'T' || c == '?' || c == '!';
}

/* Does t[0..n) end with the word s? */
static int ends_with(const char *t, long n, const char *s)
{
    long k = (long)strlen(s);
    return n >= k && memcmp(t + n - k, s, k) == 0;
}

int files_changes_path(const char *line, long n, char *out, long max)
{
    long end, start, i;
    out[0] = 0;
    /* "XY " and at least one path byte. */
    if (n < 4 || !status_letter(line[0]) || !status_letter(line[1]) || line[2] != ' ')
        return 0;
    if (line[0] == ' ' && line[1] == ' ')
        return 0;
    for (i = 0; i < n; i++)
        if ((unsigned char)line[i] == 0xC9 && i >= n - 1)
            return 0;                    /* the viewer clipped this line */
    end = n;
    if (ends_with(line, end, " binary")) {
        end -= 7;
    } else {
        /* " +<digits> -<digits>" */
        long k = end;
        while (k > 0 && line[k - 1] >= '0' && line[k - 1] <= '9')
            k--;
        if (k == end || k < 3 || line[k - 1] != '-' || line[k - 2] != ' ')
            return 0;
        k -= 2;
        {
            long d = k;
            while (d > 0 && line[d - 1] >= '0' && line[d - 1] <= '9')
                d--;
            if (d == k || d < 3 || line[d - 1] != '+' || line[d - 2] != ' ')
                return 0;
            end = d - 2;
        }
    }
    /* " (from <old>)" belongs to a rename: the path is the new name. */
    if (end > 0 && line[end - 1] == ')') {
        long j;
        for (j = end - 7; j >= 3; j--)
            if (memcmp(line + j, " (from ", 7) == 0) {
                end = j;
                break;
            }
    }
    start = 3;
    if (end <= start || end - start >= max)
        return 0;
    memcpy(out, line + start, end - start);
    out[end - start] = 0;
    return 1;
}
