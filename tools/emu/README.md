# Testing the client in an emulator

These tools let you run Claude30 against the real bridge without the SE/30 or the WiFi232. Emulators do not model the SE/30's serial timing (except Snow) or the WiFi232 itself, so the hardware runs in `client/README.md` are still needed.

## The rig: Basilisk II + tcpser

```
Basilisk II (Claude30) -> modem port -> pty -> tcpser (Hayes modem) -> TCP -> bridge
```

You need:

- Basilisk II with a System 7.5.5 disk and a ROM it accepts, such as a Mac IIci ROM. Basilisk II is not an SE/30, but it runs the same 68k code.
- `socat`.
- [tcpser](https://github.com/FozzTexx/tcpser), which answers AT commands like the WiFi232 and dials a phone-book entry to a TCP address.

Steps:

1. Start a bridge, using one of these:
   ```sh
   WIFI232_IP=127.0.0.1 ./run.sh --demo                                    # the real bridge, no tokens
   socat TCP-LISTEN:2324,reuseaddr,fork EXEC:"python3 tools/emu/stress_bridge.py"   # scripted cases
   ```
2. Run `tools/emu/basilisk-modem.sh`. It creates the pty pair and runs tcpser so that `ATDTbridge` dials `127.0.0.1:2324`.
3. In Basilisk II's preferences, set `seriala /tmp/se30-modem`.
4. Copy `build-68k/Claude30.dsk` into Basilisk II as a second disk, or copy the app across.
5. Launch Claude30 and choose Session > Connect.

## The stress bridge

`stress_bridge.py` speaks app mode like `bridge.py`, but runs a scripted case chosen by the prompt you send. Send `help` for the list:

| Prompt | What it sends |
|---|---|
| `perm` | A Write permission request with a 2,000-line body |
| `wide` | A Bash permission request whose one command line is 3,000 bytes |
| `long` | Prose, code and permission lines over 4 KB each |
| `error` | A turn ending in `I Error: ...` and `>` with no `R`, with a code block left open |
| `burst` | 600 short lines, windows back to back |
| `silent` | A tool line, 3 minutes of silence, then the answer |
| `stream` | 400 lines with short pauses, for trying Stop and Disconnect mid-turn |

`bridge.py` is not changed or needed for these. The host test `client/tests/test_bridge_session.c` runs the protocol core against both bridges over pipes.

## Snow, for timing

[Snow](https://snowemu.com) emulates the SE/30, including its serial chip (SCC). It can expose a serial port on a TCP socket (port 1984 by default). If System 7.5.5 boots in Snow, connect that socket to tcpser instead of the pty, so serial overruns show up as they would on the real machine. One way:

```sh
socat TCP:127.0.0.1:1984 pty,raw,echo=0,link=/tmp/se30-tcpser &
tcpser -d /tmp/se30-tcpser -s 57600 -n "bridge=127.0.0.1:2324"
```

Then run the link test at 57,600, 38,400 and 19,200 and compare the serial error counts.

## Manual test checklist

Run these in the emulator, and the hardware rows on the SE/30. Tick each one.

**Acceptance examples**

| | Check | Where | How |
|---|---|---|---|
| ☐ | A 2,000-line Write opens complete in the permission dialog; scrolling reaches the last line; no line is lost; the session continues afterwards | Emulator | Stress bridge, prompt `perm` |
| ☐ | With the dialog open, Return or typing "a" sends nothing; only Cmd-Y, Cmd-N, Cmd-A or a click answers | Emulator | `run.sh --demo`, any prompt (the demo asks once per turn) |
| ☐ | A long session passes 32 KB: old text leaves the window, the same text is in the log file under Claude30 Logs, and the newest output stays visible | Emulator | Stress bridge `burst` four times, then open the log |
| ☐ | Stop the bridge or tcpser mid-turn: the alert appears, the transcript is intact, and Redial starts a conversation marked "New conversation" | Emulator | `run.sh --demo`, kill tcpser while text streams |
| ☐ | `/new` typed and sent reaches the bridge unchanged, and the bridge answers "New conversation." | Emulator | `run.sh --demo` |
| ☐ | Two lines separated by Option-Return arrive as one prompt with the line break kept, and Send stays dimmed until the turn ends | Emulator | `run.sh --demo` (the demo echoes the prompt) |
| ☐ | Cmd-period with the dialog open denies, stops, closes the dialog and ends the turn | Emulator | `run.sh --demo` |

**Connection and lifecycle**

| | Check | Where |
|---|---|---|
| ☐ | Connect against tcpser and `run.sh --demo` reaches Ready, and the status line shows "Connected" | Emulator |
| ☐ | Connect with the bridge stopped shows the modem's failure in an alert and returns to "Not connected" | Emulator |
| ☐ | Disconnect while Ready ends with no alert, and `bridge.py` has exited on the host (`pgrep -fl bridge.py`) | Emulator |
| ☐ | Disconnect during the stress bridge's `stream` stops the turn, ends with no alert, and the bridge has exited | Emulator |
| ☐ | Disconnect while dialing or during the handshake hangs up, and the next Connect reaches Ready with no stray prompt in the bridge's output | Emulator |
| ☐ | The stress bridge's `silent` case shows "No output for 1:30…" in the status line, then finishes normally | Emulator |
| ☐ | Connect while another program holds the modem port shows an alert naming the port, and the app keeps running | Emulator |
| ☐ | The link test runs for 5 minutes against `run.sh --demo` and closes; Link Test can run again without relaunching | Emulator |

**Window, settings and dialog**

| | Check | Where |
|---|---|---|
| ☐ | Prose wraps to the window; code shows in Monaco; tool lines, info, errors, results and your prompts each look different | Emulator |
| ☐ | Scrolled up, new output does not move the view; scrolled to the bottom, the view follows new output | Emulator |
| ☐ | Copy from the transcript and paste into the input work | Emulator |
| ☐ | Change the speed to 38,400, quit and relaunch: the setting persists and the port opens at 38,400 | Emulator |
| ☐ | The stress bridge's `wide` case shows every character of the 3,000-byte command across marked rows | Emulator |

**Hardware (SE/30)**

| | Check |
|---|---|
| ☐ | The `.bin` arrives through `tools/send-to-mac.sh` and ZTerm, and the app launches from the Finder with its icon |
| ☐ | A 60-minute link test at 57,600 reports zero serial errors, or the speed that does is recorded in `client/README.md` |
| ☐ | A one-hour session with at least one diff of several hundred lines completes with no "Line noise" marker |
| ☐ | Menus and window dragging stay responsive while Claude streams a long answer |
| ☐ | Curly quotes and dashes from Claude display correctly |
