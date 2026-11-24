/* Line assembler: turns a byte stream into CR-terminated lines of any length.
   Portable C with no Toolbox calls, so it is built for the Mac and for host tests. */
#ifndef LINEBUF_H
#define LINEBUF_H

typedef void (*LineFn)(void *ctx, const char *line, long len);

typedef struct LineBuf {
    char *buf;      /* malloc'd, grows as needed */
    long len;
    long cap;
    int overflow;   /* set when memory ran out and bytes were dropped */
} LineBuf;

void linebuf_init(LineBuf *lb);
void linebuf_free(LineBuf *lb);
void linebuf_reset(LineBuf *lb);
/* Feed bytes; calls fn once per complete line (CR ends a line, LF is ignored).
   The line pointer is valid only during the call. */
void linebuf_feed(LineBuf *lb, const char *data, long n, LineFn fn, void *ctx);

#endif
