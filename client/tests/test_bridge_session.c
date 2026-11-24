/* End-to-end check of the protocol core against the real bridge.py --demo and
   the scripted stress bridge, over pipes instead of a serial line. A small
   stand-in answers the modem commands (AT, the dial string, ATH) so the core
   runs its normal dial, handshake and hangup paths. POSIX only (host test). */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "protocol.h"
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    int to_child, from_child;
    int child_gone;
    char local[256];           /* modem replies waiting to be "received" */
    long nlocal;
    int counts[32];
    long longest_text, longest_code, longest_perm;
    char answer;               /* permission answer */
    int stop_after_text;       /* call proto_stop after this many T lines */
    int disconnect_after_text; /* call proto_disconnect after this many T lines */
    int texts;
    int ready_count;
    char result[64];
} Sess;

static Sess S;
static Proto P;

static unsigned long ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 60UL + (unsigned long)(ts.tv_nsec / 16666667L);
}

static void local_reply(const char *s)
{
    long n = (long)strlen(s);
    memcpy(S.local + S.nlocal, s, n);
    S.nlocal += n;
}

static void on_send(void *ctx, const char *d, long n)
{
    (void)ctx;
    /* The modem's command mode is simulated; online bytes go to the bridge. */
    if (P.state == ST_DIALING) {
        if (n == 3 && memcmp(d, "AT\r", 3) == 0)
            local_reply("OK\r\n");
        else if (n == 1 && d[0] == '\r')
            local_reply("CONNECT 57600\r\n");
        return;
    }
    if (P.state == ST_HANGUP) {
        if (n == 4 && memcmp(d, "ATH\r", 4) == 0)
            local_reply("OK\r\n");
        return;
    }
    if (!S.child_gone && write(S.to_child, d, n) != n)
        S.child_gone = 1;
}

static void on_event(void *ctx, EventKind k, const char *t, long n)
{
    (void)ctx;
    if (getenv("TRACE_EVENTS"))
        fprintf(stderr, "event %d state %d: %.*s\n", (int)k, (int)P.state, (int)(n > 60 ? 60 : n), t);
    S.counts[k]++;
    switch (k) {
    case EV_TEXT:
        S.texts++;
        if (n > S.longest_text)
            S.longest_text = n;
        if (S.stop_after_text && S.texts == S.stop_after_text)
            proto_stop(&P);
        if (S.disconnect_after_text && S.texts == S.disconnect_after_text)
            proto_disconnect(&P, ticks());
        break;
    case EV_CODE:
        if (n > S.longest_code)
            S.longest_code = n;
        break;
    case EV_PERM_LINE:
        if (n > S.longest_perm)
            S.longest_perm = n;
        break;
    case EV_PERM_ASK:
        proto_answer(&P, S.answer);
        break;
    case EV_RESULT:
        if (n > 63)
            n = 63;
        memcpy(S.result, t, n);
        S.result[n] = 0;
        break;
    case EV_READY:
        S.ready_count++;
        break;
    default:
        break;
    }
}

static void start(char *const argv[])
{
    int in[2], out[2];
    memset(&S, 0, sizeof(S));
    S.answer = 'y';
    if (pipe(in) || pipe(out))
        exit(2);
    S.pid = fork();
    if (S.pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    S.to_child = in[1];
    S.from_child = out[0];
    proto_init(&P, on_event, on_send, NULL);
}

/* Runs until done() is true or the time limit passes. Returns 1 if done. */
static int run(int (*done)(void), int seconds)
{
    unsigned long end = ticks() + (unsigned long)seconds * 60;
    char buf[4096];
    while (ticks() < end) {
        fd_set fds;
        struct timeval tv = { 0, 10000 };
        if (done())
            return 1;
        if (S.nlocal) {
            long n = S.nlocal;
            char tmp[256];
            memcpy(tmp, S.local, n);
            S.nlocal = 0;
            proto_feed(&P, tmp, n, ticks());
        }
        if (!S.child_gone) {
            FD_ZERO(&fds);
            FD_SET(S.from_child, &fds);
            if (select(S.from_child + 1, &fds, NULL, NULL, &tv) > 0) {
                ssize_t n = read(S.from_child, buf, sizeof(buf));
                if (n > 0) {
                    proto_feed(&P, buf, n, ticks());
                } else {
                    S.child_gone = 1;
                    /* The WiFi232 reports the closed TCP connection like this. */
                    proto_feed(&P, "\r\nNO CARRIER\r\n", 14, ticks());
                }
            }
        } else {
            { struct timespec ns = { 0, 10000000L }; nanosleep(&ns, NULL); }
        }
        proto_tick(&P, ticks());
        proto_drawn(&P);
    }
    return done();
}

static int is_ready(void) { return P.state == ST_READY; }
static int is_idle(void) { return P.state == ST_IDLE; }
static int turn_done(void) { return P.state == ST_READY && S.ready_count >= 2; }

static int child_exited(void)
{
    int status, i;
    for (i = 0; i < 200; i++) {
        if (waitpid(S.pid, &status, WNOHANG) == S.pid)
            return 1;
        { struct timespec ns = { 0, 10000000L }; nanosleep(&ns, NULL); }
    }
    kill(S.pid, SIGKILL);
    waitpid(S.pid, &status, 0);
    return 0;
}

static void connect_and_ready(void)
{
    proto_connect(&P, "ATDTbridge", ticks());
    CHECK(run(is_ready, 20));
    S.ready_count = 1;
}

static void finish_with_disconnect(void)
{
    if (P.state == ST_READY)
        proto_disconnect(&P, ticks());
    CHECK(run(is_idle, 20));
    CHECK(S.counts[EV_CLOSED] == 1);
    CHECK_EQ_LONG(S.counts[EV_LOST], 0);
    CHECK(child_exited());           /* the bridge did not hang on exit */
    close(S.to_child);
    close(S.from_child);
}

static char *demo_argv[] = { "python3", "-u", BRIDGE_PY, "--demo", "--baud", "0", NULL };
static char *stress_argv[] = { "python3", "-u", STRESS_PY, "--baud", "0", NULL };

static void test_demo_turn(void)
{
    start(demo_argv);
    connect_and_ready();
    CHECK(proto_send_prompt(&P, "hello\rthere", 11));
    CHECK(run(turn_done, 20));
    CHECK_EQ_LONG(S.counts[EV_PERM_ASK], 1);
    CHECK_EQ_LONG(S.counts[EV_PERM_LINE], 4);
    CHECK(strcmp(S.result, "success $0.0000") == 0);
    CHECK_EQ_LONG(S.counts[EV_CODE_BEGIN], 1);
    finish_with_disconnect();
}

static void test_demo_stop(void)
{
    start(demo_argv);
    connect_and_ready();
    S.stop_after_text = 10;
    proto_send_prompt(&P, "go", 2);
    CHECK(run(turn_done, 20));
    CHECK_EQ_LONG(S.counts[EV_INTERRUPTED], 1);
    CHECK_EQ_LONG(S.counts[EV_PERM_ASK], 0);
    finish_with_disconnect();
}

static void stress_case(const char *prompt)
{
    start(stress_argv);
    connect_and_ready();
    CHECK(proto_send_prompt(&P, prompt, (long)strlen(prompt)));
    CHECK(run(turn_done, 30));
}

static void test_stress_cases(void)
{
    stress_case("perm");
    CHECK_EQ_LONG(S.counts[EV_PERM_LINE], 2000);
    CHECK(strcmp(S.result, "success $0.0000") == 0);
    finish_with_disconnect();

    stress_case("wide");
    CHECK_EQ_LONG(S.counts[EV_PERM_LINE], 1);
    CHECK_EQ_LONG(S.longest_perm, 3000);
    finish_with_disconnect();

    stress_case("long");
    CHECK(S.longest_text > 4000);
    CHECK(S.longest_code > 4500);
    CHECK(S.longest_perm > 4200);
    finish_with_disconnect();

    stress_case("error");
    CHECK_EQ_LONG(S.counts[EV_RESULT], 0);
    CHECK_EQ_LONG(S.counts[EV_CODE_END], 1);
    CHECK_EQ_LONG(S.counts[EV_ERROR], 1);
    finish_with_disconnect();

    stress_case("burst");
    CHECK_EQ_LONG(S.texts, 600);
    finish_with_disconnect();
}

static void test_disconnect_mid_turn(void)
{
    start(stress_argv);
    connect_and_ready();
    S.disconnect_after_text = 20;
    proto_send_prompt(&P, "stream", 6);
    CHECK(run(is_idle, 30));
    CHECK(S.counts[EV_CLOSED] == 1);
    CHECK(S.texts < 400);            /* the turn was stopped, not run to the end */
    CHECK(child_exited());
    close(S.to_child);
    close(S.from_child);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_demo_turn();
    test_demo_stop();
    test_stress_cases();
    test_disconnect_mid_turn();
    proto_free(&P);
    CHECK_DONE();
}
