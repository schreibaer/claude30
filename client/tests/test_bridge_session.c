/* End-to-end check of the protocol core against the real bridge.py --demo and
   the scripted stress bridge, over pipes instead of a serial line: two
   sessions at once, permission requests, Stop, the stress cases, and file
   listing, reading and saving (with damaged transfers and a conflict).
   A small stand-in answers the modem commands (AT, the dial string, ATH) so
   the core runs its normal dial, handshake and hangup paths. POSIX only. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "protocol.h"
#include "files.h"
#include <signal.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int counts[EV_COUNT];
    long longest_text, longest_code, longest_perm;
    int stop_after_text;       /* call proto_stop after this many T lines */
    int disconnect_after_text; /* call proto_disconnect after this many T lines */
    int texts;
    int readies;
    char result[64];
    char opened[64];
    char how[16], token[24];   /* from EV_RESUMED: "<how> <token>" */
    char model[48];            /* the latest EV_MODEL text */
    char said[512];            /* the first paragraph of the latest turn */
    int said_open;             /* ... still being continued */
    int kill_after_text;       /* SIGKILL the bridge after this many T lines */
} SessRec;

typedef struct {
    pid_t pid;
    int to_child, from_child;
    int child_gone;
    char local[256];           /* modem replies waiting to be "received" */
    long nlocal;
    int link[EV_COUNT];
    SessRec s[MAX_SESS + 1];
    char answer;               /* permission answer */
    /* the last file event */
    int pieces, saved, failed, conflicts, progress, list_entries, list_done;
    char piece[16384];
    long piece_len;
    char token[24], mode[24];
    long total;
    int last_slot;
    char why[128];
} Run;

static Run S;
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

static void copy_text(char *dst, long cap, const char *t, long n)
{
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, t, n);
    dst[n] = 0;
}

static void on_event(void *ctx, int sid, EventKind k, const char *t, long n)
{
    SessRec *r = &S.s[sid];
    const FileXfer *x = proto_xfer(&P);
    (void)ctx;
    if (getenv("TRACE_EVENTS"))
        fprintf(stderr, "%d: event %d: %.*s\n", sid, (int)k, (int)(n > 60 ? 60 : n), t);
    if (sid == 0)
        S.link[k]++;
    r->counts[k]++;
    switch (k) {
    case EV_TEXT:
        r->texts++;
        if (n > r->longest_text)
            r->longest_text = n;
        if (r->stop_after_text && r->texts == r->stop_after_text)
            proto_stop(&P, sid);
        if (r->disconnect_after_text && r->texts == r->disconnect_after_text)
            proto_disconnect(&P, ticks());
        if (r->kill_after_text && r->texts == r->kill_after_text)
            kill(S.pid, SIGKILL);
        if (n >= 8 && memcmp(t, "You said", 8) == 0) {
            r->said_open = 1;
            copy_text(r->said, sizeof(r->said), t, n);
        } else if (r->said_open) {
            /* the prompt had a line break: its echo spans paragraphs */
            size_t have = strlen(r->said);
            if (have + n + 1 < sizeof(r->said)) {
                r->said[have] = ' ';
                memcpy(r->said + have + 1, t, n);
                r->said[have + 1 + n] = 0;
            }
        }
        break;
    case EV_TOOL:
        r->said_open = 0;
        break;
    case EV_TEXT_CONT:
        if (r->said_open) {
            size_t have = strlen(r->said);
            if (have + n < sizeof(r->said)) {
                memcpy(r->said + have, t, n);
                r->said[have + n] = 0;
            }
        }
        break;
    case EV_RESUMED: {
        const char *sp = memchr(t, ' ', n);
        if (sid >= 1 && sp) {
            copy_text(r->how, sizeof(r->how), t, sp - t);
            copy_text(r->token, sizeof(r->token), sp + 1, n - (sp + 1 - t));
        }
        break;
    }
    case EV_MODEL:
        copy_text(r->model, sizeof(r->model), t, n);
        break;
    case EV_CODE:
        if (n > r->longest_code)
            r->longest_code = n;
        break;
    case EV_PERM_LINE:
        if (n > r->longest_perm)
            r->longest_perm = n;
        break;
    case EV_PERM_ASK:
        proto_answer(&P, sid, S.answer);
        break;
    case EV_RESULT:
        copy_text(r->result, sizeof(r->result), t, n);
        break;
    case EV_READY:
        r->readies++;
        break;
    case EV_OPENED:
        copy_text(r->opened, sizeof(r->opened), t, n);
        break;
    case EV_LIST_ENTRY:
        S.list_entries++;
        break;
    case EV_LIST_END:
        S.list_done = 1;
        break;
    case EV_LIST_FAILED:
        S.list_done = -1;
        break;
    case EV_PIECE:
        S.pieces++;
        copy_text(S.piece, sizeof(S.piece), t, n);
        S.piece_len = n < (long)sizeof(S.piece) - 1 ? n : (long)sizeof(S.piece) - 1;
        copy_text(S.token, sizeof(S.token), x->token, (long)strlen(x->token));
        copy_text(S.mode, sizeof(S.mode), x->mode, (long)strlen(x->mode));
        S.total = x->h_total;
        break;
    case EV_SAVE_PROGRESS:
        S.progress++;
        break;
    case EV_SAVED:
        S.saved++;
        copy_text(S.token, sizeof(S.token), t, n);
        break;
    case EV_SAVE_CONFLICT:
        S.conflicts++;
        S.last_slot = proto_xfer_slot(&P);
        break;
    case EV_READ_FAILED:
    case EV_SAVE_FAILED:
        S.failed++;
        copy_text(S.why, sizeof(S.why), t, n);
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
    proto_free(&P);
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
            struct timespec ns = { 0, 10000000L };
            nanosleep(&ns, NULL);
        }
        proto_tick(&P, ticks());
        proto_files_step(&P);
        proto_drawn(&P);
    }
    return done();
}

static int is_online(void) { return P.state == ST_ONLINE; }
static int is_idle(void) { return P.state == ST_IDLE; }
static int both_ready(void)
{
    return proto_sess_state(&P, 1) == SS_READY && proto_sess_state(&P, 2) == SS_READY;
}
static int both_done(void) { return S.s[1].readies >= 1 && S.s[2].readies >= 1; }
static int one_done(void) { return S.s[1].readies >= 1; }
static int list_done(void) { return S.list_done != 0; }
static int piece_or_fail(void) { return S.pieces > 0 || S.failed > 0; }
static int saved_or_fail(void) { return S.saved > 0 || S.failed > 0 || S.conflicts > 0; }

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

/* Dials, then opens session 1 in project a and session 2 in project b. */
static void connect_two(const char *a, const char *b)
{
    proto_connect(&P, "ATDTbridge", ticks());
    CHECK(run(is_online, 20));
    CHECK(proto_open(&P, 1, a));
    CHECK(proto_open(&P, 2, b));
    CHECK(run(both_ready, 20));
    CHECK(strcmp(S.s[1].opened, a) == 0);
    CHECK(strcmp(S.s[2].opened, b) == 0);
    S.s[1].readies = S.s[2].readies = 0;   /* each open ends with EV_READY */
}

static void finish_with_disconnect(void)
{
    if (P.state == ST_ONLINE)
        proto_disconnect(&P, ticks());
    CHECK(run(is_idle, 20));
    CHECK_EQ_LONG(S.link[EV_CLOSED], 1);
    CHECK_EQ_LONG(S.link[EV_LOST], 0);
    CHECK(child_exited());           /* the bridge did not hang on exit */
    close(S.to_child);
    close(S.from_child);
}

static char projects_root[] = "/tmp/se30testXXXXXX";
/* A hidden folder inside the projects root: not offered as a project, and
   no other test's bridge shares it. */
static char state_dir[sizeof(projects_root) + 8];
static char *demo_argv[] = { "python3", "-u", BRIDGE_PY, "--demo", "--baud", "0",
                             "--projects", projects_root, "--state-dir", state_dir, NULL };
static char *stress_argv[] = { "python3", "-u", STRESS_PY, "--baud", "0", NULL };

static void make_projects(void)
{
    char path[64];
    if (!mkdtemp(projects_root))
        exit(2);
    snprintf(state_dir, sizeof(state_dir), "%s/.state", projects_root);
    snprintf(path, sizeof(path), "%s/alpha", projects_root);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/beta", projects_root);
    mkdir(path, 0700);
}

static void remove_projects(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/alpha", projects_root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/beta", projects_root);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/call.lock", state_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/sessions.json", state_dir);
    unlink(path);
    rmdir(state_dir);
    rmdir(projects_root);
}

static void test_demo_two_sessions(void)
{
    start(demo_argv);
    connect_two("alpha", "beta");
    CHECK_EQ_LONG(proto_nprojects(&P), 2);
    CHECK(proto_send_prompt(&P, 1, "hello\rthere", 11));
    CHECK(proto_send_prompt(&P, 2, "second", 6));
    CHECK(run(both_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_ASK], 1);
    CHECK_EQ_LONG(S.s[2].counts[EV_PERM_ASK], 1);
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_LINE], 4);
    CHECK(strcmp(S.s[1].result, "success $0.0000") == 0);
    CHECK(strcmp(S.s[2].result, "success $0.0000") == 0);
    CHECK_EQ_LONG(S.s[1].counts[EV_CODE_BEGIN], 1);
    CHECK_EQ_LONG(S.s[2].counts[EV_CODE_BEGIN], 1);
    CHECK_EQ_LONG(P.dropped, 0);

    /* Closing one tab leaves the other working. */
    CHECK(proto_close(&P, 2));
    S.s[1].readies = 0;
    CHECK(proto_send_prompt(&P, 1, "again", 5));
    CHECK(run(one_done, 20));
    CHECK_EQ_LONG(S.s[2].counts[EV_SESSION_CLOSED], 1);
    CHECK_EQ_LONG(proto_sess_state(&P, 2), SS_NONE);
    CHECK_EQ_LONG(S.s[1].counts[EV_READY], 3);   /* open and two turns */
    finish_with_disconnect();
}


/* The demo bridge's two overlapping fake subagents: start, progress and end
   events for each, a labelled permission request, and nothing else. */
static void test_demo_agents(void)
{
    start(demo_argv);
    connect_two("alpha", "beta");
    CHECK(proto_send_prompt(&P, 1, "run the agents", 14));
    CHECK(run(one_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_AGENT_START], 2);
    CHECK_EQ_LONG(S.s[1].counts[EV_AGENT_END], 2);
    CHECK(S.s[1].counts[EV_AGENT_STEP] >= 2);
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_ASK], 1);
    CHECK_EQ_LONG(S.s[1].counts[EV_TOOL], 1);   /* only the main conversation's own tool line */
    CHECK(strcmp(S.s[1].result, "success $0.0000") == 0);
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

/* A background subagent ends after its turn: its lines and Claude's own
   follow-up arrive while the session is ready, and nothing is dropped. */
static int bg_done(void) { return S.s[1].counts[EV_AGENT_END] >= 2 && S.s[1].counts[EV_RESULT] >= 2; }

static void test_demo_background(void)
{
    start(demo_argv);
    connect_two("alpha", "beta");
    CHECK(proto_send_prompt(&P, 1, "run in the background", 21));
    CHECK(run(one_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_AGENT_END], 1);      /* "background" */
    CHECK(run(bg_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_AGENT_START], 2);
    CHECK(proto_sess_state(&P, 1) == SS_READY);
    CHECK(proto_send_prompt(&P, 1, "hi", 2));            /* still takes prompts */
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

/* A drop in the middle of a turn, then a new bridge and the same
   conversations: session 1 continues, session 2 reports its reply cut off. */
static void test_drop_and_resume(void)
{
    char tok1[24], tok2[24];
    start(demo_argv);
    connect_two("alpha", "beta");
    CHECK(strcmp(S.s[1].how, "new") == 0);
    CHECK(strlen(S.s[1].token) == 16);
    CHECK(strcmp(S.s[1].token, S.s[2].token) != 0);
    strcpy(tok1, S.s[1].token);
    strcpy(tok2, S.s[2].token);
    CHECK(proto_send_prompt(&P, 1, "one", 3));
    CHECK(proto_send_prompt(&P, 2, "one", 3));
    CHECK(run(both_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_ASK], 1);

    /* Session 2's next turn is cut off by the bridge dying (SIGKILL: no
       cleanup, as when a Wi-Fi drop leaves the process to be evicted). */
    S.s[2].kill_after_text = S.s[2].texts + 2;
    CHECK(proto_send_prompt(&P, 2, "two", 3));
    CHECK(run(is_idle, 20));
    CHECK_EQ_LONG(S.link[EV_LOST], 1);
    CHECK(child_exited());
    close(S.to_child);
    close(S.from_child);

    /* A new call, the same state folder. */
    start(demo_argv);
    proto_connect(&P, "ATDTbridge", ticks());
    CHECK(run(is_online, 20));
    CHECK(P.resume_cap && P.ping_cap);
    CHECK(proto_open_resume(&P, 1, "alpha", tok1));
    CHECK(proto_open_resume(&P, 2, "beta", tok2));
    CHECK(run(both_ready, 20));
    CHECK(strcmp(S.s[1].how, "resumed") == 0);
    CHECK(strcmp(S.s[1].token, tok1) == 0);
    CHECK(strcmp(S.s[2].how, "cut") == 0);
    CHECK(strcmp(S.s[2].token, tok2) == 0);
    S.s[1].readies = S.s[2].readies = 0;
    CHECK(proto_send_prompt(&P, 1, "three", 5));
    CHECK(proto_send_prompt(&P, 2, "three", 5));
    CHECK(run(both_done, 20));
    CHECK(strstr(S.s[1].said, "turn 2") != NULL);
    CHECK(strstr(S.s[1].said, "dropped") == NULL);
    CHECK(strstr(S.s[2].said, "turn 3") != NULL);
    CHECK(strstr(S.s[2].said, "dropped") != NULL);   /* Claude was told */
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

static void test_demo_stop_one(void)
{
    start(demo_argv);
    connect_two("alpha", "beta");
    S.s[1].stop_after_text = 10;
    proto_send_prompt(&P, 1, "go", 2);
    proto_send_prompt(&P, 2, "go", 2);
    CHECK(run(both_done, 20));
    CHECK_EQ_LONG(S.s[1].counts[EV_INTERRUPTED], 1);
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_ASK], 0);
    CHECK_EQ_LONG(S.s[2].counts[EV_INTERRUPTED], 0);
    CHECK_EQ_LONG(S.s[2].counts[EV_PERM_ASK], 1);
    finish_with_disconnect();
}

static void stress_turn(int sid, const char *prompt)
{
    S.s[sid].readies = 0;
    CHECK(proto_send_prompt(&P, sid, prompt, (long)strlen(prompt)));
}

static int s1_done(void) { return S.s[1].readies >= 1; }
static int s2_done(void) { return S.s[2].readies >= 1; }

static void test_stress_cases(void)
{
    start(stress_argv);
    connect_two("other", "stress");

    /* Two streams at once share the window. */
    stress_turn(1, "burst");
    stress_turn(2, "burst");
    CHECK(run(both_done, 60));
    CHECK_EQ_LONG(S.s[1].texts, 600);
    CHECK_EQ_LONG(S.s[2].texts, 600);

    stress_turn(1, "perm");
    CHECK(run(s1_done, 30));
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_LINE], 2000);
    CHECK(strcmp(S.s[1].result, "success $0.0000") == 0);

    memset(&S.s[1], 0, sizeof(S.s[1]));
    stress_turn(1, "wide");
    CHECK(run(s1_done, 30));
    CHECK_EQ_LONG(S.s[1].counts[EV_PERM_LINE], 1);
    CHECK_EQ_LONG(S.s[1].longest_perm, 3000);

    stress_turn(1, "long");
    CHECK(run(s1_done, 30));
    CHECK(S.s[1].longest_text > 4000);
    CHECK(S.s[1].longest_code > 4500);
    CHECK(S.s[1].longest_perm > 4200);

    memset(&S.s[1], 0, sizeof(S.s[1]));
    stress_turn(1, "error");
    CHECK(run(s1_done, 30));
    CHECK_EQ_LONG(S.s[1].counts[EV_RESULT], 0);
    CHECK_EQ_LONG(S.s[1].counts[EV_CODE_END], 1);
    CHECK_EQ_LONG(S.s[1].counts[EV_ERROR], 1);
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

static void test_disconnect_mid_turn(void)
{
    start(stress_argv);
    connect_two("other", "stress");
    S.s[1].disconnect_after_text = 20;
    proto_send_prompt(&P, 1, "stream", 6);
    proto_send_prompt(&P, 2, "stream", 6);
    CHECK(run(is_idle, 30));
    CHECK_EQ_LONG(S.link[EV_CLOSED], 1);
    CHECK(S.s[1].texts < 400);       /* the turns were stopped, not run to the end */
    CHECK(S.s[2].texts < 400);
    CHECK(child_exited());
    close(S.to_child);
    close(S.from_child);
}

static void reset_files(void)
{
    S.pieces = S.saved = S.failed = S.conflicts = S.progress = 0;
    S.list_entries = S.list_done = 0;
    S.why[0] = 0;
}

static int read_piece(int sid, const char *path, long first, long count)
{
    reset_files();
    CHECK(proto_read(&P, sid, 1, path, first, count, 0) >= 0);
    CHECK(run(piece_or_fail, 30));
    return S.pieces == 1;
}

static int save_text(int sid, const char *path, const char *text, long len)
{
    char tok[24];
    strcpy(tok, S.token);
    reset_files();
    CHECK(proto_save(&P, sid, 1, path, tok, text, len) >= 0);
    CHECK(run(saved_or_fail, 30));
    return S.saved == 1;
}

static void test_files_end_to_end(void)
{
    static char text[8192];
    long n, i;

    start(stress_argv);
    connect_two("other", "stress");
    CHECK_EQ_LONG(proto_files_mode(&P), FILES_RW);

    /* Listing hides .env. */
    reset_files();
    CHECK(proto_list(&P, 2, ""));
    CHECK(run(list_done, 20));
    CHECK_EQ_LONG(S.list_done, 1);
    CHECK_EQ_LONG(S.list_entries, 7);   /* sub and six files, no .env */

    /* Read, edit, save, read back. */
    CHECK(read_piece(2, "README.md", 0, PIECE_LINES));
    CHECK(strcmp(S.mode, "e") == 0);
    CHECK_EQ_LONG(S.total, 4);
    n = S.piece_len;
    memcpy(text, S.piece, n);
    memcpy(text + n, "\rA line from the SE/30.", 23);
    n += 23;
    CHECK(save_text(2, "README.md", text, n));
    CHECK(read_piece(2, "README.md", 0, PIECE_LINES));
    CHECK_EQ_LONG(S.piece_len, n);
    CHECK(memcmp(S.piece, text, n) == 0);

    /* A file that can only be read. */
    CHECK(read_piece(2, "big.txt", 31990, PIECE_LINES));
    CHECK(strncmp(S.mode, "r:", 2) == 0);

    /* Damaged pieces and a damaged block are sent again. */
    S.s[2].readies = 0;
    stress_turn(2, "files-noise");
    CHECK(run(s2_done, 20));
    for (i = 0; i < 6; i++)
        CHECK(read_piece(2, "big.txt", i * PIECE_LINES, PIECE_LINES));
    CHECK_EQ_LONG(S.failed, 0);
    CHECK(read_piece(2, "README.md", 0, PIECE_LINES));
    for (i = 0; i < 80; i++) {
        memcpy(text + n, "\rpadding padding padding padding padding padding", 48);
        n += 48;
    }
    CHECK(save_text(2, "README.md", text, n));
    CHECK_EQ_LONG(S.progress, 2);
    CHECK(read_piece(2, "README.md", 0, PIECE_LINES));
    CHECK_EQ_LONG(S.piece_len, n);
    CHECK(memcmp(S.piece, text, n) == 0);

    /* The file changes on the host before the save: conflict, then force. */
    S.s[1].readies = 0;
    stress_turn(1, "files-conflict");
    CHECK(run(s1_done, 20));
    CHECK(read_piece(1, "README.md", 0, PIECE_LINES));
    n = S.piece_len;
    memcpy(text, S.piece, n);
    CHECK(!save_text(1, "README.md", text, n - 1));
    CHECK_EQ_LONG(S.conflicts, 1);
    S.conflicts = 0;
    CHECK(proto_save_force(&P, S.last_slot));
    CHECK(run(saved_or_fail, 20));
    CHECK_EQ_LONG(S.saved, 1);
    CHECK(read_piece(1, "README.md", 0, PIECE_LINES));
    CHECK_EQ_LONG(S.piece_len, n - 1);

    /* Outside the project. */
    reset_files();
    proto_read(&P, 1, 1, "../stress/README.md", 0, 10, 0);
    CHECK(run(piece_or_fail, 20));
    CHECK_EQ_LONG(S.failed, 1);
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

static int never(void) { return 0; }
static int model_seen(void) { return S.s[1].counts[EV_MODEL] >= 1; }

/* A preset before the tab opens, then a change while it is ready: the
   bridge confirms each with EV_MODEL and the demo names the settings. */
static void test_model_and_effort(void)
{
    start(demo_argv);
    proto_connect(&P, "ATDTbridge", ticks());
    CHECK(run(is_online, 20));
    CHECK(proto_models_cap(&P));
    CHECK_EQ_LONG(proto_nmodels(&P), 3);
    CHECK(strcmp(proto_model(&P, 0), "opus") == 0);
    CHECK(strcmp(proto_model(&P, 2), "haiku") == 0);
    CHECK(strcmp(proto_default_model(&P), "") == 0);
    CHECK(proto_set_model(&P, 1, "sonnet", "high"));
    CHECK(proto_open(&P, 1, "alpha"));
    CHECK(run(one_done, 20));
    CHECK(strcmp(S.s[1].model, "sonnet high") == 0);
    S.s[1].readies = 0;
    CHECK(proto_send_prompt(&P, 1, "first", 5));
    CHECK(run(one_done, 20));
    CHECK(strstr(S.s[1].said, "model sonnet") != NULL);
    CHECK(strstr(S.s[1].said, "effort high") != NULL);

    S.s[1].counts[EV_MODEL] = 0;
    CHECK(proto_set_model(&P, 1, "haiku", ""));
    CHECK(run(model_seen, 20));         /* confirmed once applied, then a fresh > */
    run(never, 1);                      /* let that > arrive before prompting */
    CHECK(strcmp(S.s[1].model, "haiku -") == 0);
    S.s[1].readies = 0;
    CHECK(proto_send_prompt(&P, 1, "second", 6));
    CHECK(run(one_done, 20));
    CHECK(strstr(S.s[1].said, "model haiku") != NULL);
    CHECK(strstr(S.s[1].said, "effort") == NULL);
    CHECK(strstr(S.s[1].said, "turn 2") != NULL);
    CHECK_EQ_LONG(P.dropped, 0);
    finish_with_disconnect();
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    make_projects();
    test_demo_two_sessions();
    test_demo_agents();
    test_demo_background();
    test_drop_and_resume();
    test_model_and_effort();
    test_demo_stop_one();
    test_stress_cases();
    test_disconnect_mid_turn();
    test_files_end_to_end();
    remove_projects();
    proto_free(&P);
    CHECK_DONE();
}
