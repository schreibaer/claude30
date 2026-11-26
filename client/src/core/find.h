/* Find: case-insensitive search over Mac Roman text, forward and backward,
   in a buffer or in a file read in chunks (the tab's log). Letter case folds
   with a 256-byte table (A-Z and the accented letters of Mac Roman), not the
   Script Manager, so the host tests and the Mac agree.
   Portable C with no Toolbox calls, host-tested. */
#ifndef FIND_H
#define FIND_H

/* The lower-case form of c in Mac Roman. */
unsigned char find_fold(unsigned char c);

/* The first match that starts at or after from, or -1. An empty pattern or
   one longer than the buffer never matches. */
long find_forward(const char *buf, long len, long from, const char *pat, long plen);
/* The last match that ends at or before before, or -1. */
long find_backward(const char *buf, long len, long before, const char *pat, long plen);

/* Copies [off, off+len) of the file to out; returns 0 on success. */
typedef int (*FindRead)(void *ctx, long off, long len, char *out);

/* The same searches over a file of size bytes, read through rd into buf
   (bufcap bytes, at least 2 * plen + 1). A match that crosses a chunk edge is
   found. Returns the match's start, -1 for none, -2 when rd failed. */
long find_file_backward(FindRead rd, void *ctx, long size, long before,
                        const char *pat, long plen, char *buf, long bufcap);
long find_file_forward(FindRead rd, void *ctx, long size, long from,
                       const char *pat, long plen, char *buf, long bufcap);

#endif
