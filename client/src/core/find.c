#include "find.h"

static unsigned char gFold[256];
static int gReady;

/* Mac Roman accented capitals and their lower-case forms. */
static const unsigned char kUpper[][2] = {
    { 0xE7, 0x87 }, { 0xCB, 0x88 }, { 0xE5, 0x89 }, { 0x80, 0x8A }, { 0xCC, 0x8B },
    { 0x81, 0x8C }, { 0x82, 0x8D }, { 0x83, 0x8E }, { 0xE9, 0x8F }, { 0xE6, 0x90 },
    { 0xE8, 0x91 }, { 0xEA, 0x92 }, { 0xED, 0x93 }, { 0xEB, 0x94 }, { 0xEC, 0x95 },
    { 0x84, 0x96 }, { 0xEE, 0x97 }, { 0xF1, 0x98 }, { 0xEF, 0x99 }, { 0x85, 0x9A },
    { 0xCD, 0x9B }, { 0xF2, 0x9C }, { 0xF4, 0x9D }, { 0xF3, 0x9E }, { 0x86, 0x9F },
    { 0xAE, 0xBE }, { 0xAF, 0xBF }, { 0xCE, 0xCF },
};

static void init(void)
{
    unsigned i;
    for (i = 0; i < 256; i++)
        gFold[i] = (unsigned char)(i >= 'A' && i <= 'Z' ? i + 32 : i);
    for (i = 0; i < sizeof(kUpper) / sizeof(kUpper[0]); i++)
        gFold[kUpper[i][0]] = kUpper[i][1];
    gReady = 1;
}

unsigned char find_fold(unsigned char c)
{
    if (!gReady)
        init();
    return gFold[c];
}

static int match_at(const char *buf, const char *pat, long plen)
{
    long i;
    for (i = 0; i < plen; i++)
        if (find_fold((unsigned char)buf[i]) != find_fold((unsigned char)pat[i]))
            return 0;
    return 1;
}

long find_forward(const char *buf, long len, long from, const char *pat, long plen)
{
    long s;
    if (plen <= 0 || plen > len)
        return -1;
    if (from < 0)
        from = 0;
    for (s = from; s + plen <= len; s++)
        if (match_at(buf + s, pat, plen))
            return s;
    return -1;
}

long find_backward(const char *buf, long len, long before, const char *pat, long plen)
{
    long s;
    if (plen <= 0 || plen > len)
        return -1;
    if (before > len)
        before = len;
    for (s = before - plen; s >= 0; s--)
        if (match_at(buf + s, pat, plen))
            return s;
    return -1;
}

long find_file_backward(FindRead rd, void *ctx, long size, long before,
                        const char *pat, long plen, char *buf, long bufcap)
{
    long hi, lo, n, r;
    if (plen <= 0 || bufcap < 2 * plen + 1)
        return -1;
    if (before > size)
        before = size;
    hi = before;                        /* the chunk ends here */
    while (hi >= plen) {
        lo = hi - bufcap;
        if (lo < 0)
            lo = 0;
        n = hi - lo;
        if (rd(ctx, lo, n, buf))
            return -2;
        r = find_backward(buf, n, n, pat, plen);
        if (r >= 0)
            return lo + r;
        if (lo == 0)
            break;
        hi = lo + plen - 1;             /* overlap: a match across the edge */
    }
    return -1;
}

long find_file_forward(FindRead rd, void *ctx, long size, long from,
                       const char *pat, long plen, char *buf, long bufcap)
{
    long lo, hi, n, r;
    if (plen <= 0 || bufcap < 2 * plen + 1)
        return -1;
    if (from < 0)
        from = 0;
    lo = from;
    while (lo + plen <= size) {
        hi = lo + bufcap;
        if (hi > size)
            hi = size;
        n = hi - lo;
        if (rd(ctx, lo, n, buf))
            return -2;
        r = find_forward(buf, n, 0, pat, plen);
        if (r >= 0)
            return lo + r;
        if (hi == size)
            break;
        lo = hi - plen + 1;
    }
    return -1;
}
