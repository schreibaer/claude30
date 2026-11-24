#include "linebuf.h"
#include <stdlib.h>
#include <string.h>

#define LB_INITIAL 256

void linebuf_init(LineBuf *lb)
{
    lb->buf = NULL;
    lb->len = 0;
    lb->cap = 0;
    lb->overflow = 0;
}

void linebuf_free(LineBuf *lb)
{
    free(lb->buf);
    linebuf_init(lb);
}

void linebuf_reset(LineBuf *lb)
{
    lb->len = 0;
    lb->overflow = 0;
}

static int linebuf_put(LineBuf *lb, char c)
{
    if (lb->len >= lb->cap) {
        long ncap = lb->cap ? lb->cap * 2 : LB_INITIAL;
        char *nb = (char *)realloc(lb->buf, ncap);
        if (!nb) {
            lb->overflow = 1;
            return 0;
        }
        lb->buf = nb;
        lb->cap = ncap;
    }
    lb->buf[lb->len++] = c;
    return 1;
}

void linebuf_feed(LineBuf *lb, const char *data, long n, LineFn fn, void *ctx)
{
    long i;
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n')
            continue;
        if (c == '\r') {
            fn(ctx, lb->buf ? lb->buf : "", lb->len);
            lb->len = 0;
            lb->overflow = 0;
            /* Give back very large buffers once the long line is done. */
            if (lb->cap > 16384) {
                free(lb->buf);
                lb->buf = NULL;
                lb->cap = 0;
            }
            continue;
        }
        linebuf_put(lb, c);
    }
}
