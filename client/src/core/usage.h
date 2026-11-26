/* A tab's context and cost reading, as the bridge sends it in a `$` line
   (PROTOCOL.md, Context and cost): the tokens in the conversation, the
   model's context window, and the conversation's running cost. Parsing and
   every number the screen shows come from here, so the Mac code only draws.
   Portable C with no Toolbox calls and no floating point, host-tested. */
#ifndef USAGE_H
#define USAGE_H

/* From this share of the window on, the tab's gauge is drawn solid black:
   Claude Code compacts a conversation not long after. */
#define USAGE_NEAR_PERCENT 80

/* Room for "ctx 100% $123456" and its end. */
#define USAGE_STATUS_MAX 24

typedef struct {
    long used;     /* tokens in the context, or -1 */
    long window;   /* the model's context window in tokens, or -1 */
    long cost;     /* dollars in ten-thousandths (0.4414 is 4414), or -1 */
} Usage;

/* No reading: every field unknown. */
void usage_clear(Usage *u);

/* Reads a payload "<used> <window> <cost>", each field a number or "-"
   (cost has up to four decimals). A malformed payload returns 0 and leaves
   u as it was. A window of 0 counts as unknown. */
int usage_parse(Usage *u, const char *s, long n);

/* True when there is anything to show: a context size or a cost. */
int usage_known(const Usage *u);

/* Whole percent of the window used (0 to 100), or -1 when used or window
   is unknown. */
int usage_percent(const Usage *u);

/* The context is at or past USAGE_NEAR_PERCENT. */
int usage_near_full(const Usage *u);

/* How many of `height` gauge pixels are filled, from the bottom: 0 with no
   reading, at least 1 once any tokens are in use, height at 100%. */
int usage_fill(const Usage *u, int height);

/* The status-line text: "ctx 42% $0.31", "ctx 42%" when hide_cost is set or
   the cost is unknown, "$0.31" when only the cost is known, "" when there
   is nothing to show. Under $100 the cost shows cents, from $100 whole
   dollars. */
void usage_status(const Usage *u, int hide_cost, char *out, int max);

#endif
