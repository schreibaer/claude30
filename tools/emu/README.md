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

`stress_bridge.py` is `bridge.py` itself (protocol version 2, sessions, project list and file service) with Claude replaced by scripted cases. It offers two projects, `stress` and `other`, temporary folders with sample files: `big.txt` (40,000 lines), `README.md`, `run.sh` with CRLF endings, a Latin-1 file, a file with an emoji, a PNG, a `.env` that must stay hidden and a subfolder. Each prompt runs a case in the tab that sent it. Send `help` for the list:

| Prompt | What it sends |
|---|---|
| `perm` | A Write permission request with a 2,000-line body |
| `wide` | A Bash permission request whose one command line is 3,000 bytes |
| `long` | Prose, code and permission lines over 4 KB each |
| `error` | A turn ending in `I Error: ...` and `>` with no `R`, with a code block left open |
| `burst` | 600 short lines, windows back to back |
| `silent` | A tool line, 3 minutes of silence, then the answer |
| `stream` | 400 lines with short pauses, for trying Stop and Disconnect mid-turn |
| `files-bigread` | Points at `big.txt`, to open read-only and scroll |
| `files-noise` | From now on this tab's file transfers are damaged: one byte in every third read piece, and the second block of the next save (once) |
| `files-conflict` | The next save in this tab finds the file changed on the host |

`--files ro` and `--files off` start it with less file access. The host test `client/tests/test_bridge_session.c` runs the protocol core against both bridges over pipes, including two sessions at once and a list, read and save with damaged transfers.

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

**Tabs and the project picker**

| | Check | Where | How |
|---|---|---|---|
| ☐ | A background tab's permission request marks its label and names it in the status line; no dialog opens until you click that tab, and then it shows the full request | Emulator | Stress bridge: `perm` in tab 2, then click tab 1 before it arrives |
| ☐ | A background tab running `burst` trims to its own log while hidden; switching to it shows the tail and the log holds the rest | Emulator | Stress bridge, `burst` twice in a background tab |
| ☐ | Switching tabs mid-turn keeps each tab's input text and scroll position | Emulator | Two tabs, `stream` in one |
| ☐ | Cmd-period with a background tab busy and the front tab idle does nothing | Emulator | `stream` in tab 2, Cmd-period in tab 1 |
| ☐ | With the front tab's dialog open, a background `perm` arrives: the open dialog's text does not change, and the background tab later shows its own request | Emulator | Two tabs, `perm` in both |
| ☐ | Two tabs reopened in the same second write two separate log files | Emulator | Three tabs, drop the link, Redial |
| ☐ | Three tabs, drop the link (stop tcpser), Redial: all three reopen with "New conversation" | Emulator | `run.sh --demo --projects ...` |
| ☐ | Remove a remembered project folder on the host and connect: that tab says it is not available with Send dimmed; the others open | Emulator | `run.sh --demo --projects ...` |
| ☐ | First launch with version 1 prefs connects and shows the picker; Cancel leaves "No project open. Press Cmd-T." and Cmd-T opens the picker again | Emulator | Old `Claude30 Prefs` |
| ☐ | New Tab is dimmed with six tabs open; projects already open show "(open)" and cannot be opened twice | Emulator | Six folders under `--projects` |
| ☐ | An empty `--projects` folder shows "No projects available…" with Open dimmed | Emulator | `run.sh --demo --projects /empty` |
| ☐ | `/quit` in a tab closes that tab only; quitting with tabs open and relaunching restores the same tab order | Emulator | Any bridge |
| ☐ | Close Tab on a busy tab asks first, and Cancel keeps it running | Emulator | `stream` |

**Project files**

| | Check | Where | How |
|---|---|---|---|
| ☐ | The Open dialog lists folders first, then files with sizes; `.env`, `.git` and ignored files are absent | Emulator | Demo bridge on a copy of this repo |
| ☐ | Opening a folder and pressing Up returns to the root listing; an empty folder says "Empty folder" | Emulator | Same |
| ☐ | From the root listing, opening a folder with fewer entries leaves no names from the root below the new rows, and the list starts at the top even if the root was scrolled down | Emulator | Stress bridge, `stress` project, `subfolder/` |
| ☐ | The New Tab list refilled after a fresh project list arrives has no leftover rows | Emulator | Open New Tab right after Connect |
| ☐ | Output from a background tab keeps arriving while the Open dialog is up, and a front-tab permission request shows "Claude is waiting for permission in…" in the dialog, then opens right after Cancel | Emulator | Stress bridge, `stream` and `perm` |
| ☐ | With `--files off` the Open command is absent; with `--files ro` files open read-only and Save is dimmed | Emulator | Stress bridge with each option |
| ☐ | `big.txt` opens read-only; dragging the scroll bar to the middle shows that region within a few seconds, and scrolling back up refetches the start | Emulator | Stress bridge, `files-bigread` |
| ☐ | Edit `README.md` and press Cmd-S: the host file has the change with LF endings intact, the title's bullet clears, and Claude's next turn mentions the edit | Emulator | Real bridge, or the stress bridge for the save alone |
| ☐ | After `files-conflict`, Save shows the alert; Cancel keeps the edits, and Replace saves | Emulator | Stress bridge |
| ☐ | After `files-noise`, a read of `big.txt` and a save of `README.md` both complete, and the host file matches the window byte for byte | Emulator | Stress bridge |
| ☐ | Typing past 31,000 bytes in a file window beeps and inserts nothing | Emulator | Any editable file near 28 KB |
| ☐ | Closing a tab with a dirty file window asks Save, Don't Save or Cancel, and Cancel keeps both the tab and the window | Emulator | Any bridge |
| ☐ | Dropping the link keeps the window and its edits; after Redial, Save succeeds; quitting while offline offers only Don't Save and Cancel | Emulator | Stop tcpser |
| ☐ | A clean window of a file Claude edits in the same tab reloads when the turn ends and shows "Changed on the host" | Emulator | Real bridge |
| ☐ | With tab 2's file window in front, Cmd-period stops tab 1's turn, Paste goes into the file window, and Cmd-O browses tab 2's project | Emulator | Two tabs |
| ☐ | Switching to tab 1 hides tab 2's file windows; switching back shows them | Emulator | Two tabs |
| ☐ | A file containing tabs displays usably in Monaco 9 (TextEdit's tab drawing on the real ROM) | Emulator and SE/30 | Any file with tabs |

**Mascot and keyboard scrolling**

| | Check | Where | How |
|---|---|---|---|
| ☐ | During a turn the mascot walks in the empty input box beside a changing word, and the word reads "Running *tool*" while a tool runs | Emulator | Stress bridge, `silent` |
| ☐ | Typing during a turn moves the mascot beside Send without touching the text; deleting the text brings it back; the caret keeps blinking | Emulator | `stream` |
| ☐ | A permission request shows the mascot with its arms up and "Waiting for you"; it is still while the dialog is up and gone after the turn | Emulator | `perm` |
| ☐ | Switching between a busy and an idle tab shows and hides the mascot; Full Screen during a turn moves it to the new layout | Emulator | Two tabs, `stream` |
| ☐ | `burst` shows no line-noise markers that the previous release does not also show | Emulator, then SE/30 | `burst` |
| ☐ | With several prompts, Cmd-↑ and Cmd-↓ step between them, Cmd-↓ after the last goes to the end, Cmd-↑ before the first beeps; still right after `long` forced a trim | Emulator | Stress bridge |
| ☐ | Control-, Option- and Cmd-Option-arrows scroll three lines, a page and to the ends from both the input box and the transcript; plain arrows still move the input cursor | Emulator and SE/30 | Any transcript |
| ☐ | On an extended keyboard Page Up, Page Down, Home and End scroll the transcript | SE/30 | Apple Extended Keyboard |

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
| ☐ | A one-hour session with three tabs shows no misrouted output: every line lands in the tab whose project produced it |
| ☐ | An hour of saves at 57,600 never leaves a host file different from what the window showed |
