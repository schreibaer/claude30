/* Minimal test helpers for the host-built core tests. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { \
        check_failures++; \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_LONG(a, b) do { \
    long check_a_ = (long)(a), check_b_ = (long)(b); \
    check_count++; \
    if (check_a_ != check_b_) { \
        check_failures++; \
        fprintf(stderr, "%s:%d: %s == %ld, expected %ld\n", __FILE__, __LINE__, #a, check_a_, check_b_); \
    } \
} while (0)

#define CHECK_MEM(p, n, s) do { \
    check_count++; \
    if ((long)(n) != (long)strlen(s) || memcmp((p), (s), (n)) != 0) { \
        check_failures++; \
        fprintf(stderr, "%s:%d: got \"%.*s\", expected \"%s\"\n", __FILE__, __LINE__, (int)(n), (const char *)(p), (s)); \
    } \
} while (0)

#define CHECK_DONE() do { \
    printf("%d checks, %d failed\n", check_count, check_failures); \
    return check_failures ? 1 : 0; \
} while (0)

#endif
