---
title: Reconnect and Resume After a Wi-Fi Drop - Plan
type: feat
date: 2026-11-24
topic: claude30-reconnect
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Reconnect and Resume After a Wi-Fi Drop - Plan

## Goal Capsule

- **Objective:** When the WiFi232 loses Wi-Fi in the middle of a session, the person on the SE/30 gets their tabs back with the same Claude conversations, without touching the host Mac, and no bridge process is ever left running there.
- **Means:** A new call ends any earlier bridge on the host (KTD1), the app and bridge exchange a heartbeat so both sides notice a dead link (KTD2, KTD3), the bridge keeps a small resume record per conversation and reopens it with the Agent SDK's `resume` option (KTD5 to KTD7), and the app redials by itself and reopens each tab with its resume token (KTD8 to KTD10).
- **Product authority:** This Product Contract. PROTOCOL.md v2 is extended through capability words, so older apps and ZTerm keep working unchanged (KTD11).
- **Execution profile:** One pull request to `schreibaer/claude30` touching bridge, protocol spec, client and docs. Build order U1, U2, U3, U4, U5, U6, U7. U1 and U2 fix the orphan on their own and can ship first if resume slips.
- **Stop conditions:** Stop and ask if the Agent SDK cannot resume a conversation in a new process with the same cwd (KTD5's spike fails), or if resuming an interrupted turn leaves the conversation unusable.
- **Who finishes:** An implementing agent builds the app with the shared Retro68 toolchain cache, runs the bridge and host tests and the emulator rows, and refreshes the release files. The person runs the hardware rows on the real SE/30.
- **Open blockers:** None.

---

## Product Contract

### Summary

Two changes that belong together.
On the host, a bridge whose link has died always goes away: the next call ends it, and a heartbeat ends it after a few minutes even if nobody calls back.
On the SE/30, a dropped link no longer means starting over: Claude30 redials by itself and every tab picks up its conversation where it was, with a note in the transcript saying so and saying when the last reply was cut off.

### Problem Frame

The SE/30 reaches the host through a WiFi232 on Wi-Fi, so the link drops now and then.
When Wi-Fi goes away, the TCP connection between the WiFi232 and socat is left half-open: no FIN or RST reaches the host, socat never sees end of input, and `bridge.py` keeps running.
If that bridge was waiting for an `!ACK` it never writes again, so it never finds out the connection is gone, and it stays until someone kills it by hand (`client/README.md`, "A stuck bridge on the host").
Even when the old bridge does exit, every call starts fresh conversations, so a drop throws away the context of every open tab and the person has to explain everything to Claude again.

### Key Decisions

- **A new call ends the previous one.** Only one WiFi232 dials in, and it holds one call at a time, so a new call proves the old one is dead. Governs R1.
- **A drop stops the running turn.** The bridge does not let a turn finish unattended, because it cannot ask the person for permission and they cannot see the output. The conversation keeps everything up to the stop. Governs R4, R8.
- **Resume is automatic, and `/new` is the way out.** Every tab that had a conversation resumes it, after a drop, a Redial, a Disconnect or a relaunch of the app. The person starts over with `/new` or by closing the tab. Governs R6, R10. (Open question Q1 asks whether a deliberate Disconnect should start fresh instead.)
- **ZTerm and the v1 app get the cleanup, not resume.** Resume needs the app to keep tokens; a dumb terminal cannot. Governs R3, R13.

### Requirements

**Host cleanup**

- R1. When a call arrives while an earlier bridge is still running, the earlier bridge stops its sessions and exits before the new call starts any session. If it has not exited within a few seconds, it is killed.
- R2. A bridge talking to a heartbeat-capable app exits on its own when it has heard nothing from the app for 3 minutes, even while it waits for an `!ACK`.
- R3. In tty and v1 mode, the host's TCP keepalive closes a half-open connection, so the bridge sees end of input and exits. This is best effort and depends on socat on the host.
- R4. However a bridge exits (end of input, timeout, being ended by a new call, or `kill`), it denies any open permission request, stops running turns, and stops its Claude processes.

**Resume**

- R5. Each conversation a v2 session runs has a resume token the app can keep. A new conversation (`/new`, `/edits`) gets a new token.
- R6. When the app opens a tab with a token, the bridge continues that conversation in the same project. If it cannot (unknown, expired, other project, or Claude refuses), it opens a fresh conversation and says so.
- R7. A resumed conversation keeps its "Always" permission grants and its auto-accept-edits setting.
- R8. When the last turn was stopped by a drop, the transcript says the reply was cut off, and Claude is told at the start of the next turn that the person did not see the end of its last turn.
- R9. Closing a tab ends its conversation for good: its token is forgotten on both sides.

**The app**

- R10. The app remembers each tab's token in its prefs, so resume works after a drop, a Redial, a Disconnect and a relaunch.
- R11. When the link drops or stops answering, the app hangs up and redials by itself a few times with growing pauses, showing a countdown in the status line, and Disconnect cancels it. Only when those attempts fail does the existing Redial alert appear.
- R12. After a reconnect, each tab's transcript gets one line: the conversation continues, continues with the last reply cut off, or is new because it could not be resumed. Open file windows come back online as they do after a Redial today.
- R13. The app detects a dead link within about 90 seconds even when the modem reports no `NO CARRIER`, and the existing "No output for m:ss" warning still works during long tools.

**Compatibility**

- R14. An older Claude30 (v2 without these capabilities), the v1 app and ZTerm keep working against the new bridge, and the new app keeps working against a v2 bridge without them (no resume, no heartbeat, today's behavior).

### Acceptance Examples

- AE1. **Covers R1, R4.** Given a bridge stuck waiting for `!ACK` after Wi-Fi dropped, when the SE/30 dials again, then the old bridge exits within 15 seconds, `pgrep -fl bridge.py` shows one bridge, and no `claude` process from the old bridge remains.
- AE2. **Covers R2.** Given Wi-Fi drops and the SE/30 is switched off, then within 4 minutes no bridge for that call is running.
- AE3. **Covers R6, R8, R11, R12.** Given two tabs, one idle and one in the middle of a reply, when Wi-Fi drops for 30 seconds, then the app redials by itself, both tabs reopen, the idle tab says the conversation continues, the busy tab says the last reply was cut off, and asking "what were we doing?" in either tab gets an answer that knows the earlier conversation.
- AE4. **Covers R6, R10.** Given a tab with a conversation, when the person quits Claude30 and launches it again tomorrow and connects, then the tab resumes the same conversation.
- AE5. **Covers R6.** Given a token for a conversation Claude can no longer find, when the tab reopens, then it opens with a fresh conversation and the transcript says it could not be resumed.
- AE6. **Covers R14.** Given the new bridge, when the Claude30 build from before this change connects, then it works as before and ignores the new lines.

### Open Questions

- Q1. Should a deliberate Disconnect (Cmd-D or Quit) also resume next time, as planned, or start fresh? The plan resumes, because `/new` is one line away and lost context is the bigger cost. Changing it later only touches when the app clears tokens (U6).

### Scope Boundaries

- No replay of output the SE/30 missed: the end of a cut-off reply is not resent. The person can ask Claude to repeat it.
- No keeping a bridge alive across calls, and no detached sessions on the host. Every call still runs its own bridge; resume goes through Claude Code's saved conversation.
- No resume for ZTerm or the v1 app.
- No change to how a turn is interrupted or how permissions work while the link is up.

#### Deferred to Follow-Up Work

- Resending the tail of a cut-off reply, if the person finds asking Claude to repeat it too slow at 57,600 baud.
- A menu item to start a tab fresh without typing `/new`.

### Sources / Research

- `bridge.py:142` `Link.read` raises `EOFError` only on end of input; `bridge.py:220` `dispatch` releases one `!ACK` waiter on EOF; `bridge.py:335` `_send_line` blocks on `acks.acquire()` with no timeout. A half-open TCP connection never produces EOF, so this wait never ends.
- `bridge.py:1515` `V2.run` shuts every channel down in `finally`; `bridge.py:1433` `Channel.shutdown` denies the open request, interrupts the turn and stops the SDK client. Reusing this path is what makes R4 cheap.
- `bridge.py:1636` `main` catches only `KeyboardInterrupt` and `SystemExit`; a SIGTERM today ends Python without running `shutdown`.
- `run.sh` runs `socat TCP-LISTEN:...,fork,range=.../32 EXEC:.launch.sh`, one bridge per call, with no keepalive options.
- `client/README.md` "A stuck bridge on the host": the manual `pgrep`/`kill` workaround this plan retires.
- Claude Agent SDK 0.2.172 `ClaudeAgentOptions` has `resume`, `session_id` and `fork_session`; the transport passes them as `--resume=` and `--session-id=` to the CLI (`_internal/transport/subprocess_cli.py`). `ResultMessage` carries `session_id`.
- `client/src/mac/tabs.h`: a tab keeps its session id for life and a Redial reopens it with the same id; `modem.c` `link_event(EV_CONNECTED)` reopens every tab, and `EV_LOST` writes "The connection was lost." and queues the Redial alert.
- `client/src/core/protocol.h`: `last_input` drives the 90-second silence warning (`T_SILENCE`); `ST_HANGUP` already runs `+++`/`ATH`.
- `client/src/mac/prefs.h`: PREF version 4 already stores the open tabs' project names.
- Native client plan (`docs/plans/2026-11-24-0950-feat-native-se30-client-plan.md`, KTD6 and R4): named the stuck-bridge gap and deferred resume because each call started fresh.

---

## Planning Contract

Product Contract preservation: new plan; no earlier contract.

### Key Technical Decisions

- KTD1. **One call at a time, enforced by a lock file.** At startup, before the banner or any Claude process, the bridge takes an exclusive `fcntl.flock` on `call.lock` in its state directory and writes its pid into it. If the lock is held, it sends SIGTERM to the pid in the file, waits up to 12 seconds for the lock (the old bridge may spend 5 seconds waiting for its turn to stop and 5 more while the SDK lets the CLI flush its transcript), then sends SIGKILL and waits again. The lock, not the pid, decides: a stale pid in an unlocked file is never signalled, so a reused pid cannot be hit. The state directory is `~/.se30bridge` by default and `--state-dir` overrides it, so tests and a second setup do not end each other. Governs R1.
- KTD2. **SIGTERM, SIGHUP and timeouts take the end-of-input path.** A signal handler (`loop.add_signal_handler`) and the idle watchdog both call one `Terminal.lose_link()` that does what `dispatch` does on EOF: mark `eof`, release every `!ACK` waiter (not just one), and put `None` on the line and key queues. Everything after that is the existing shutdown code. `_send_line` re-checks `eof` after its wait so a released writer stops instead of sending into a dead link. Governs R2, R4.
- KTD3. **Heartbeat: `!PING` from the app, `#PONG` from the bridge.** The app sends `!PING` when it has sent nothing for 30 seconds while online. The bridge answers `#PONG` at once, uncounted and outside the window lock (one whole line under `Link`'s own lock, so it cannot split another line), so it gets through even while the bridge waits for an `!ACK`. The bridge's 3-minute idle watchdog arms only after the first `!PING`, so ZTerm, the v1 app and older v2 apps are never timed out. The app treats the link as lost when nothing has been heard for 90 seconds and a `!PING` sent at least 10 seconds earlier went unanswered, so an alert left open past 90 seconds (which stops the pump and the pings) does not cause a false drop. `#PONG` updates a separate `last_heard` time, not `last_input`, so the silence warning still measures real output. Governs R2, R13.
- KTD4. **TCP keepalive in run.sh, as a backstop.** socat gets `keepalive` plus idle, interval and count options on the listening socket, so a half-open connection from a ZTerm call closes in a few minutes. macOS socat may not support the idle option, whose default is two hours; check on the host during implementation and fall back to `keepalive` alone with a comment. R1 and KTD3 do not depend on it. Governs R3.
- KTD5. **The bridge picks the Claude session id and resumes with it.** A new conversation starts with `session_id=<fresh uuid4>`, so the bridge knows the id before the first turn. A resumed one starts with `resume=<id>`. After each turn the bridge stores `ResultMessage.session_id` if it differs, in case the CLI hands out a new id on resume. The first step of U3 is a short spike against the real CLI: start with `session_id`, run one turn, stop, start a new process with `resume`, and check the conversation continues and the id it reports. Also resume an unknown id and record whether the failure comes from `connect()` or only from the first query; if only from the query, U3 must detect it before sending `<n>Y` so a failed resume is reported as `new`, not `resumed`. If resume does not work in a new process, stop (Goal Capsule). Governs R6.
- KTD6. **Opaque resume tokens in a bridge-owned record.** The token the app sees is 16 random hex digits (`secrets.token_hex(8)`), not the Claude id. `sessions.json` in the state directory maps each token to its project name, Claude id, auto-accept-edits setting, "Always" set, turn count, a `turn_running` flag and a last-used time. The bridge resumes only tokens it issued, only into the project they were issued for, and so never opens a conversation someone started on the host's own terminal. Entries unused for 30 days are dropped on load, matching Claude Code's default transcript retention. The file is written atomically (reuse `write_atomic`) with mode 0600; only the lock holder writes it, so there is a single writer. Governs R5, R6, R7, R9.
- KTD7. **"Cut off" comes from a flag set at turn start.** The bridge sets `turn_running` before a turn starts and clears it only when the turn ends while the link is still up (`eof` not set and the channel not closing). A turn that ends because of `lose_link` or `shutdown` returns normally with an `interrupted` result, so a plain set-and-clear around the turn would wrongly clear it; the flag must stay set in that case, and it is also right when the old bridge was killed with SIGKILL. On resume, a set flag means "cut off": the bridge reports it to the app (R12) and prepends one bracketed sentence to the next prompt, the same way it already reports files saved from the SE/30. Governs R8.
- KTD8. **Wire format for resume.** The bridge adds `resume` and `ping` to the `#OK` capability words. Bridge to app: `<n>Y <token> <how>`, sent after `<n>O` and before the first `<n>>`, and again after `/new` or `/edits`, where `<how>` is `new`, `resumed` or `cut`. App to bridge: `<n>Y <token> <name>` opens session `<n>` resuming that token; the name comes last because it may contain spaces. The bridge answers it exactly like `<n>O <name>` (same refusals), followed by `<n>Y`. A fallback to a fresh conversation is `<n>Y <newtoken> new`, so the app always ends up holding the token in use. Closing a session (`<n>X`, `/quit`) deletes its record. Governs R5, R6, R9.
- KTD9. **Tokens live in the tab and in prefs.** `Tab` gains a 17-byte token. PREF version 5 adds one token per remembered tab, in strip order; version 4 prefs load with no tokens. `remember_tabs()` saves whenever a `Y` line changes a token. When `EV_CONNECTED` reopens tabs, a tab with a token sends `<n>Y`, one without sends `<n>O`, and against a bridge without the `resume` capability the app always sends `<n>O`. Governs R10.
- KTD10. **Automatic redial lives in the shell, the hangup in the core.** After `NO CARRIER` the modem is already on hook, and the core's `link_lost` keeps emitting `EV_LOST` at once as now. After the dead-link timeout the WiFi232 may still be in data mode, so the core first runs the `+++`/`ATH` hangup and emits `EV_LOST` only once the modem is on hook (U4). The countdown starts from `EV_LOST`. The shell then schedules redial attempts after 5, 20 and 60 seconds, shows "Link lost. Redialing in m:ss (Cmd-D cancels)" in the status line, and runs `session_connect()` when the time comes, all from `session_pump` with `TickCount`, so no modal alert holds up the serial pump. A failed attempt (`EV_DIAL_FAILED`) moves to the next pause; after the third, or after Disconnect, the existing Redial alert or idle state takes over. A drop after `!QUIT` is still a normal disconnect. Governs R11.
- KTD11. **Capability words instead of a version bump.** v2 already says the app ignores unknown `#OK` words and both sides ignore unknown session tags, and the bridge ignores unknown link lines. An old app ignores `<n>Y` and never sends `!PING`, so the bridge never times it out. The new app sends `<n>Y` and `!PING` only when the capabilities are there. PROTOCOL.md v2 gains a "Resume and heartbeat" section and a rule that the bridge ignores unknown link-level lines, which it already does. Governs R14.

### High-Level Technical Design

A drop and the reconnect that follows, as directional guidance:

```
SE/30 app                         host
---------                         ----
 ... tab 1 idle, tab 2 mid-reply  bridge A: 2 sessions, sessions.json:
                                    t1 {turn_running:false}, t2 {turn_running:true}
 Wi-Fi drops; no NO CARRIER       TCP half-open; bridge A blocked on !ACK
 90 s without a byte -> lost      (bridge A keeps waiting; watchdog would fire at 3 min)
 hang up, wait 5 s, redial
 CONNECT                          socat starts bridge B
                                  B: flock busy -> SIGTERM A
                                  A: lose_link -> deny, interrupt, stop CLIs, exit
                                  B: lock taken
 #HELLO SE30 2               ->
                             <-   #OK se30bridge 2 files=rw resume ping, P[ ... P]
 1Y t1 claude30              ->
 2Y t2 website               ->
                             <-   1O claude30, 1Y t1 resumed, 1>
                             <-   2O website,  2Y t2 cut,     2>
 tab 1: "Reconnected. The conversation continues."
 tab 2: "Reconnected. The last reply was cut off when the link dropped."
 2P go on                    ->   B prepends "[The link dropped during your last turn ...]"
```

Bridge state per call:

```
startup -> take call lock (evict) -> banner -> tty / v1 / v2
v2 open:   O name          -> new token, session_id=uuid4          -> O, Y tok new, >
           Y tok name      -> record ok? resume=id : new token     -> O, Y tok resumed|cut|new, >
turn:      set turn_running -> run -> clear turn_running, store session_id
close:     X / /quit       -> delete record
link lost: EOF | !PING watchdog | SIGTERM/SIGHUP -> lose_link -> existing shutdown
```

### Risks & Dependencies

- **Resume behavior of the CLI.** Resuming a conversation whose last turn was interrupted mid-tool is the least certain part. The U3 spike checks it first; the fallback is a fresh conversation with a note, which is still no worse than today.
- **Two processes on one conversation.** If eviction failed, two bridges could append to the same Claude transcript. KTD1 makes the new bridge wait for the lock before it opens anything, and SIGKILL ends a bridge that ignores SIGTERM.
- **Claude processes left by a SIGKILLed bridge.** The CLI is the SDK's child with a stdin pipe and should exit when the pipe closes. U1's test checks for leftover children after SIGKILL, as `test_stdin_close_leaves_no_children` does for end of input.
- **Modal dialogs on the SE/30.** The app's pump runs inside the permission dialog and file windows, but a plain `Alert` stops it. The bridge's 3-minute watchdog is long enough for a person to read an alert; the 30-second ping resumes when the alert closes.
- **Tests that run several bridges.** With eviction, two test bridges sharing `~/.se30bridge` would end each other. `tests/v2client.py` and `client/tests/test_bridge_session.c` pass a temporary `--state-dir`.

---

## Implementation Units

### U1. One call at a time, and a clean exit on every path

- **Goal:** A new call ends the old bridge, and signals shut down as cleanly as end of input does.
- **Requirements:** R1, R4.
- **Dependencies:** None.
- **Files:** `bridge.py`, `tests/v2client.py`, `tests/test_bridge_lifecycle.py` (new).
- **Approach:** KTD1 and KTD2. Add `--state-dir`, the call lock with eviction at the top of `main_async`, `Terminal.lose_link()`, and SIGTERM/SIGHUP handlers that call it. `dispatch`'s EOF branch calls the same method. Release all `!ACK` waiters (set `eof` and release the semaphore in a way every waiter sees, for example by checking `eof` in a loop around the acquire). Log eviction to stderr. `v2client.Client` creates a temporary state directory unless a test passes one.
- **Test scenarios:**
  - A bridge blocked on an unanswered `W` (client with `auto_ack=False`, pipes held open) is ended when a second bridge starts with the same state dir; the first exits within 8 seconds and the second gets `#OK`.
  - A first bridge that ignores SIGTERM (test hook or a stub process holding the lock) is SIGKILLed and the second starts.
  - A stale `call.lock` with a dead pid and no lock held: the second bridge starts at once and signals nothing.
  - SIGTERM to a bridge mid-turn in v2 with a permission request open: it exits, and the demo session's stop ran (stderr log line).
  - Two bridges with different state dirs run side by side.
  - A demo test hook starts a child that reads stdin until EOF, as the CLI does. Record its pid, SIGKILL the bridge, and assert that pid is gone within a few seconds (checking the bridge's ppid would always pass, since orphans move to init).
- **Verification:** New tests and the existing `tests/` suite pass.

### U2. Heartbeat and idle watchdog in the bridge, keepalive in run.sh

- **Goal:** A bridge whose app went silent exits on its own; ZTerm calls get TCP keepalive.
- **Requirements:** R2, R3, R13 (bridge side).
- **Dependencies:** U1.
- **Files:** `bridge.py`, `run.sh`, `tests/test_bridge_lifecycle.py`.
- **Approach:** KTD3 and KTD4. Track the time of the last received byte in `dispatch`. Handle `!PING` in `_route` for v2: answer `#PONG` directly through `Link.send`, and arm a watchdog task that calls `lose_link()` after `--idle-timeout` seconds (default 180) without input. Add `resume ping` to the `#OK` words only in U3, together with resume, so the capability list changes once. Add keepalive options to `run.sh`'s socat listen address after checking the host's socat version.
- **Test scenarios:**
  - `!PING` gets `#PONG` within a second, also while the bridge waits for an `!ACK`, and `#PONG` does not count toward the window.
  - With `--idle-timeout 2`, a client that pinged once and then holds pipes open silently is dropped after about 2 seconds, and the bridge exits through the normal shutdown.
  - A client that never pings is not timed out (wait past the timeout, then send a prompt and get an answer).
  - Bytes other than `!PING` reset the watchdog.
- **Verification:** New tests pass; `run.sh` still starts and accepts a call from the emulator.

### U3. Resume records and resumed sessions in the bridge

- **Goal:** v2 sessions get tokens and can reopen a conversation in a new bridge.
- **Requirements:** R5 to R9, R14 (bridge side).
- **Dependencies:** U1 (state dir and single writer).
- **Files:** `bridge.py`, `tests/test_bridge_resume.py` (new), `tests/test_bridge_v2.py`.
- **Approach:** Start with the KTD5 spike and record the result in the PR description. Then KTD5 to KTD8: a small `ResumeStore` class (load, prune, get, put, delete, atomic save); `SessionConfig` gains `token`, `claude_id` and `resume`; `ClaudeSession.start` passes `session_id` or `resume`, and `ClaudeSession` loads and saves its "Always" set through the record. `Channel.run` sends `<n>Y` after `<n>O`; `_restart` issues a new token and deletes the old record; `V2.session_line` accepts `Y <token> <name>`; `V2.close` and `/quit` delete the record. Set `turn_running` before `session.turn` and clear it afterwards only if the link is still up (KTD7), and add the cut-off sentence to the next prompt next to the existing "edited these files" note. Add `resume ping` to the `#OK` words. `DemoSession` supports resume by keeping its turn count in the record and saying "turn N" in its reply, so tests can see continuity without Claude.
- **Test scenarios:**
  - `<n>O` gets `<n>O`, `<n>Y <16 hex> new`, `<n>>` in that order; `#OK` carries `resume` and `ping`.
  - Demo: two turns, end the bridge, start a new one with the same state dir, `<n>Y <token> <name>` gets `resumed` and the next reply says turn 3.
  - End the bridge mid-turn (`silent` or a held permission) both by closing stdin and by SIGTERM eviction from a second bridge, then resume: `<how>` is `cut` in both cases, and the next prompt the session receives starts with the cut-off sentence (demo echoes it, or check via a test hook).
  - A token issued for project A sent with project B, an unknown token and a malformed token each open a fresh conversation with a new token and `new`.
  - `/new` sends a new `Y ... new`; the old token no longer resumes.
  - `<n>X` and `/quit` delete the record; resuming that token gives `new`.
  - "Always" given before the drop still applies after resume (demo permission flow).
  - Records older than 30 days are pruned on load; `sessions.json` has mode 0600.
  - An old-style client that ignores `Y` and never pings still runs the existing v2 tests unchanged.
- **Verification:** New and existing bridge tests pass; one manual run with real Claude confirms that a resumed tab remembers the earlier conversation, and that after SIGKILL of a bridge `pgrep -fl claude` shows no process left from it.

### U4. Protocol core: capabilities, resume opens, heartbeat, dead-link detection

- **Goal:** The portable core speaks the new lines and notices a dead link.
- **Requirements:** R6, R11 (core side), R13, R14.
- **Dependencies:** U3 for the wire format; can be built against the spec in parallel.
- **Files:** `client/src/core/protocol.h`, `client/src/core/protocol.c`, `client/tests/test_protocol.c`, `client/tests/test_bridge_session.c`.
- **Approach:** KTD3, KTD8, KTD11. Parse `resume` and `ping` from `#OK`. Add `proto_open_resume(p, sid, project, token)` that sends `<n>Y` when the capability is there and `<n>O` otherwise. Parse `<n>Y <token> <how>` into a new `EV_RESUMED` event whose text is `"<how> <token>"`. In `proto_tick`, send `!PING` after 30 seconds without sending while `ST_ONLINE` and the capability is there; track `last_sent` and a new `last_heard` (any byte, including `#PONG`); keep `last_input` for real lines only, so `#PONG` does not reset the silence warning. If nothing is heard for `T_DEAD` (90 seconds) while online and a `!PING` sent at least 10 seconds earlier is unanswered, run `start_hangup` with a new after-action that emits `EV_LOST` when the modem is on hook. Leave `link_lost` unchanged for `NO CARRIER`. `test_bridge_session.c` passes a temporary `--state-dir` and adds a drop-and-resume case against `bridge.py --demo`.
- **Test scenarios:**
  - `#OK se30bridge 2 files=rw resume ping` sets both capabilities; without them, `proto_open_resume` sends `<n>O` and no `!PING` ever goes out.
  - `!PING` goes out after 30 idle seconds and not while other lines are being sent.
  - `#PONG` resets the dead-link timer but not the silence timer: a busy session with only pongs still gets `EV_SILENCE` at 90 seconds.
  - 90 seconds with no bytes and an unanswered `!PING` while online starts the hangup (`+++`, `ATH`) and emits `EV_LOST` once after it; 90 seconds of silence with no ping sent yet (the pump was stopped) sends a ping first instead of declaring the link lost.
  - `1Y 0123456789abcdef cut` emits `EV_RESUMED` for session 1 with that text; a malformed `Y` is ignored.
  - End to end: open two demo sessions, kill the bridge, start a new one, reopen both with their tokens, and get `resumed` with the turn count continuing.
- **Verification:** Host test suite passes with `-Wall -Wextra -Werror`.

### U5. Tokens in tabs and prefs

- **Goal:** The app keeps each tab's token and uses it when reopening.
- **Requirements:** R5, R9, R10, R12.
- **Dependencies:** U4.
- **Files:** `client/src/mac/tabs.h`, `client/src/mac/tabs.c`, `client/src/mac/prefs.h`, `client/src/mac/prefs.c`, `client/src/mac/modem.c`.
- **Approach:** KTD9. On `EV_RESUMED`, store the token in the tab, call `remember_tabs()`, and add the transcript line for `resumed` ("Reconnected. The conversation continues."), `cut` ("Reconnected. The last reply was cut off when the link dropped.") or `new` on a reopen ("Could not resume the conversation; this is a new one."). Skip the line on a tab's first open from New Tab. `open_tab` calls `proto_open_resume` with the tab's token. Closing a tab clears its token. Bump `PREFS_VERSION` to 5 and load version 4 with empty tokens. No new Toolbox managers: Resource Manager calls as `prefs.c` uses today.
- **Test scenarios:** No host test reaches the shell. Emulator rows E1, E3 and E4.
- **Verification:** Emulator rows pass; prefs from the current release load without losing remembered tabs.

### U6. Automatic redial

- **Goal:** A lost link redials by itself before falling back to the alert.
- **Requirements:** R11, R12.
- **Dependencies:** U4, U5.
- **Files:** `client/src/mac/modem.c`, `client/src/mac/modem.h`, `client/src/mac/window.c` (status line text).
- **Approach:** KTD10. On `EV_LOST`, instead of queueing `kAlertLost`, start a redial schedule (5, 20, 60 seconds) held in `modem.c` and checked in `session_pump` with `TickCount`. The status line shows the countdown and the attempt number. `session_disconnect` cancels the schedule. `EV_DIAL_FAILED` during a scheduled attempt moves to the next one instead of showing `kAlertFailed`; after the last, show the existing Redial alert with the last failure text. Keep the per-tab "The connection was lost." line. The link test (`session_linktest_*`) keeps today's behavior: no automatic redial while it runs.
- **Test scenarios:** Emulator rows E2, E5 and E6.
- **Verification:** Emulator rows pass.

### U7. Docs, emulator checklist and release files

- **Goal:** Describe the new behavior, retire the manual kill instructions, ship a build.
- **Requirements:** All.
- **Dependencies:** U1 to U6.
- **Files:** `PROTOCOL.md`, `README.md`, `client/README.md`, `tools/emu/README.md`.
- **Approach:** PROTOCOL.md: the `resume` and `ping` capabilities, `<n>Y` both ways, `!PING`/`#PONG` and the watchdog, the rule that the bridge ignores unknown link lines, and an example exchange of a resume. README.md: one call at a time and `--state-dir`, `--idle-timeout`, `sessions.json` and what it holds, the keepalive in `run.sh`. client/README.md: replace "A stuck bridge on the host" with "When Wi-Fi drops" (what the app does, `/new` to start over, and `pgrep` as a last resort only). Emulator checklist rows below plus two hardware rows. Build with the shared Retro68 cache and copy `Claude30.bin` and `Claude30.dsk` to `/mnt/project-files/release/`.
- **Test scenarios:** None beyond the rows it adds.
- **Verification:** Docs match the flags and lines in the code; release files are dated after the last commit.

---

## Verification Contract

- **Bridge tests:** `python3 -m unittest discover tests`. All existing tests plus `test_bridge_lifecycle.py` and `test_bridge_resume.py` pass.
- **Host tests:** `cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host`, including the new `test_protocol.c` cases and the drop-and-resume case in `test_bridge_session.c`.
- **68k build:** `tools/setup-retro68.sh`, then the build in `client/README.md`, with no new warnings.
- **Emulator rows** (Basilisk II, tcpser and `run.sh --demo`, as `tools/emu/README.md` describes):
  - E1. Two tabs with a few demo turns each; kill tcpser while one tab streams. The app redials by itself after tcpser is back, both tabs reopen, the streaming tab says "cut off", and the turn count continues.
  - E2. Kill tcpser and leave it down: the status line counts down through three attempts, then the Redial alert appears. Cmd-D during the countdown cancels it and leaves the app idle.
  - E3. Quit the app with two tabs open, relaunch, Connect: both tabs resume.
  - E4. Close a tab, then reconnect: the closed tab does not come back, the other resumes.
  - E5. Stop tcpser from forwarding without closing the connection (pause the process): within about 90 seconds the app declares the link lost and redials; on the host, the old bridge is gone after the redial (AE1).
  - E6. A long `silent` turn: "No output for m:ss" still appears at 90 seconds while pings flow.
  - E7. The app from the current release against the new bridge: tabs, files and permissions work as before.
- **Hardware rows (the person):**
  - H1. Mid-reply, unplug the WiFi232's power or turn off the access point for 30 seconds. The app reconnects on its own, both tabs continue, and `pgrep -fl bridge.py` on the host shows one bridge.
  - H2. Drop Wi-Fi, then switch the SE/30 off. Within 4 minutes no bridge is running on the host.

## Definition of Done

- R1 to R14 hold, shown by the tests and rows above.
- U1 to U7 merged in one pull request; release files rebuilt from it.
- PROTOCOL.md describes every new line and capability; v1 and tty sections are unchanged.
- `client/README.md` no longer tells people to kill a stuck bridge as the normal fix.
- What was tested in the emulator, on the host and on hardware is stated in the PR, and what was not.

### Assumptions

Scoping was not confirmed with the person before writing; these are the bets the plan makes.

- Resuming after a deliberate Disconnect or a relaunch is wanted, not only after a drop (Q1).
- The host Mac runs only one SE/30 call setup per state directory; a second WiFi232 or emulator uses its own `--state-dir`.
- Stopping the running turn on a drop is preferred over letting it finish unattended.
- Claude Code keeps conversation transcripts long enough (its default is 30 days) for tokens to stay useful.

### Deferred to Implementation

- Whether macOS socat accepts `keepidle`, `keepintvl` and `keepcnt` (KTD4).
- Whether the CLI keeps the same session id on resume; KTD5 stores whatever it reports either way.
- Exact wording of the cut-off sentence given to Claude.
