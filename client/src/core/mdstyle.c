#include "mdstyle.h"
#include <stdlib.h>
#include <string.h>

/* Mac Roman bytes used as glyphs. */
#define MR_BULLET  0xA5
#define MR_ENDASH  0xD0
#define MR_EMDASH  0xD1

#define MD_TAIL_MAX 1024   /* held text (at most MD_HOLD_MAX) plus a piece */
#define MD_RUNS     256    /* * and _ runs looked at in one scan */
#define RULE_DEFAULT 40

/* What scan() decides about each byte of the tail. */
#define C_FLAGS 0x1F       /* BF_* faces */
#define C_DROP  0x20       /* a mark: not drawn */
#define C_PROT  0x40       /* literal text: never a * or _ mark */

struct MdStyle {
    int started;             /* the line's kind is decided */
    int table;               /* a table row: Monaco, no marks */
    int tbl_free;            /* ... and not a separator row */
    unsigned char base;      /* faces of the whole line */
    int prev;                /* the byte before the tail, 0 at the line start */
    long n;                  /* bytes in the tail */
    unsigned char tail[MD_TAIL_MAX];   /* text not drawn yet */
    unsigned char cls[MD_TAIL_MAX];    /* scan's verdict per byte */
    /* scan's * and _ runs */
    short rpos[MD_RUNS], rlo[MD_RUNS], rhi[MD_RUNS], stk[MD_RUNS];
    unsigned char rch[MD_RUNS], ropen[MD_RUNS], rclose[MD_RUNS];
};

/* ---- byte classes -------------------------------------------------------- */

static int is_sp(int c)
{
    return c == 0 || c == ' ' || c == '\t';
}

/* Mac Roman punctuation above 0x7F: quotes, dashes, bullet, guillemets. */
static int is_hi_punct(int c)
{
    return c == 0xA5 || c == 0xC0 || c == 0xC1 || c == 0xC7 || c == 0xC8 || c == 0xC9
        || (c >= 0xD0 && c <= 0xD5);
}

static int is_alnum(int c)
{
    if (c >= 0x80)
        return !is_hi_punct(c);
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_punct(int c)
{
    return !is_sp(c) && !is_alnum(c);
}

static int is_ascii_punct(int c)
{
    return c > 0x20 && c < 0x7F && !is_alnum(c);
}

static int starts_http(const unsigned char *s, long rem)
{
    if (rem > 8 && memcmp(s, "https://", 8) == 0)
        return 8;
    if (rem > 7 && memcmp(s, "http://", 7) == 0)
        return 7;
    return 0;
}

/* ---- the tail ------------------------------------------------------------ */

static void reset(MdStyle *m)
{
    m->started = 0;
    m->table = 0;
    m->tbl_free = 0;
    m->base = 0;
    m->prev = 0;
    m->n = 0;
}

static void shift(MdStyle *m, long k)
{
    memmove(m->tail, m->tail + k, (size_t)(m->n - k));
    m->n -= k;
}

/* Draws tail[0, upto) with the faces scan chose, without the marks. */
static void emit_range(Batch *b, MdStyle *m, long upto)
{
    long i = 0, j;
    unsigned char f;
    while (i < upto) {
        if (m->cls[i] & C_DROP) {
            i++;
            continue;
        }
        f = (unsigned char)(m->base | (m->cls[i] & C_FLAGS));
        for (j = i + 1; j < upto && !(m->cls[j] & C_DROP)
                && (unsigned char)(m->base | (m->cls[j] & C_FLAGS)) == f; j++)
            ;
        batch_put_prose(b, f, (const char *)m->tail + i, j - i);
        i = j;
    }
}

/* ---- scan: where the marks are -------------------------------------------- */

/* Finds the spans in the tail and sets cls[]. Returns how many bytes at the
   front are settled: with !final, text from the first mark that still waits
   for its partner on is held back. Marks before lit are literal. */
static long scan(MdStyle *m, int final, long lit)
{
    unsigned char *t = m->tail, *c = m->cls;
    long n = m->n, i, j, k, x, limit = n, h;
    long lstart = -1, lstop = -1, skip_at = -1, skip_to = 0;
    int nr = 0, sp = 0, r, s, ch;

    memset(c, 0, (size_t)n);

    /* Pass 1: escapes, code spans, links and addresses. These are literal
       to the * and _ pass. */
    for (i = 0; i < n;) {
        ch = t[i];
        if (i == skip_at) {
            i = skip_to;
            skip_at = -1;
            continue;
        }
        if (ch == '\\' && i + 1 < n && is_ascii_punct(t[i + 1])) {
            c[i] |= C_DROP;
            c[i + 1] |= C_PROT;
            i += 2;
            continue;
        }
        if (ch == '`') {
            k = 1;
            while (i + k < n && t[i + k] == '`')
                k++;
            if (i < lit || k > 3) {
                for (x = i; x < i + k; x++)
                    c[x] |= C_PROT;
                i += k;
                continue;
            }
            for (j = i + k; j < n;) {
                if (t[j] == '`') {
                    long e = 1;
                    while (j + e < n && t[j + e] == '`')
                        e++;
                    if (e == k)
                        break;
                    j += e;
                } else {
                    j++;
                }
            }
            if (j < n) {
                for (x = i; x < i + k; x++)
                    c[x] |= C_DROP | C_PROT;
                for (x = i + k; x < j; x++)
                    c[x] |= BF_MONO | C_PROT;
                for (x = j; x < j + k; x++)
                    c[x] |= C_DROP | C_PROT;
                i = j + k;
                continue;
            }
            if (!final && i >= lit) {
                limit = i;        /* the closing backticks may still come */
                break;
            }
            for (x = i; x < i + k; x++)
                c[x] |= C_PROT;
            i += k;
            continue;
        }
        if (ch == '[' && skip_at < 0 && i >= lit) {
            for (j = i + 1; j < n && t[j] != ']'; j++)
                ;
            if (j >= n) {
                if (!final) {
                    limit = i;    /* the ] may still come */
                    break;
                }
                i++;
                continue;
            }
            if (j + 1 < n && t[j + 1] == '(') {
                for (k = j + 2; k < n && t[k] != ')'; k++)
                    ;
                if (k >= n) {
                    if (!final) {
                        limit = i;
                        break;
                    }
                    i++;
                    continue;
                }
                c[i] |= C_DROP | C_PROT;
                for (x = i + 1; x < j; x++)
                    c[x] |= BF_UNDER;
                for (x = j; x <= k; x++)
                    c[x] |= C_DROP | C_PROT;
                lstart = i;
                lstop = k;
                skip_at = j;
                skip_to = k + 1;
            }
            i++;
            continue;
        }
        if (ch == '<' && starts_http(t + i + 1, n - i - 1)) {
            for (j = i + 1; j < n && t[j] != '>' && t[j] != ' '; j++)
                ;
            if (j < n && t[j] == '>') {
                c[i] |= C_DROP | C_PROT;
                for (x = i + 1; x < j; x++)
                    c[x] |= BF_UNDER | C_PROT;
                c[j] |= C_DROP | C_PROT;
                i = j + 1;
                continue;
            }
        }
        if (ch == 'h' && !is_alnum(i > 0 ? t[i - 1] : m->prev)) {
            k = starts_http(t + i, n - i);
            if (k) {
                for (j = i + k; j < n && t[j] != ' '; j++)
                    ;
                x = j;
                while (x > i + k) {
                    int e = t[x - 1];
                    if (e == '.' || e == ',' || e == ';' || e == ':' || e == '!' || e == '?'
                            || e == '\'' || e == '"') {
                        x--;
                    } else if (e == ')') {
                        long p, open = 0;
                        for (p = i; p < x - 1; p++)
                            if (t[p] == '(')
                                open++;
                        for (p = i; p < x - 1; p++)
                            if (t[p] == ')')
                                open--;
                        if (open > 0)
                            break;
                        x--;
                    } else {
                        break;
                    }
                }
                for (k = i; k < x; k++)
                    c[k] |= BF_UNDER | C_PROT;
                i = j;
                continue;
            }
        }
        i++;
    }

    /* Pass 2: runs of * and _ up to limit. */
    for (i = 0; i < limit;) {
        ch = t[i];
        if ((ch == '*' || ch == '_') && !(c[i] & C_PROT)) {
            int pc, nc;
            for (j = i + 1; j < limit && t[j] == ch && !(c[j] & C_PROT); j++)
                ;
            pc = i > 0 ? t[i - 1] : m->prev;
            nc = j < n ? t[j] : 0;
            if (nr < MD_RUNS) {
                m->rpos[nr] = (short)i;
                m->rlo[nr] = (short)i;
                m->rhi[nr] = (short)j;
                m->rch[nr] = (unsigned char)ch;
                m->ropen[nr] = (unsigned char)(i >= lit && !is_sp(nc) && (is_sp(pc) || is_punct(pc)));
                m->rclose[nr] = (unsigned char)(i >= lit && !is_sp(pc) && (is_sp(nc) || is_punct(nc)));
                nr++;
            }
            i = j;
            continue;
        }
        i++;
    }
    for (r = 0; r < nr; r++) {
        if (m->rclose[r]) {
            while (m->rhi[r] > m->rlo[r]) {
                int o, nn;
                long ostart, cstart, avail_o, avail_c;
                for (s = sp - 1; s >= 0 && m->rch[m->stk[s]] != m->rch[r]; s--)
                    ;
                if (s < 0)
                    break;
                o = m->stk[s];
                avail_o = m->rhi[o] - m->rlo[o];
                avail_c = m->rhi[r] - m->rlo[r];
                nn = (avail_o >= 2 && avail_c >= 2) ? 2 : 1;
                ostart = m->rhi[o] - nn;
                cstart = m->rlo[r];
                for (x = m->rhi[o]; x < cstart; x++)
                    c[x] |= nn == 2 ? BF_BOLD : BF_ITALIC;
                for (x = ostart; x < ostart + nn; x++)
                    c[x] |= C_DROP;
                for (x = cstart; x < cstart + nn; x++)
                    c[x] |= C_DROP;
                m->rhi[o] = (short)ostart;
                m->rlo[r] = (short)(cstart + nn);
                sp = s + (m->rhi[o] > m->rlo[o] ? 1 : 0);
            }
        }
        if (m->ropen[r] && m->rhi[r] > m->rlo[r])
            m->stk[sp++] = (short)r;
    }

    h = final ? n : limit;
    if (!final && sp > 0 && m->rlo[m->stk[0]] < h)
        h = m->rlo[m->stk[0]];
    if (lstart >= 0 && h > lstart && h <= lstop)
        h = lstart;
    return h;
}

/* ---- the three kinds of line -------------------------------------------- */

static void put_rule(Batch *b)
{
    unsigned char d[128];
    int k = b->rule_len > 0 ? b->rule_len : RULE_DEFAULT;
    if (k > (int)sizeof d)
        k = (int)sizeof d;
    memset(d, MR_EMDASH, (size_t)k);
    batch_put_prose(b, 0, (const char *)d, k);
}

/* Reads the line's first bytes and draws its marker. Returns 0 when it
   cannot tell yet (a lone "##", "-" or ">", or a line of only dashes). */
static int decide(Batch *b, MdStyle *m, int final)
{
    unsigned char *t = m->tail;
    long n = m->n, i = 0, k, e;
    int c;

    if (n >= MD_HOLD_MAX)
        final = 1;
    while (i < n && t[i] == ' ')
        i++;
    if (i == n) {
        if (!final)
            return 0;
        goto plain;
    }
    c = t[i];
    if (c == '|') {
        m->table = 1;
        m->base = BF_MONO;
        m->started = 1;
        m->prev = 0;
        return 1;
    }
    if (c == '#') {
        for (k = 0; i + k < n && t[i + k] == '#'; k++)
            ;
        if (k <= 6) {
            if (i + k == n) {
                if (!final)
                    return 0;
            } else if (t[i + k] == ' ') {
                for (e = i + k; e < n && t[e] == ' '; e++)
                    ;
                m->base = k == 1 ? (BF_BOLD | BF_EXTEND | BF_UNDER)
                        : k == 2 ? (BF_BOLD | BF_EXTEND) : BF_BOLD;
                shift(m, e);
                m->started = 1;
                m->prev = ' ';
                return 1;
            }
        }
        goto plain;
    }
    if (c == '>') {
        if (i + 1 == n) {
            if (!final)
                return 0;
        } else if (t[i + 1] == ' ') {
            batch_put_prose(b, BF_ITALIC, "| ", 2);
            m->base = BF_ITALIC;
            shift(m, i + 2);
            m->started = 1;
            m->prev = ' ';
            return 1;
        }
        goto plain;
    }
    if (c == '-' || c == '*' || c == '_' || c == '+') {
        int rule = c != '+';
        long cnt = 0, j;
        for (j = i; j < n; j++) {
            if (t[j] == c) {
                cnt++;
            } else if (t[j] != ' ') {
                rule = 0;
                break;
            }
        }
        if (rule) {
            if (!final)
                return 0;                 /* a rule, or a bullet, once more arrives */
            if (cnt >= 3) {
                put_rule(b);
                m->n = 0;
                m->started = 1;
                m->prev = 0;
                return 1;
            }
        }
        if (c != '_') {
            if (i + 1 < n && t[i + 1] == ' ') {
                unsigned char g[2];
                g[0] = i >= 2 ? MR_ENDASH : MR_BULLET;
                g[1] = ' ';
                if (i > 0)
                    batch_put_prose(b, 0, (const char *)t, i);
                batch_put_prose(b, 0, (const char *)g, 2);
                for (e = i + 1; e < n && t[e] == ' '; e++)
                    ;
                shift(m, e);
                m->started = 1;
                m->prev = ' ';
                return 1;
            }
            if (i + 1 == n && !final)
                return 0;
        }
    }
plain:
    m->started = 1;
    m->prev = 0;
    return 1;
}

static void table_step(Batch *b, MdStyle *m, int final)
{
    unsigned char *t = m->tail;
    long n = m->n, j, first = -1, last = -1;

    if (n == 0)
        return;
    if (!m->tbl_free) {
        int sep = 1, dash = 0;
        for (j = 0; j < n; j++) {
            if (t[j] == '-')
                dash = 1;
            else if (t[j] != '|' && t[j] != ':' && t[j] != ' ')
                sep = 0;
        }
        if (sep && !final && n < MD_HOLD_MAX)
            return;                       /* a separator row is drawn whole */
        if (sep && dash && final) {
            for (j = 0; j < n; j++) {
                if (t[j] == '|') {
                    if (first < 0)
                        first = j;
                    last = j;
                }
            }
            for (j = first + 1; j < last; j++) {
                if (t[j] == '|')
                    t[j] = '+';
                else if (t[j] == ':' || t[j] == ' ')
                    t[j] = '-';
            }
        }
        m->tbl_free = 1;
    }
    batch_put_prose(b, BF_MONO, (const char *)t, n);
    m->n = 0;
}

static void inline_step(Batch *b, MdStyle *m, int final)
{
    long lit = 0, h = 0, k;

    if (m->n == 0)
        return;
    for (;;) {
        h = scan(m, final, lit);
        if (!final && m->n - h > MD_HOLD_MAX) {
            /* A mark has waited too long: it is text. */
            int o = m->tail[h];
            k = 1;
            if (o == '*' || o == '_' || o == '`')
                while (h + k < m->n && m->tail[h + k] == o)
                    k++;
            lit = h + k;
            continue;
        }
        break;
    }
    if (h > 0) {
        emit_range(b, m, h);
        m->prev = m->tail[h - 1];
        shift(m, h);
    }
}

static void step(Batch *b, MdStyle *m, int final)
{
    if (!m->started && !decide(b, m, final))
        return;
    if (m->table)
        table_step(b, m, final);
    else
        inline_step(b, m, final);
}

static void feed(Batch *b, MdStyle *m, const unsigned char *s, long n)
{
    while (n > 0) {
        long room = MD_TAIL_MAX - m->n, take = n < room ? n : room;
        if (take <= 0) {            /* cannot happen: a step leaves at most MD_HOLD_MAX */
            step(b, m, 1);
            m->n = 0;
            continue;
        }
        memcpy(m->tail + m->n, s, (size_t)take);
        m->n += take;
        s += take;
        n -= take;
        step(b, m, 0);
    }
}

void md_text(Batch *b, const char *text, long len, int cont)
{
    MdStyle *m = b->md;
    if (!m) {
        m = (MdStyle *)calloc(1, sizeof *m);
        if (!m) {
            b->oom = 1;
            if (cont)
                batch_put_prose(b, 0, " ", 1);
            batch_put_prose(b, 0, text, len);
            return;
        }
        b->md = m;
    }
    if (!cont) {
        md_flush(b);
        reset(m);
    } else {
        feed(b, m, (const unsigned char *)" ", 1);
    }
    feed(b, m, (const unsigned char *)text, len);
}

void md_flush(Batch *b)
{
    MdStyle *m = b->md;
    if (!m)
        return;
    if (m->started || m->n > 0)
        step(b, m, 1);
    reset(m);
}

void md_free(Batch *b)
{
    free(b->md);
    b->md = NULL;
}
