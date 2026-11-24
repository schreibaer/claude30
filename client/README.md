# Claude30: a native SE/30 client

Claude30 is a System 7 application for a stock Macintosh SE/30. It replaces ZTerm's tty mode with one Mac window:

- The transcript stays readable. Prose wraps to the window and code shows in Monaco.
- Up to six tabs, each a Claude conversation in its own project on the host, all over one call.
- Permission requests open a dialog that shows the whole diff, file or command.
- You can browse a tab's project, read any text file and edit small ones, without spending a Claude turn.
- The app dials and hangs up the WiFi232 itself.

It talks to `bridge.py` with protocol version 2 (see `../PROTOCOL.md`). Update `bridge.py` on the host and the app on the SE/30 together: this app waits for a version 2 bridge and says so after a minute if the bridge is older. The bridge still speaks version 1, so the previous Claude30 build and ZTerm keep working with the new bridge.

```
SE/30 (Claude30) -> WiFi232 -> Wi-Fi -> socat (run.sh) -> bridge.py -> Claude
```

## What you need

- A Macintosh SE/30 running System 7.5.5. Claude30 asks for 3.5 MB of memory and runs in 2 MB. It needs no FPU, no Color QuickDraw and no TCP/IP.
- The WiFi232, set up as in `../README.md`: `ATNET0`, with a stored `bridge` alias for the host on port 2324.
- `run.sh` running on the host, with `--projects` naming the folder that holds your projects (see `../README.md`).
- **AppleTalk off** (Chooser) and **virtual memory off** (Memory control panel). Both hold off serial interrupts, and the SE/30's serial chip has only a 3-byte buffer.

## Getting the app onto the SE/30

The build makes two release files:

- `build-68k/Claude30.bin` is a MacBinary file. It keeps the resource fork, the icon and the creator code.
- `build-68k/Claude30.dsk` is an 800 KB HFS disk image holding the app.

**Over the WiFi232 (no extra hardware):**

1. On the host, run `tools/send-to-mac.sh`. It needs `socat` and `sz` (`brew install lrzsz socat`). It waits once on port 2325 and accepts only the WiFi232's address. It sends in small windows that ZTerm must acknowledge, because the SE/30 drops bytes in long bursts at 57,600. If a transfer still fails, the script tells you what to try next: smaller bursts through `SZ_OPTS`, or 19,200 baud.
2. On the SE/30, open ZTerm at 57600 8N1 and turn on MacBinary for received binary files.
3. Type `ATDT<host-ip>:2325`. ZTerm receives the file by ZMODEM and decodes the MacBinary into the application.
4. Hang up with `+++`, wait a second, then type `ATH`. Quit ZTerm so it frees the modem port.

**With a disk image:** copy `Claude30.dsk` to a BlueSCSI or SCSI2SD card, or write it to an 800 KB HFS floppy.

## Using it

1. Open **Edit > Settings…** and check three things: the port (Modem), the speed (57,600) and the dial string (`ATDTbridge`). The dial string goes to the modem exactly as typed.
2. Choose **Session > Connect** (Cmd-K). The status line goes from "Dialing…" to "Connected at 57,600".
3. The first time, a list of the host's projects opens. Pick one and click **Open**. It opens in a tab named after the project.
4. Type a prompt and press **Return**, or click **Send**.
   - **Option-Return** starts a new line in the same prompt.
   - Bridge commands such as `/new` and `/edits on` work as typed, and affect only that tab's conversation. `/quit` closes the tab.
5. While Claude works, the status line shows the tool in use.
   - **Cmd-period** (Session > Stop) interrupts Claude in the front tab, and the transcript marks the turn "Interrupted".
   - If nothing arrives for 90 seconds, for example while a long command runs on the host, the status line counts the quiet time. The session stays connected.
6. A permission request opens a dialog.
   - **Cmd-Y** allows, **Cmd-N** denies and **Cmd-A** allows for the rest of the session. You can also click a button.
   - **Cmd-period** denies and stops the turn.
   - Return and plain keys do nothing, so text you type ahead can't approve anything. Clicks made just before the dialog opens are thrown away, and the buttons ignore clicks for the first three quarters of a second.
   - If the app ran out of memory while the request arrived, the dialog says part of it could not be shown and offers only Deny.
   - Long lines wrap onto rows marked with `¬`, so you see every character of a command.
7. **Session > Disconnect** ends every conversation and hangs up. Quit also disconnects first.
8. **File > Full Screen** (Cmd-F) makes the window cover the whole screen and hides the menu bar. Menus still work by their Command keys, and Cmd-F again brings the menu bar back. Quitting or switching to another application also leaves full screen. If the app crashes while in full screen, restart the Mac to get the menu bar back. Full Screen is off while a file window is open.

If the link drops, an alert offers **Redial**. A redial reopens every tab in the same project with a fresh conversation, and each transcript marks it "New conversation".

### Tabs

- **Session > New Tab** (Cmd-T) opens the project list. Projects already open in a tab are marked "(open)". The list is fetched again each time, so a folder you just made on the host shows up without redialing.
- Click a tab, or press **Cmd-1** to **Cmd-6**, to bring it to the front. The **Window** menu lists the tabs and the open file windows.
- **Session > Close Tab**, or **Cmd-W** while the main window is in front, ends that tab's conversation. A tab whose turn is running asks first.
- Tabs keep working in the background. A tab's label shows a bullet while its turn runs, an underline when new output arrived while you were elsewhere, and turns black when it waits for a permission answer. The permission dialog never opens over another tab: the status line says "*project* is waiting for permission", and the dialog opens when you click that tab.
- Each tab keeps its own transcript, input and log file. Switching tabs leaves the input and the scroll position as you left them.
- The open tabs are remembered. Connect reopens them, in the same order. If a project is gone from the host, its tab says it is not available and takes no prompts until you close it.

### Project files

**File > Open Project File…** (Cmd-O) browses the front tab's project, folders first. Double-click a folder to go into it, and **Up** (or Cmd-up arrow) to go back. Typing selects by name. The list hides `.git`, secrets such as `.env` and keys, and anything the project's git ignore rules exclude.

- A file opens in its own window, titled with the file and the project, in Monaco. Up to six file windows can be open. They belong to their tab and hide while another tab is in front.
- **Small text files open editable**: up to 28 KB and 2,000 lines. Type, cut, copy and paste as usual. A bullet in the title means unsaved changes.
- **File > Save** (Cmd-S) writes the file back. The bridge keeps its line endings, final newline and permissions, and replaces the file in one step, so the host never sees half a file. Claude hears at the start of its next turn in that tab which files you saved.
- If the file changed on the host since you opened it, for example because Claude edited it, Save asks before replacing it. **Cancel** is the default and keeps your edits in the window.
- **File > Revert to Saved** throws away your edits and loads the file again.
- **Larger files open read-only** and load in pieces as you scroll. Drag the scroll bar anywhere: the window shows "Loading line N…" and fills in within a few seconds. Files over 32,000 lines show the first 32,000. Read-only windows also open for text that would not survive the trip through Mac Roman (not UTF-8, characters the Mac lacks, control characters, mixed line endings); the bottom strip says why. Binary files are refused.
- Closing a window, closing its tab, disconnecting or quitting asks about unsaved changes. If the link is down, the alert says so and offers only Don't Save and Cancel; after a redial, Save works again.
- Every piece and every save is checked with a CRC-32, and a damaged one is sent again (up to 3 tries), so line noise can't put wrong text on screen or on the host.
- The bridge decides how much the SE/30 may do: `run.sh --files rw` (the default) allows saving, `--files ro` only reading, and `--files off` removes Open Project File from the menu.
- Claude30 never creates, renames or deletes host files. Those stay with Claude, behind its permission prompts.

### Moving around the transcript

You never need the scroll bar. These keys work whether you are typing in the input box or have clicked in the transcript; plain arrow keys still move the cursor in the input box.

| Keys | Moves the transcript |
|---|---|
| Cmd-↑ / Cmd-↓ | to your previous or next prompt (also **Session > Previous Prompt** and **Next Prompt**). Cmd-↓ after your last prompt goes to the end. |
| Control-↑ / Control-↓ | three lines |
| Option-↑ / Option-↓ | one page |
| Cmd-Option-↑ / Cmd-Option-↓ | to the top or the bottom |
| Page Up, Page Down, Home, End | the same, on keyboards that have them |

Prompts that have already moved to the log file can't be reached this way.

While Claude works in the front tab, the Claude mascot paces the empty input box between a word that changes every few seconds ("Warming up the 68030…", or the tool Claude is running) and a tiny compact Mac whose screen flips between a Happy Mac and the wristwatch. When Claude waits for your permission it stands still with its arms up. Once you type ahead, it steps in place beside the Send button so it never covers your text.

**File > Compact Input** (Cmd-E), or the small triangle at the left end of the line above the input box, collapses the input box to a single line so the transcript gets about four more lines. A prompt that needs a second line (Option-Return, or text that wraps) opens the box again until you send it. The setting is remembered.

### Transcript and logs

Each tab keeps roughly the last 20 to 28 KB of its conversation, which is TextEdit's practical limit. Older text moves to a log file as it leaves the window, so nothing is lost, including in background tabs.

- Logs go in a **Claude30 Logs** folder next to the application, one file per tab and conversation, named after the project, the time and the tab, for example `claude30-web 11-24 10.31.05 #2`.
- They are plain text files that open in SimpleText or TeachText.
- If the log can't be written (for example, the disk is full), the transcript says so, the status line shows "log not saving", and old text stays in the window until it is nearly full. Only then does the oldest text go, and the transcript says how much.

If the serial driver reports lost bytes, the transcripts of running tabs show "Line noise: output above may be damaged" and the status line says "line noise seen". Claude's output cannot be resent (file transfers can). If you see this often, use the link test below to pick a slower speed.

## The link test: run this first

The SE/30's serial hardware can drop bytes at 57,600 baud. Prove the link before relying on it.

1. On the host, run `./run.sh --demo`, which spends no tokens.
2. On the SE/30, choose **Session > Link Test…**. Enter a duration (60 minutes) and click Start.
3. The test dials, opens the first project, sends a fixed prompt to the demo again and again, and counts bytes, lines and serial errors. It ends after the duration, or when you press Cmd-period or Stop. Its summary goes into the transcript.
4. If it reports serial errors at 57,600, repeat at 38,400, then at 19,200. Each time, set the same speed in three places: the Settings dialog, `run.sh --baud`, and the WiFi232's own serial speed.

Record the results here:

| Date | Speed | Minutes | Bytes | Serial errors |
|---|---|---|---|---|
| | 57,600 | 60 | | |

## A stuck bridge on the host

If the link drops at an unlucky moment, `bridge.py` can stay running on the host. That moment is while the bridge waits for the client's `!ACK`. The bridge never notices the closed connection while it waits, and fixing that needs a bridge change.

Disconnecting from the app avoids this, because it stops the turn first. If it happens anyway, find and stop the process on the host:

```sh
pgrep -fl bridge.py
kill <pid>
```

## Building

The client builds with [Retro68](https://github.com/autc04/Retro68). Retro68 has no tagged releases, so this project pins a commit: `a4a389c41e90b1095a40ccf13f28678b6cfbba86` (master, 2026-11-10). Build the 68k toolchain from that commit:

```sh
git clone https://github.com/autc04/Retro68.git && cd Retro68
git checkout a4a389c41e90b1095a40ccf13f28678b6cfbba86
git submodule update --init
mkdir ../Retro68-build && cd ../Retro68-build
../Retro68/build-toolchain.bash --no-ppc --no-carbon --multiversal
```

Then build from this repository's root:

```sh
# the app: build-68k/Claude30.bin, Claude30.dsk, Claude30.APPL
cmake -S client -B build-68k \
  -DCMAKE_TOOLCHAIN_FILE=<Retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build-68k

# the portable core's tests, with the host compiler (needs python3 for the bridge test)
cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host
```

Retro68 leaves the Finder's "has bundle" flag clear. A post-build step (`cmake/set_bundle_bit.py`) sets it in the `.bin`, then rebuilds the `.dsk` from it, so the Finder shows the app's icon.

`tests/fixtures/record_demo.py` re-records `demo_session.bin`, the bytes `bridge.py --demo` sends over protocol version 2 with two sessions, which the protocol tests replay.

## How it is built

- `src/core/` is portable C with no Toolbox calls. It holds:
  - the line assembler (lines of any length)
  - the protocol state machine: dialing, the version 2 handshake and project list, up to six sessions, flow control, permission requests, stop, close, disconnect and hangup timing
  - file transfers (`files.c`): listings, piece reads and block saves with CRC-32 checks (`crc32.c`) and retries
  - the prompt encoder (backslash continuation lines with the session prefix)
  - the transcript batcher and trim plan

  It is compiled into the app and, separately, into the host tests.
- `src/mac/` is the Toolbox shell:
  - `serial.c`: Serial Driver. Opens `.AOut` before `.AIn`, 8N1, handshaking off, a 32 KB input buffer, polled every event-loop pass, and `SerStatus` errors.
  - `modem.c`: ties the core to the port, routes each session's events to its tab, and runs the dialogs.
  - `tabs.c`: the tab table, in strip order.
  - `window.c`: the tab strip, each tab's styled TextEdit transcript and input, the status line. Background tabs receive text with drawing clipped away.
  - `picker.c`: the project list (List Manager).
  - `opendlg.c`: Open Project File (List Manager).
  - `filewin.c`: file windows, paging, saving and conflicts.
  - `permdlg.c`: permission dialog with a QuickDraw line list, one pending request per tab.
  - `prefs.c`: settings and remembered tabs in a resource in the Preferences folder.
  - `log.c`: per-tab session logs.
  - `linktest.c`: the link test.

Toolbox managers used: QuickDraw, Font Manager, Window Manager, Menu Manager, TextEdit (styled and plain), Dialog Manager, Control Manager, List Manager (standard LDEF), Event Manager, Device Manager and Serial Driver, File Manager, Resource Manager, Folder Manager (`FindFolder`), Process Manager (`GetProcessInformation`), Text Utilities (`TruncString`), Scrap Manager and Memory Manager. All of them are in System 7.0 and later. The app uses no floating point.
