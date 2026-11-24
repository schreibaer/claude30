/* File transfers: listings, piece reads with their checks and retries, block
   saves with resends, conflicts and forced saves, timeouts and link loss.
   The bridge side is played by hand here; test_bridge_session runs the same
   paths against bridge.py. */
#include "check.h"
#include "protocol.h"
#include "files.h"
#include "crc32.h"

#define MAXEV 256

typedef struct {
    EventKind kind[MAXEV];
    int sid[MAXEV];
    int slot[MAXEV];
    long owner[MAXEV];
    char text[MAXEV][160];
    long len[MAXEV];
    int n;
    char sent[16384];
    long nsent;
    Proto *p;
} Rec;

static Rec R;
static Proto P;
static unsigned long now;

static void on_event(void *ctx, int sid, EventKind k, const char *t, long n)
{
    Rec *r = (Rec *)ctx;
    const FileXfer *x = proto_xfer(r->p);
    if (r->n < MAXEV) {
        r->kind[r->n] = k;
        r->sid[r->n] = sid;
        r->slot[r->n] = proto_xfer_slot(r->p);
        r->owner[r->n] = x ? x->owner : -1;
        r->len[r->n] = n;
        memcpy(r->text[r->n], t, n < 159 ? n : 159);
        r->text[r->n][n < 159 ? n : 159] = 0;
        r->n++;
    }
}

static void on_send(void *ctx, const char *d, long n)
{
    Rec *r = (Rec *)ctx;
    if (r->nsent + n < (long)sizeof(r->sent)) {
        memcpy(r->sent + r->nsent, d, n);
        r->nsent += n;
    }
}

static void clear_rec(void)
{
    R.n = 0;
    R.nsent = 0;
}

static void feed(const char *s)
{
    proto_feed(&P, s, (long)strlen(s), now);
    proto_drawn(&P);
}

static void tick(unsigned long ticks)
{
    unsigned long end = now + ticks;
    while (now < end) {
        now++;
        proto_tick(&P, now);
    }
}

static int find(EventKind k)
{
    int i;
    for (i = 0; i < R.n; i++)
        if (R.kind[i] == k)
            return i;
    return -1;
}

static int count(EventKind k)
{
    int i, c = 0;
    for (i = 0; i < R.n; i++)
        if (R.kind[i] == k)
            c++;
    return c;
}

static int count_sent(const char *s)
{
    long k = (long)strlen(s), i;
    int c = 0;
    for (i = 0; i + k <= R.nsent; i++)
        if (memcmp(R.sent + i, s, k) == 0)
            c++;
    return c;
}

static void crc_hex(const char *t, long n, char *out)
{
    snprintf(out, 9, "%08lx", crc32_buf(t, n));
}

/* Online, files=<mode>, session 1 open in alpha. */
static void setup(const char *mode)
{
    char buf[160];
    proto_free(&P);
    memset(&R, 0, sizeof(R));
    R.p = &P;
    proto_init(&P, on_event, on_send, &R);
    now = 1000;
    proto_connect(&P, "ATDTbridge", now);
    feed("OK\r\nCONNECT 57600\r\n");
    snprintf(buf, sizeof(buf), "#OK se30bridge 2 files=%s\r\nP[\r\nP alpha\r\nP]\r\n", mode);
    feed(buf);
    CHECK_EQ_LONG(P.state, ST_ONLINE);
    proto_open(&P, 1, "alpha");
    feed("1O alpha\r\n1>\r\n");
    CHECK_EQ_LONG(proto_sess_state(&P, 1), SS_READY);
    clear_rec();
}

static void test_crc32(void)
{
    char hex[9];
    crc_hex("123456789", 9, hex);
    CHECK_MEM(hex, 8, "cbf43926");
    CHECK_EQ_LONG(crc32_buf("", 0), 0);
    CHECK_EQ_LONG(crc32_end(crc32_add(crc32_add(CRC32_INIT, "1234", 4), "56789", 5)),
                  0xCBF43926UL);
}

static void test_modes(void)
{
    setup("ro");
    CHECK_EQ_LONG(proto_files_mode(&P), FILES_RO);
    CHECK_EQ_LONG(proto_save(&P, 1, 7, "a.txt", "abc", "x", 1), -1);
    CHECK(proto_read(&P, 1, 7, "a.txt", 0, 10, 0) >= 0);

    setup("off");
    CHECK_EQ_LONG(proto_files_mode(&P), FILES_OFF);
    CHECK_EQ_LONG(proto_list(&P, 1, ""), 0);
    CHECK_EQ_LONG(proto_read(&P, 1, 7, "a.txt", 0, 10, 0), -1);
    CHECK_EQ_LONG(R.nsent, 0);

    setup("rw");
    CHECK_EQ_LONG(proto_files_mode(&P), FILES_RW);
    /* No such session. */
    CHECK_EQ_LONG(proto_read(&P, 2, 7, "a.txt", 0, 10, 0), -1);
    CHECK_EQ_LONG(proto_list(&P, 2, ""), 0);
}

static void test_listing(void)
{
    int is_dir;
    long size, nlen;
    const char *name;

    setup("rw");
    CHECK(proto_list(&P, 1, "src"));
    CHECK_MEM(R.sent, R.nsent, "1L src\r");
    feed("1L[ src\r\n1L d 0 lib\r\n1L f 1234 my file.c\r\n1L]\r\n");
    CHECK_EQ_LONG(count(EV_LIST_BEGIN), 1);
    CHECK_EQ_LONG(count(EV_LIST_ENTRY), 2);
    CHECK_EQ_LONG(count(EV_LIST_END), 1);
    CHECK(files_parse_entry(R.text[2], R.len[2], &is_dir, &size, &name, &nlen));
    CHECK_EQ_LONG(is_dir, 0);
    CHECK_EQ_LONG(size, 1234);
    CHECK_MEM(name, nlen, "my file.c");
    CHECK(!files_parse_entry("x 1 a", 5, &is_dir, &size, &name, &nlen));
    CHECK(!files_parse_entry("f 1 ", 4, &is_dir, &size, &name, &nlen));
    CHECK(!files_parse_entry("f z a", 5, &is_dir, &size, &name, &nlen));

    /* Lines after the end, or for a listing nobody asked for, are ignored. */
    clear_rec();
    feed("1L f 1 late\r\n1L]\r\n");
    CHECK_EQ_LONG(R.n, 0);

    /* Refused. */
    CHECK(proto_list(&P, 1, "../etc"));
    feed("1N - That is outside the project.\r\n");
    CHECK_EQ_LONG(count(EV_LIST_FAILED), 1);
    CHECK_MEM(R.text[0], R.len[0], "That is outside the project.");

    /* Cut short. */
    clear_rec();
    proto_list(&P, 1, "");
    feed("1L[ \r\n1L f 1 a\r\n1L] cut\r\n");
    CHECK_MEM(R.text[R.n - 1], R.len[R.n - 1], "cut");

    /* No answer. */
    clear_rec();
    proto_list(&P, 1, "");
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count(EV_LIST_FAILED), 1);
}

static void test_read(void)
{
    char buf[256], hex[9];
    int slot, i;
    const char body[] = "line one\rline two is longer\r";

    setup("rw");
    slot = proto_read(&P, 1, 42, "src/a.c", 0, 3, 0);
    CHECK(slot >= 0);
    CHECK_MEM(R.sent, R.nsent, "1G 1 0 3 4096 src/a.c\r");

    crc_hex(body, sizeof(body) - 1, hex);
    snprintf(buf, sizeof(buf),
             "1H 1 0 3 3 0123456789ab e\r\n1B line one\r\n1B line two\r\n1+  is longer\r\n1B\r\n"
             "1Z 1 %ld %s\r\n", (long)sizeof(body) - 1, hex);
    feed(buf);
    i = find(EV_PIECE);
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_MEM(R.text[i], R.len[i], body);
        CHECK_EQ_LONG(R.owner[i], 42);
        CHECK_EQ_LONG(R.slot[i], slot);
    }
    CHECK_EQ_LONG(P.xfer[slot].kind, XF_NONE);
    CHECK_EQ_LONG(P.dropped, 0);

    /* The header fields reach the shell during EV_PIECE. */
    setup("rw");
    slot = proto_read(&P, 1, 1, "big.txt", 10, 1, 100);
    crc_hex("x", 1, hex);
    snprintf(buf, sizeof(buf), "1H 1 10 1 5000 abcdefabcdef r:big\r\n1B x\r\n1Z 1 1 %s\r\n", hex);
    feed(buf);
    CHECK_EQ_LONG(count(EV_PIECE), 1);
    CHECK_EQ_LONG(P.xfer[slot].h_total, 5000);
    CHECK_MEM(P.xfer[slot].mode, strlen(P.xfer[slot].mode), "r:big");
    CHECK_MEM(P.xfer[slot].token, strlen(P.xfer[slot].token), "abcdefabcdef");

    /* Refused. */
    setup("rw");
    proto_read(&P, 1, 1, "nope", 0, 10, 0);
    feed("1N 1 No such file.\r\n");
    CHECK_EQ_LONG(count(EV_READ_FAILED), 1);
    CHECK_MEM(R.text[0], R.len[0], "No such file.");
}

static void test_read_retries(void)
{
    char buf[256], hex[9];
    int slot;

    /* A bad CRC asks again with a new number; the old number's lines are dropped. */
    setup("rw");
    slot = proto_read(&P, 1, 1, "a", 0, 1, 0);
    feed("1H 1 0 1 1 tok e\r\n1B hello\r\n1Z 1 5 00000000\r\n");
    CHECK_EQ_LONG(count(EV_PIECE), 0);
    CHECK_EQ_LONG(count_sent("1G 2 0 1 4096 a\r"), 1);
    feed("1H 1 0 1 1 tok e\r\n");          /* late answer to the first try */
    CHECK_EQ_LONG(P.xfer[slot].phase, 0);   /* still waiting for H 2 */
    crc_hex("hello", 5, hex);
    snprintf(buf, sizeof(buf), "1H 2 0 1 1 tok e\r\n1B hello\r\n1Z 2 5 %s\r\n", hex);
    feed(buf);
    CHECK_EQ_LONG(count(EV_PIECE), 1);

    /* Missing lines fail the count check. */
    setup("rw");
    proto_read(&P, 1, 1, "a", 0, 2, 0);
    crc_hex("hello", 5, hex);
    snprintf(buf, sizeof(buf), "1H 1 0 2 2 tok e\r\n1B hello\r\n1Z 1 5 %s\r\n", hex);
    feed(buf);
    CHECK_EQ_LONG(count(EV_PIECE), 0);
    CHECK_EQ_LONG(count_sent("1G 2 "), 1);

    /* Serial errors during a piece fail it even when the CRC matches. */
    setup("rw");
    proto_read(&P, 1, 1, "a", 0, 1, 0);
    feed("1H 1 0 1 1 tok e\r\n1B hello\r\n");
    proto_line_noise(&P);
    snprintf(buf, sizeof(buf), "1Z 1 5 %s\r\n", hex);
    feed(buf);
    CHECK_EQ_LONG(count(EV_PIECE), 0);
    CHECK_EQ_LONG(count_sent("1G 2 "), 1);

    /* No answer: a spare !ACK, then ask again; three tries, then give up. */
    setup("rw");
    proto_read(&P, 1, 1, "a", 0, 1, 0);
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count_sent("!ACK\r"), 1);
    CHECK_EQ_LONG(count_sent("1G 2 "), 1);
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count_sent("1G 3 "), 1);
    CHECK_EQ_LONG(count(EV_READ_FAILED), 0);
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count(EV_READ_FAILED), 1);
    CHECK_EQ_LONG(count_sent("1G 4 "), 0);
}

/* Bytes of the S lines between S[ and S] in R.sent, joined with CR. */
static long sent_block(long from, char *out, long cap, long *after)
{
    long i = from, n = 0;
    while (i < R.nsent && memcmp(R.sent + i, "1S[ ", 4) != 0)
        i++;
    while (i < R.nsent && R.sent[i] != '\r')
        i++;
    i++;
    while (i + 3 <= R.nsent && memcmp(R.sent + i, "1S ", 3) == 0) {
        long s = i + 3;
        i = s;
        while (i < R.nsent && R.sent[i] != '\r')
            i++;
        if (n && n < cap)
            out[n++] = '\r';
        if (n + (i - s) <= cap) {
            memcpy(out + n, R.sent + s, i - s);
            n += i - s;
        }
        i++;
    }
    *after = i;
    return n;
}

static void test_save(void)
{
    static char text[6000], got[6000], all[6000];
    char buf[128], hex[9];
    long i, n, at = 0, total = 0;
    int slot, blocks = 0;

    setup("rw");
    /* 100 lines of 50 bytes: three blocks of about 2 KB. */
    for (i = 0; i < 100; i++) {
        memset(text + i * 51, 'a' + i % 26, 50);
        text[i * 51 + 50] = '\r';
    }
    n = 100 * 51 - 1;
    slot = proto_save(&P, 1, 9, "src/a.c", "0123456789ab", text, n);
    CHECK(slot >= 0);
    CHECK_MEM(R.sent, R.nsent, "1F 1 0123456789ab src/a.c\r");
    CHECK_EQ_LONG(P.xfer[slot].nblocks, 3);

    /* Nothing is sent before K 0. */
    proto_files_step(&P);
    CHECK_EQ_LONG(count_sent("1S[ "), 0);
    feed("1K 1 0\r\n");
    for (;;) {
        long k, before = R.nsent;
        proto_files_step(&P);
        if (R.nsent == before)
            break;
        blocks++;
        k = sent_block(at, got, sizeof(got), &at);
        CHECK(k <= SAVE_BLOCK + 60);
        memcpy(all + total, got, k);
        total += k;
        if (total < n)
            all[total++] = '\r';
        /* The block's own trailer. */
        crc_hex(got, k, hex);
        snprintf(buf, sizeof(buf), "1S] 1 %d %ld %s\r", blocks, k, hex);
        CHECK(count_sent(buf) == 1);
        snprintf(buf, sizeof(buf), "1K 1 %d\r\n", blocks);
        feed(buf);
    }
    CHECK_EQ_LONG(blocks, 3);
    CHECK_EQ_LONG(count(EV_SAVE_PROGRESS), 3);
    CHECK_EQ_LONG(total, n);
    CHECK(memcmp(all, text, n) == 0);
    crc_hex(text, n, hex);
    snprintf(buf, sizeof(buf), "1S. 1 %ld %s\r", n, hex);
    CHECK_EQ_LONG(count_sent(buf), 1);
    feed("1K 1 saved fedcba987654\r\n");
    i = find(EV_SAVED);
    CHECK(i >= 0);
    if (i >= 0) {
        CHECK_MEM(R.text[i], R.len[i], "fedcba987654");
        CHECK_EQ_LONG(R.owner[i], 9);
    }
    CHECK_EQ_LONG(P.xfer[slot].kind, XF_NONE);

    /* An empty file is one empty block. */
    setup("rw");
    proto_save(&P, 1, 1, "e", "tok", "", 0);
    feed("1K 1 0\r\n");
    proto_files_step(&P);
    CHECK_EQ_LONG(count_sent("1S[ 1 1\r1S \r1S] 1 1 0 00000000\r"), 1);
}

static void test_save_resend_and_conflict(void)
{
    int slot;

    /* A damaged block is sent again; three refusals give up. */
    setup("rw");
    slot = proto_save(&P, 1, 1, "a", "tok", "hi", 2);
    feed("1K 1 0\r\n");
    proto_files_step(&P);
    CHECK_EQ_LONG(count_sent("1S[ 1 1\r"), 1);
    feed("1N 1 1 resend\r\n");
    proto_files_step(&P);
    CHECK_EQ_LONG(count_sent("1S[ 1 1\r"), 2);
    feed("1N 1 1 resend\r\n");
    proto_files_step(&P);
    feed("1N 1 1 resend\r\n");
    CHECK_EQ_LONG(count(EV_SAVE_FAILED), 1);
    CHECK_EQ_LONG(P.xfer[slot].kind, XF_NONE);

    /* Conflict, then force. */
    setup("rw");
    slot = proto_save(&P, 1, 1, "a", "tok", "hi", 2);
    feed("1K 1 0\r\n");
    proto_files_step(&P);
    feed("1K 1 1\r\n");
    feed("1N 1 changed\r\n");
    CHECK_EQ_LONG(count(EV_SAVE_CONFLICT), 1);
    tick(T_FILE * 3);                       /* the shell may take its time */
    CHECK_EQ_LONG(count(EV_SAVE_FAILED), 0);
    CHECK(proto_save_force(&P, slot));
    CHECK_EQ_LONG(count_sent(" force\r"), 1);
    feed("1K 1 saved newtok\r\n");
    CHECK_EQ_LONG(count(EV_SAVED), 1);
    CHECK_EQ_LONG(proto_save_force(&P, slot), 0);

    /* Other refusals end the save with the bridge's reason. */
    setup("rw");
    proto_save(&P, 1, 1, "a", "tok", "hi", 2);
    feed("1N 1 The file is read-only.\r\n");
    CHECK_EQ_LONG(count(EV_SAVE_FAILED), 1);
    CHECK_MEM(R.text[0], R.len[0], "The file is read-only.");

    /* No answer to F: !ACK and F again; third time gives up. */
    setup("rw");
    proto_save(&P, 1, 1, "a", "tok", "hi", 2);
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count_sent("1F 1 tok a\r"), 2);
    CHECK_EQ_LONG(count_sent("!ACK\r"), 1);
    tick(T_FILE + 1);
    tick(T_FILE + 1);
    CHECK_EQ_LONG(count(EV_SAVE_FAILED), 1);
}

static void test_close_and_loss(void)
{
    int a, b;

    setup("rw");
    proto_open(&P, 2, "alpha");
    feed("2O alpha\r\n2>\r\n");
    a = proto_read(&P, 1, 1, "a", 0, 1, 0);
    b = proto_read(&P, 2, 2, "b", 0, 1, 0);
    proto_list(&P, 1, "");
    clear_rec();
    proto_close(&P, 1);
    CHECK_EQ_LONG(count(EV_READ_FAILED), 1);
    CHECK_EQ_LONG(count(EV_LIST_FAILED), 1);
    CHECK_EQ_LONG(P.xfer[a].kind, XF_NONE);
    CHECK_EQ_LONG(P.xfer[b].kind, XF_READ);
    /* Session 1 is closing: no new transfers there. */
    CHECK_EQ_LONG(proto_read(&P, 1, 1, "a", 0, 1, 0), -1);

    clear_rec();
    feed("NO CARRIER\r\n");
    CHECK_EQ_LONG(count(EV_READ_FAILED), 1);
    CHECK_EQ_LONG(P.xfer[b].kind, XF_NONE);

    /* The table fills up, and a cancel frees a slot without events. */
    setup("rw");
    for (a = 0; a < MAX_XFER; a++)
        CHECK(proto_read(&P, 1, a, "a", 0, 1, 0) >= 0);
    CHECK_EQ_LONG(proto_read(&P, 1, 99, "a", 0, 1, 0), -1);
    clear_rec();
    proto_xfer_cancel(&P, 3);
    CHECK_EQ_LONG(R.n, 0);
    CHECK(proto_read(&P, 1, 99, "a", 0, 1, 0) == 3);
}

int main(void)
{
    test_crc32();
    test_modes();
    test_listing();
    test_read();
    test_read_retries();
    test_save();
    test_save_resend_and_conflict();
    test_close_and_loss();
    proto_free(&P);
    CHECK_DONE();
}
