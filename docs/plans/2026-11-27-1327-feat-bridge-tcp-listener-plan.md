---
title: Bridge listens on TCP itself (no socat) - Plan
type: feat
date: 2026-11-27
topic: se30bridge-tcp-listener
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Bridge listens on TCP itself (no socat) - Plan

## Goal Capsule

- **Objective:** The bridge accepts the WiFi232's calls itself. Bridge
  setup no longer needs `brew install socat`, `run.sh` no longer writes a
  launcher, and the project folder may contain spaces. Every call
  behaves exactly as today.
- **Means:** A `--listen` mode in `bridge.py` that runs a small
  supervisor: accept, check the caller's IP, set TCP keepalive, start
  one ordinary bridge process per call with the socket as its
  stdin/stdout. That is what socat does today.
- **Product authority:** The person's request in this session ("step 1"
  of making the bridge easier to run, toward an always-on or menu bar
  bridge later).
- **Execution profile:** One pull request (U1-U2), then a run by the
  person on their Mac and SE/30 (U3).
- **Stop conditions:** Stop and ask if
  - any change to the wire protocol or `PROTOCOL.md` semantics seems
    needed;
  - the per-call child cannot run unchanged (needs edits to `Link`,
    `take_call_lock` or `main_async` beyond reading a marker and the
    pre-approved `Link.read` change in KTD8);
  - the supervisor would need a third-party package.
- **Who finishes:** An implementing agent writes the code and runs the
  bridge tests (`tests/`). The person runs U3 on the real hardware.

---

## Product Contract

### Summary
`./run.sh --demo` works as before, with socat gone. Running
`python bridge.py --listen 2324 --allow 192.168.0.9 [options]` directly
works too. The log shows `listening on port 2324 for 192.168.0.9`, then
`call from 192.168.0.9` for each call, then the bridge's usual lines.

### Problem Frame
socat is the bridge's only non-Python dependency on the host. It is also the
reason for `.launch.sh`, the "no spaces in the path" rule and the
version probe for keepalive options in `run.sh`. Later steps (a launchd
agent, a menu bar app) are simpler if one Python program owns the
whole host side.

### Requirements
- R1. `--listen [HOST:]PORT` makes `bridge.py` accept TCP calls; HOST
  defaults to all IPv4 interfaces.
- R2. `--allow IP` (repeatable, IP or CIDR) limits callers; others are
  closed at once and logged. Default `192.168.0.9`, as `run.sh`, used
  only when no `--allow` is given (an explicit `--allow` replaces it;
  `default=None` plus fill-in after parsing, like `--projects`).
- R3. Each accepted call runs as its own bridge process with every
  other option passed through; call lock, eviction, heartbeat and
  signal behaviour are unchanged.
- R4. Accepted sockets get TCP keepalive (idle 60 s, interval 20 s,
  3 probes) where the OS offers those knobs.
- R5. No zombies or leftover bridges: finished calls are reaped;
  stopping the supervisor ends its live calls.
- R6. `run.sh` keeps its `WIFI232_IP` and `PORT` variables and option
  pass-through, but no longer uses socat.
- R7. No wire change; `PROTOCOL.md` changes in wording only.

### Scope Boundaries
- Not in scope: launchd agent / start at login, menu bar app, status
  file for a GUI, IPv6 callers, more than one simultaneous call.
- socat stays for the other tools: `tools/send-to-mac.sh` (first app
  install over ZTerm) and the emulator setup in `tools/emu/`. The
  README's install line for the bridge drops it; `client/README.md`
  keeps it for `send-to-mac.sh`.
- ZTerm, v1 and v2 clients see identical bytes.

#### Deferred to Follow-Up Work
- Step 2: a launchd agent plus install script.
- Step 3: menu bar wrapper reading a status line or log.

---

## Key Technical Decisions

- **KTD1. Supervisor plus one child process per call, not an in-process
  asyncio server.** The child model is load-bearing: `take_call_lock`
  (`bridge.py`, "One call at a time") evicts the previous call by
  SIGTERM/SIGKILL of its pid; `main_async` maps SIGTERM/SIGHUP to
  `term.lose_link`; `Link` reads fd 0 and writes fd 1. socat EXEC
  already hands the child a socket, so the child path is proven.
- **KTD2. The child is marked by an environment variable, not by
  stripping `--listen` from argv.** argparse accepts abbreviations
  (`--lis`), so stripping is fragile and a missed flag would make a
  child try to listen. The supervisor starts
  `sys.executable -u <absolute path of bridge.py> <original args>`
  (so the current folder never matters, e.g. under launchd) with a
  marker such as
  `SE30_CALL=1`; `main()` pops the marker and skips `--listen`.
- **KTD3. The supervisor parses and validates all options before
  binding**, so a typo fails at start, not at the first call. It never
  takes the call lock and never imports the Agent SDK.
- **KTD4. Standard library only:** `socket` blocking accept with a
  ~1 s timeout, `subprocess.Popen` (`close_fds` default, so the
  listening socket does not leak into calls and the port is freed when
  the supervisor exits), `ipaddress` for `--allow`. The listening
  socket sets `SO_REUSEADDR` before `bind` (socat's `reuseaddr` today),
  so a restart right after a live call is not blocked by TIME_WAIT; not
  `SO_REUSEPORT`, so a second supervisor still gets "port in use".
- **KTD5. Keepalive via `setsockopt` with `getattr` lookups:**
  `TCP_KEEPALIVE` (macOS) or `TCP_KEEPIDLE` (Linux), `TCP_KEEPINTVL`,
  `TCP_KEEPCNT`; a missing constant is skipped, as `run.sh` skips
  unsupported socat options today.
- **KTD6. Child stderr is inherited**, so logs stay in the one
  Terminal window, as with socat.
- **KTD7. Stopping:** SIGINT/SIGTERM/SIGHUP to the supervisor close the
  listener first (so a restart can bind), SIGTERM live children, log
  `waiting for the call to end`, wait up to `SE30_EVICT_WAIT` (default
  12 s, the same budget `take_call_lock` gives a bridge to save its
  conversation), SIGKILL any child still running, then exit. (Ctrl-C
  also reaches children through the process group, as today.)
- **KTD8. Socket read errors end the call like EOF.** Under socat the
  child only ever saw a clean EOF. With the TCP socket on fd 0, a peer
  reset or keepalive timeout reaches `Link.read` as an `OSError`
  (`ConnectionResetError`, `TimeoutError`) via the asyncio read
  transport, and `Terminal.dispatch` catches only `EOFError`, so the
  call would hang. `Link.read` maps `OSError` to `EOFError`; this is the
  one pre-approved change to the child.
- **KTD9. One helper configures each accepted socket** (keepalive
  options per KTD5). The supervisor calls it on every accepted socket,
  and tests call the same helper on a real accepted socket, so the test
  proves what the supervisor does.

---

## Implementation Units

### U1. Listener mode in bridge.py

**Goal:** R1-R5, R7.
**Dependencies:** none.
**Files:** `bridge.py`, `tests/test_bridge_listen.py` (new),
`tests/v2client.py` (socket support).

**Approach:**
- Add `--listen` and `--allow` to the parser in `main()`; validate
  `--allow` with `ipaddress`; parse `HOST:PORT` (port 0 allowed, for
  tests).
- If the child marker is set: pop it, ignore `--listen`, continue as
  today. Else if `--listen`: run the supervisor and return before
  `take_call_lock`.
- Supervisor loop: bind (clear message and exit 1 if the port is in
  use), log the real port, accept with timeout, reject disallowed
  peers, set keepalive, `Popen` the child with the socket fd as stdin
  and stdout and the marker in its env, close the socket in the parent,
  poll children each pass.
- Test client: let `Client` in `tests/v2client.py` run over a connected
  socket (e.g. a constructor or factory taking a socket and reading
  bytes the same way); shape decided at implementation.

**Patterns to follow:** `log()` for messages; `tests/test_bridge_lifecycle.py`
for process and eviction checks (`alive`, `wait_gone`, `children`).

**Test scenarios** (bridge in `--demo`, `--listen 127.0.0.1:0`,
`--allow 127.0.0.1`, temp `--state-dir`; port read from the log):
- Happy path: connect, send `#HELLO SE30 2`, receive `#OK` and the
  project list; a tty-mode connection gets the demo banner.
- Options pass through: `--projects <tmp>` on the supervisor shows
  those projects to the caller.
- Second call evicts the first: open call A, then call B; A's child is
  gone within the eviction wait and A's socket reaches EOF; B works.
- Disallowed peer: supervisor with `--allow 127.0.0.2`; a connection
  from 127.0.0.1 is closed with no bytes and no child started; log
  names the address.
- Reaping: after a call hangs up, the supervisor has no zombie
  children.
- Stop: SIGTERM to the supervisor with a live call ends the child and
  frees the port (a new bind to it succeeds).
- Port in use: second supervisor on the same port exits non-zero with
  a readable message.
- Bad options fail at start: `--allow notanip` exits with an argparse
  error before binding.
- Explicit `--allow` replaces the default: with `--allow 127.0.0.1`
  the effective list logged at start is only 127.0.0.1.
- Restart after a live call: stop the supervisor mid-call, start a new
  one on the same port; it binds at once.
- Marker: a child started with the marker and `--listen` in argv does
  not listen (runs on its stdin/stdout).
- Keepalive: the test accepts a real loopback connection, runs the
  KTD9 helper on it, and reads back `SO_KEEPALIVE` (and the idle /
  interval / count options the platform has).
- Reset ends the call: a tty-mode client connects, then closes with
  `SO_LINGER` 0 (sends an RST); the child logs the lost link and exits,
  and the supervisor reaps it.
- Stop waits, then kills: with a short `SE30_EVICT_WAIT`, a child that
  ignores SIGTERM is killed after the wait and the supervisor exits.

**Verification:** new tests pass; existing test files pass with no
change to their assertions (only the shared `Client` helper gains
socket support).

### U2. run.sh and docs

**Goal:** R6, R7; remove socat from setup.
**Dependencies:** U1.
**Files:** `run.sh`, `README.md`, `PROTOCOL.md`, `client/README.md`.

**Approach:**
- `run.sh`: one `exec` of the venv Python on `bridge.py --listen
  "$PORT" --allow "$WIFI232_IP" "$@"`; drop `.launch.sh`, the keepalive
  probe and the no-spaces comment.
- `README.md`: drop `brew install socat`; diagrams read
  `bridge.py (listening)`; keepalive sentence now credits the bridge;
  show running without `run.sh`; note macOS may ask once to allow the
  venv's Python to accept incoming connections.
- `PROTOCOL.md` line 3 and `client/README.md` diagram: replace "socat"
  with the bridge's listener. No version bump.

**Test expectation:** none -- script and docs; covered by U1 tests and
U3.

### U3. Run on the real hardware (the person)

**Goal:** confirm on macOS with the WiFi232.
**Dependencies:** U2.
- `./run.sh --demo`: Claude30 connects, tabs work; ZTerm connects.
- Wi-Fi drop and redial: log shows `ending the previous call`.
- Ctrl-C: `ps` shows no bridge left; restart binds at once.
- Project folder path with a space works.

---

## Risks and Deferred Notes

- **Shared socket fd for stdin and stdout.** `Link.open` sets fd 0
  non-blocking and then fd 1 blocking; on one socket these share the
  same file flags. socat EXEC already passes a single socketpair end
  the same way, and review found it harmless (the asyncio transport
  reads only after the selector reports data); U1's socket tests confirm
  it. A `dup` would not help (it shares the flags); if reads stall,
  stop and ask before changing `Link`.
- **macOS keepalive constants** exist only in newer Pythons; `getattr`
  skips them, and Claude30 does not depend on keepalive (heartbeat and
  eviction cover it). Verified only in U3.
- Tests run on Linux in CI-like environments (`/proc` is used by
  lifecycle tests); the macOS path is covered by U3 only.
- Old installs keep a stale `.launch.sh`; harmless, not deleted.
- Review notes (FYI, no change): the IP allowlist is still the only
  access control, as with socat, and `--allow` would accept a wide
  CIDR such as `0.0.0.0/0`; the start line logs the effective list.
  For step 2, launchd's own per-connection socket activation
  (`inetdCompatibility`) could replace the supervisor; weigh that then.
