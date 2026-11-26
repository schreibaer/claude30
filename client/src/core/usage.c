#include "usage.h"
#include <stdio.h>
#include <string.h>

#define MAX_TOKENS 999999999L   /* nine digits fit a long */

void usage_clear(Usage *u)
{
    u->used = -1;
    u->window = -1;
    u->cost = -1;
}

/* A run of digits at s[*i..n) up to the next space or the end. Returns the
   value, or -2 for something that is not a number; -1 for "-". */
static long field_tokens(const char *s, long n, long *i)
{
    long v = 0, start = *i;
    if (*i < n && s[*i] == '-' && (*i + 1 == n || s[*i + 1] == ' ')) {
        (*i)++;
        return -1;
    }
    while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
        v = v * 10 + (s[*i] - '0');
        if (v > MAX_TOKENS)
            return -2;
        (*i)++;
    }
    if (*i == start || (*i < n && s[*i] != ' '))
        return -2;
    return v;
}

/* "0.4414", "12", "3.5" or "-" into ten-thousandths. */
static long field_cost(const char *s, long n, long *i)
{
    long whole = 0, frac = 0, digits = 0, start = *i;
    if (*i < n && s[*i] == '-' && (*i + 1 == n || s[*i + 1] == ' ')) {
        (*i)++;
        return -1;
    }
    while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
        whole = whole * 10 + (s[*i] - '0');
        if (whole > 99999L)
            return -2;          /* $100,000 or more: not a plausible cost */
        (*i)++;
    }
    if (*i == start)
        return -2;
    if (*i < n && s[*i] == '.') {
        (*i)++;
        while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
            if (digits < 4) {
                frac = frac * 10 + (s[*i] - '0');
                digits++;
            }
            (*i)++;
        }
        if (digits == 0)
            return -2;
        while (digits < 4) {
            frac *= 10;
            digits++;
        }
    }
    if (*i < n && s[*i] != ' ')
        return -2;
    return whole * 10000L + frac;
}

static void skip_space(const char *s, long n, long *i)
{
    while (*i < n && s[*i] == ' ')
        (*i)++;
}

int usage_parse(Usage *u, const char *s, long n)
{
    long i = 0, used, window, cost;
    used = field_tokens(s, n, &i);
    if (used == -2)
        return 0;
    skip_space(s, n, &i);
    window = field_tokens(s, n, &i);
    if (window == -2)
        return 0;
    skip_space(s, n, &i);
    cost = field_cost(s, n, &i);
    if (cost == -2)
        return 0;
    skip_space(s, n, &i);
    if (i != n)
        return 0;
    u->used = used;
    u->window = window > 0 ? window : -1;
    u->cost = cost;
    return 1;
}

int usage_known(const Usage *u)
{
    return u->used >= 0 || u->cost >= 0;
}

int usage_percent(const Usage *u)
{
    long used = u->used, window = u->window;
    if (used < 0 || window <= 0)
        return -1;
    if (used >= window)
        return 100;
    while (window > 10000000L) {   /* keep used * 100 inside a 32-bit long */
        window /= 16;
        used /= 16;
    }
    if (window <= 0)
        return 0;
    return (int)(used * 100 / window);
}

int usage_near_full(const Usage *u)
{
    return usage_percent(u) >= USAGE_NEAR_PERCENT;
}

int usage_fill(const Usage *u, int height)
{
    int pct = usage_percent(u), fill;
    if (pct < 0 || height <= 0)
        return 0;
    fill = (pct * height + 50) / 100;
    if (fill == 0 && u->used > 0)
        fill = 1;
    return fill > height ? height : fill;
}

void usage_status(const Usage *u, int hide_cost, char *out, int max)
{
    int pct = usage_percent(u);
    char tmp[USAGE_STATUS_MAX + 8];
    int len = 0;
    tmp[0] = 0;
    if (pct >= 0)
        len = sprintf(tmp, "ctx %d%%", pct);
    if (u->cost >= 0 && !hide_cost) {
        if (len)
            tmp[len++] = ' ';
        if (u->cost >= 1000000L)
            sprintf(tmp + len, "$%ld", (u->cost + 5000L) / 10000L);
        else {
            long cents = (u->cost + 50L) / 100L;
            sprintf(tmp + len, "$%ld.%02ld", cents / 100L, cents % 100L);
        }
    }
    if (max <= 0)
        return;
    strncpy(out, tmp, (size_t)max - 1);
    out[max - 1] = 0;
}
