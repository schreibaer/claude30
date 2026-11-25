#include "agentline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mac Roman bytes. */
#define MR_RAQUO  "\xC8"   /* right-pointing double angle quotation mark */
#define MR_MIDDOT "\xE1"   /* middle dot */
#define MR_ELLIP  "\xC9"   /* ellipsis */

void al_init(AgentLines *a)
{
    memset(a, 0, sizeof(*a));
}

/* Copies up to cap-1 bytes of s[0..n) into dst, control characters as spaces. */
static void copy_text(char *dst, long cap, const char *s, long n)
{
    long i;
    if (n > cap - 1)
        n = cap - 1;
    for (i = 0; i < n; i++)
        dst[i] = (unsigned char)s[i] < ' ' ? ' ' : s[i];
    dst[n] = 0;
}

/* The next space-separated word of pay, advancing *pos past it. */
static long word(const char *pay, long n, long *pos, const char **w)
{
    long i = *pos, e;
    while (i < n && pay[i] == ' ')
        i++;
    e = i;
    while (e < n && pay[e] != ' ')
        e++;
    *w = pay + i;
    *pos = e;
    return e - i;
}

/* The rest of pay after the words taken so far. */
static long rest(const char *pay, long n, long pos, const char **r)
{
    while (pos < n && pay[pos] == ' ')
        pos++;
    *r = pay + pos;
    return n - pos;
}

static int number(const char *w, long len)
{
    long v = 0, i;
    if (len <= 0 || len > 4)
        return -1;
    for (i = 0; i < len; i++) {
        if (w[i] < '0' || w[i] > '9')
            return -1;
        v = v * 10 + (w[i] - '0');
    }
    return (int)v;
}

static AgentSlot *find_open(AgentLines *a, int k, int *idx)
{
    int i;
    for (i = 0; i < AL_SLOTS; i++)
        if (a->s[i].state == AL_OPEN && a->s[i].k == k) {
            *idx = i;
            return &a->s[i];
        }
    return NULL;
}

static void put_line(char *out, long cap, const char *s)
{
    long n = (long)strlen(s);
    if (n > cap - 1)
        n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static const char *plural(int n)
{
    return n == 1 ? "tool" : "tools";
}

/* "  » type: task", then " · tool · N tools" once it has taken steps. */
static void compose_running(const AgentSlot *s, char *out, long cap)
{
    char buf[AL_TEXT + 64];
    if (s->steps > 0 && s->tool[0] && strcmp(s->tool, "-") != 0)
        snprintf(buf, sizeof(buf), "  " MR_RAQUO " %s: %s " MR_MIDDOT " %s " MR_MIDDOT " %d %s",
                 s->type, s->task, s->tool, s->steps, plural(s->steps));
    else if (s->steps > 0)
        snprintf(buf, sizeof(buf), "  " MR_RAQUO " %s: %s " MR_MIDDOT " %d %s",
                 s->type, s->task, s->steps, plural(s->steps));
    else
        snprintf(buf, sizeof(buf), "  " MR_RAQUO " %s: %s", s->type, s->task);
    put_line(out, cap, buf);
}

/* "  » type: status · N tools · summary" */
static void compose_end(const AgentSlot *s, const char *status, const char *sum,
                        char *out, long cap)
{
    char buf[AL_TEXT + 64];
    int k = snprintf(buf, sizeof(buf), "  " MR_RAQUO " %s: %s", s->type, status);
    if (s->steps > 0)
        k += snprintf(buf + k, sizeof(buf) - k, " " MR_MIDDOT " %d %s", s->steps, plural(s->steps));
    if (sum[0])
        snprintf(buf + k, sizeof(buf) - k, " " MR_MIDDOT " %s", sum);
    put_line(out, cap, buf);
}

int al_start(AgentLines *a, const char *pay, long n, char *out, long cap)
{
    long pos = 0;
    const char *w, *r;
    long wl;
    int k, i, idx = -1, old;
    AgentSlot *s;
    char type[24], task[80];

    out[0] = 0;
    wl = word(pay, n, &pos, &w);
    k = number(w, wl);
    wl = word(pay, n, &pos, &w);
    if (k < 0 || wl <= 0)
        return -1;
    copy_text(type, sizeof(type), w, wl);
    {
        long rl = rest(pay, n, pos, &r);
        copy_text(task, sizeof(task), r, rl);
    }

    /* A line still open with this number lost its end: close it quietly. */
    if ((s = find_open(a, k, &old)) != NULL)
        s->state = AL_ENDED;

    for (i = 0; i < AL_SLOTS && idx < 0; i++)
        if (a->s[i].state == AL_FREE)
            idx = i;
    if (idx < 0) {
        for (i = 0; i < AL_SLOTS; i++)
            if (a->s[i].state == AL_ENDED && (idx < 0 || a->s[i].seq < a->s[idx].seq))
                idx = i;
    }
    if (idx < 0) {
        if (a->nplain < AL_PLAIN) {
            a->plain[a->nplain].k = k;
            memcpy(a->plain[a->nplain].type, type, sizeof(type));
            a->nplain++;
        }
        {
            char buf[AL_TEXT + 64];
            snprintf(buf, sizeof(buf), "Agent %s: %s", type, task);
            put_line(out, cap, buf);
        }
        return -2;
    }
    s = &a->s[idx];
    memset(s, 0, sizeof(*s));
    s->state = AL_OPEN;
    s->k = k;
    s->seq = ++a->seq;
    memcpy(s->type, type, sizeof(type));
    memcpy(s->task, task, sizeof(task));
    compose_running(s, out, cap);
    return idx;
}

int al_step(AgentLines *a, const char *pay, long n, char *out, long cap)
{
    long pos = 0;
    const char *w, *r;
    long wl;
    int k, steps, idx;
    AgentSlot *s;

    out[0] = 0;
    wl = word(pay, n, &pos, &w);
    k = number(w, wl);
    wl = word(pay, n, &pos, &w);
    steps = number(w, wl);
    if (k < 0 || steps < 0 || (s = find_open(a, k, &idx)) == NULL)
        return -1;
    wl = word(pay, n, &pos, &w);
    copy_text(s->tool, sizeof(s->tool), w, wl);
    (void)r;
    if (steps > s->steps)
        s->steps = steps;
    compose_running(s, out, cap);
    return idx;
}

int al_end(AgentLines *a, const char *pay, long n, char *out, long cap)
{
    long pos = 0;
    const char *w, *r;
    long wl, rl;
    int k, steps, idx, i;
    AgentSlot *s;
    char status[16], sum[AL_TEXT];

    out[0] = 0;
    wl = word(pay, n, &pos, &w);
    k = number(w, wl);
    wl = word(pay, n, &pos, &w);
    copy_text(status, sizeof(status), w, wl);
    {
        const char *sw;
        long sl = word(pay, n, &pos, &sw);
        steps = number(sw, sl);
    }
    rl = rest(pay, n, pos, &r);
    copy_text(sum, sizeof(sum), r, rl);
    if (k < 0 || status[0] == 0)
        return -1;
    if ((s = find_open(a, k, &idx)) == NULL) {
        for (i = 0; i < a->nplain; i++)
            if (a->plain[i].k == k) {
                char buf[AL_TEXT + 64];
                snprintf(buf, sizeof(buf), "Agent %s %s: %s", a->plain[i].type, status, sum);
                put_line(out, cap, buf);
                memmove(&a->plain[i], &a->plain[i + 1], (a->nplain - i - 1) * sizeof(a->plain[0]));
                a->nplain--;
                return -2;
            }
        return -1;
    }
    if (steps > s->steps)
        s->steps = steps;
    s->state = AL_ENDED;
    compose_end(s, status, sum, out, cap);
    return idx;
}

int al_stop_next(AgentLines *a, char *out, long cap)
{
    int i;
    out[0] = 0;
    for (i = 0; i < AL_SLOTS; i++)
        if (a->s[i].state == AL_OPEN) {
            a->s[i].state = AL_ENDED;
            compose_end(&a->s[i], "stopped", "", out, cap);
            return i;
        }
    a->nplain = 0;               /* plain ones have no line to close */
    return -1;
}

void al_cut(AgentLines *a, long cut)
{
    int i;
    if (cut <= 0)
        return;
    for (i = 0; i < AL_SLOTS; i++) {
        AgentSlot *s = &a->s[i];
        if (!s->drawn || s->gone)
            continue;
        if (s->off >= cut)
            s->off -= cut;
        else
            s->gone = 1;
    }
}

void al_shift(AgentLines *a, long off, long delta)
{
    int i;
    for (i = 0; i < AL_SLOTS; i++) {
        AgentSlot *s = &a->s[i];
        if (s->drawn && !s->gone && s->off > off)
            s->off += delta;
    }
}

long al_fit(char *s, long len, long maxw, long (*measure)(const char *, long))
{
    long keep;
    if (measure(s, len) <= maxw)
        return len;
    keep = len;
    while (keep > 2) {
        char tmp[AL_TEXT + 4];
        long k = keep;
        if (k > AL_TEXT)
            k = AL_TEXT;
        memcpy(tmp, s, k);
        memcpy(tmp + k, MR_ELLIP, 1);
        if (measure(tmp, k + 1) <= maxw) {
            memcpy(s, tmp, k + 1);
            return k + 1;
        }
        keep--;
    }
    return 2;
}
