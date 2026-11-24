# Claude30: a native SE/30 client

Claude30 is a System 7 application for a stock Macintosh SE/30. It replaces ZTerm's tty mode with one Mac window:

- The transcript stays readable. Prose wraps to the window and code shows in Monaco.
- Permission requests open a dialog that shows the whole diff, file or command.
- The app dials and hangs up the WiFi232 itself.

It talks to the unchanged `bridge.py` in app mode (see `../PROTOCOL.md`, version 1).

```
SE/30 (Claude30) -> WiFi232 -> Wi-Fi -> socat (run.sh) -> bridge.py -> Claude
```

## What you need

- A Macintosh SE/30 running System 7.5.5. Claude30 asks for 2 MB of memory and runs in 1 MB. It needs no FPU, no Color QuickDraw and no TCP/IP.
- The WiFi232, set up as in `../README.md`: `ATNET0`, with a stored `bridge` alias for the host on port 2324.
- `run.sh` running on the host.
- **AppleTalk off** (Chooser) and **virtual memory off** (Memory control panel). Both hold off serial interrupts, and the SE/30's serial chip has only a 3-byte buffer.

## Getting the app onto the SE/30

The build makes two release files:

- `build-68k/Claude30.bin` is a MacBinary file. It keeps the resource fork, the icon and the creator code.
- `build-68k/Claude30.dsk` is an 800 KB HFS disk image holding the app.

**Over the WiFi232 (no extra hardware):**

1. On the host, run `tools/send-to-mac.sh`. It needs `socat` and `sz` (`brew install lrzsz`). It waits once on port 2325 and accepts only the WiFi232's address.
2. On the SE/30, open ZTerm at 57600 8N1 and turn on MacBinary for received binary files.
3. Type `ATDT<host-ip>:2325`. ZTerm receives the file by ZMODEM and decodes the MacBinary into the application.
4. Hang up with `+++`, wait a second, then type `ATH`. Quit ZTerm so it frees the modem port.

**With a disk image:** copy `Claude30.dsk` to a BlueSCSI or SCSI2SD card, or write it to an 800 KB HFS floppy.

## Using it

1. Open **Edit > Settings…** and check three things: the port (Modem), the speed (57,600) and the dial string (`ATDTbridge`). The dial string goes to the modem exactly as typed.
2. Choose **Session > Connect** (Cmd-K). The status line goes from "Dialing…" to "Connected at 57,600".
3. Type a prompt and press **Return**, or click **Send**.
   - **Option-Return** starts a new line in the same prompt.
   - Bridge commands such as `/new` and `/edits on` work as typed.
4. While Claude works, the status line shows the tool in use.
   - **Cmd-period** (Session > Stop) interrupts Claude, and the transcript marks the turn "Interrupted".
   - If nothing arrives for 90 seconds, for example while a long command runs on the host, the status line counts the quiet time. The session stays connected.
5. A permission request opens a dialog.
   - **Cmd-Y** allows, **Cmd-N** denies and **Cmd-A** allows for the rest of the session. You can also click a button.
   - **Cmd-period** denies and stops the turn.
   - Return and plain keys do nothing, so text you type ahead can't approve anything. Clicks made just before the dialog opens are thrown away, and the buttons ignore clicks for the first three quarters of a second.
   - If the app ran out of memory while the request arrived, the dialog says part of it could not be shown and offers only Deny.
   - Long lines wrap onto rows marked with `¬`, so you see every character of a command.
6. **Session > Disconnect** ends the session and hangs up. Quit also disconnects first.

If the link drops, an alert offers **Redial**. A redial starts a fresh conversation, and the transcript marks it "New conversation".

### Transcript and logs

The window keeps roughly the last 20 to 28 KB of the conversation, which is TextEdit's practical limit. Older text moves to a log file as it leaves the window, so nothing is lost.

- Logs go in a **Claude30 Logs** folder next to the application, one file per session, for example `Session 2026-11-24 10.31.05`.
- They are plain text files that open in SimpleText or TeachText.
- If the log can't be written (for example, the disk is full), the transcript says so, the status line shows "log not saving", and old text stays in the window until it is nearly full. Only then does the oldest text go, and the transcript says how much.

If the serial driver reports lost bytes, the transcript shows "Line noise: output above may be damaged" and the status line says "line noise seen". Protocol version 1 cannot resend lost bytes. If you see this often, use the link test below to pick a slower speed.

## The link test: run this first

The SE/30's serial hardware can drop bytes at 57,600 baud. Prove the link before relying on it.

1. On the host, run `./run.sh --demo`, which spends no tokens.
2. On the SE/30, choose **Session > Link Test…**. Enter a duration (60 minutes) and click Start.
3. The test dials, sends a fixed prompt to the demo again and again, and counts bytes, lines and serial errors. It ends after the duration, or when you press Cmd-period or Stop. Its summary goes into the transcript.
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

`tests/fixtures/record_demo.py` re-records `demo_session.bin`, the bytes `bridge.py --demo` sends in app mode, which the protocol tests replay.

## How it is built

- `src/core/` is portable C with no Toolbox calls. It holds:
  - the line assembler (lines of any length)
  - the protocol state machine: dialing, handshake, flow control, permission requests, stop, disconnect and hangup timing
  - the prompt encoder (backslash continuation lines)
  - the transcript batcher and trim plan
  
  It is compiled into the app and, separately, into the host tests.
- `src/mac/` is the Toolbox shell:
  - `serial.c`: Serial Driver. Opens `.AOut` before `.AIn`, 8N1, handshaking off, a 32 KB input buffer, polled every event-loop pass, and `SerStatus` errors.
  - `modem.c`: ties the core to the port, the window and the dialogs.
  - `window.c`: styled TextEdit transcript, input area, status line.
  - `permdlg.c`: permission dialog with a QuickDraw line list.
  - `prefs.c`: settings resource in the Preferences folder.
  - `log.c`: session logs.
  - `linktest.c`: the link test.

Toolbox managers used: QuickDraw, Font Manager, Window Manager, Menu Manager, TextEdit (styled), Dialog Manager, Control Manager, Event Manager, Device Manager and Serial Driver, File Manager, Resource Manager, Folder Manager (`FindFolder`), Process Manager (`GetProcessInformation`), Scrap Manager and Memory Manager. All of them are in System 7.0 and later. The app uses no floating point.
