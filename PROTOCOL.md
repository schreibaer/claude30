# se30bridge protocol, version 1

The bridge runs on the modern host. Its stdin/stdout is the serial link: SE/30 → WiFi232 → TCP → socat → bridge. There are two modes.

## tty mode (default)

This mode is for ZTerm or any dumb/VT100 terminal. There are no escape sequences, only printable text, CR LF and backspace.

- Output is wrapped to `--cols` (default 80) and sent as 7-bit ASCII by default. With `--charset macroman`, curly quotes and dashes come through.
- The bridge echoes what you type. Backspace/DEL deletes a character, ctrl-U clears the line, and return sends it. A line ending in `\` continues onto the next line.
- After `--rows - 1` lines, a pager appears: `--More--`. Space shows the next page, return shows one more line, and `q` skips the rest of the output for this turn. Permission prompts still appear.
- While Claude is working, ctrl-C interrupts it.
- Permission prompts show the tool, a summary and a diff, if there is one, then ask `Allow? [y]es [n]o [a]lways:` and wait for one key. "Always" lasts for the rest of the session: per tool, or per exact command for Bash.
- Commands: `/new`, `/edits on|off`, `/help`, `/quit`.

## app mode

This is a tagged line protocol for a native SE/30 client.

**Entering it.** The client sends `#HELLO SE30 1` CR. The bridge is still in tty mode at that point, so it echoes this line. The client should ignore everything until it receives `#OK se30bridge 1`. From then on:

- Every line is terminated by CR LF (bridge→client) or CR (client→bridge). A client→bridge LF is ignored.
- Text is **Mac Roman** in both directions.
- The bridge never echoes. The client does its own line editing.
- Bridge lines are `<TAG>` or `<TAG> <payload>`, and are always ≤ 255 bytes.

### Bridge → client

| Line | Meaning |
|---|---|
| `#OK se30bridge 1` | App mode is active |
| `>` | Ready for a prompt (the turn is finished) |
| `T <text>` | Start a new paragraph of Claude's prose. An empty `T` is a blank line. Unwrapped: the client wraps to its window width. |
| `+ <text>` | Continuation of the previous `T` or `C` line, for paragraphs longer than 200 bytes. After `T`, join with one space. After `C`, join with nothing. |
| `C[` / `C]` | Start/end of a code block |
| `C <text>` | One line of code inside a code block. Show it monospaced and don't rewrap it. |
| `U <Tool> <summary>` | Claude used a tool (status line, e.g. `U Read src/parser.py`) |
| `E <message>` | A tool returned an error (first line only) |
| `I <message>` | Informational message from the bridge |
| `Q <id> <Tool> <summary>` | Permission request header... |
| `D[`, `D <line>`, `D]` | ...followed by an optional unified diff or command body... |
| `? <id>` | ...followed by this line: now answer it |
| `R <status> <cost>` | Turn finished. Status is `success`, `interrupted` or an error subtype. Cost is e.g. `$0.0123`. |
| `W` | Flow-control window boundary (see below) |

### Client → bridge

| Line | Meaning |
|---|---|
| `<any text>` | A prompt, sent after `>`. Commands such as `/new`, `/edits on`, `/quit` work here too. |
| `A <id> y` / `A <id> n` / `A <id> a` | Answer permission request `<id>`: yes, no, always |
| `!STOP` | Interrupt the running turn |
| `!ACK` | Acknowledge a `W`: "I've processed everything so far, send more" |

### Flow control

After every `--window` counted lines (default 8), the bridge sends `W` and stops until it receives `!ACK`. The lines `>`, `#OK` and `W` don't count. With this scheme, a burst from Claude can never overrun the SE/30's serial buffer, so you don't need RTS/CTS. The client should send `!ACK` once it has finished drawing the lines received so far. Answer a `W` even in the middle of a permission request.

Separately, `--baud` paces the bridge's raw output to about 90% of the serial line rate, so the WiFi232's buffer doesn't fill up either.

### A typical exchange

```
C: #HELLO SE30 1
B: #OK se30bridge 1
B: >
C: add a test for the parser
B: U Read src/parser.py
B: T I'll add a test covering empty input.
B: Q 1 Write tests/test_parser.py (12 lines)
B: D[
B: D +def test_empty():
...
B: W
C: !ACK
B: D]
B: ? 1
C: A 1 y
B: U Write tests/test_parser.py (12 lines)
B: R success $0.0210
B: >
```

---

# se30bridge protocol, version 2

Version 2 lets one call carry up to six Claude conversations ("sessions") at once, each in its own project folder on the host, and lets the client browse, read and save files in those folders. The bridge speaks v1 app mode, tty mode and v2 in the same program: the client's HELLO line picks the mode.

Everything in the v1 app mode section above still holds unless this section says otherwise: CR LF line ends from the bridge, CR from the client, a client→bridge LF is ignored, Mac Roman text, no echo, bridge lines of at most 255 bytes, and the `+` continuation for payloads over 200 bytes.

## Entering it

The client sends one bare CR, then `#HELLO SE30 2` CR. The bridge is still in tty mode at that point and echoes the line, so the client ignores everything until it receives a line that starts with `#OK se30bridge 2`. Words after the version are capabilities; a client ignores words it does not know. The bridge then sends the project list and the handshake ends at `P]`:

```
#OK se30bridge 2 files=rw
P[
P claude30
P website
P]
```

A v2 client that gets no `#OK se30bridge 2` within 60 seconds should tell the person the bridge may be older than the app. A v1 bridge treats `#HELLO SE30 2` as a prompt.

## Lines

A line whose first byte is a digit `1` to `6` belongs to that session. The digit is followed directly by a tag, then a space and the payload, if any: `2T Hello`, `2>`, `2? 4`. All other lines are link-level. v1 tags never start with a digit, so the two cannot be confused.

Both sides ignore a session line whose tag they do not know, and a line for a session that is not open. There are no checksums or resends for ordinary lines. File transfers have their own (see Files).

### Link-level, bridge → client

| Line | Meaning |
|---|---|
| `#OK se30bridge 2 [words]` | v2 is active. `files=rw`, `files=ro` or `files=off` says what file access allows; no `files=` word means `off`. |
| `P[`, `P <name>`, `P]` | The project list: one line per project the client may open, in display order. Sent after `#OK` and again after each `!LIST`. |
| `W` | Flow-control window boundary, as in v1 |
| `I <message>` | Information about the link, not about one session |

### Link-level, client → bridge

| Line | Meaning |
|---|---|
| `!ACK` | Acknowledge a `W`, as in v1 |
| `!LIST` | Send the project list again |
| `!QUIT` | End the call. The bridge stops every session and exits. |

### Session lines, bridge → client

Every v1 bridge→client tag works per session with the digit in front: `<n>T`, `<n>+`, `<n>C[`, `<n>C`, `<n>C]`, `<n>U`, `<n>E`, `<n>I`, `<n>Q`, `<n>D[`, `<n>D`, `<n>D]`, `<n>?`, `<n>R`, `<n>>`. In addition:

| Line | Meaning |
|---|---|
| `<n>O <name>` | Session `<n>` is open in project `<name>`. `<n>>` follows when it is ready for a prompt. |
| `<n>X <reason>` | Session `<n>` could not be opened (`not available`, `too many tabs`, `already open`, `could not start: ...`), or it ended. |
| `<n>X closed` | The answer to the client's `<n>X`: the session is stopped and its number is free again. |

`<n>>` means only "ready for a new prompt". Unlike v1, the bridge sends no `>` for continuation lines.

### Session lines, client → bridge

| Line | Meaning |
|---|---|
| `<n>O <name>` | Open session `<n>` in the project named `<name>`, exactly as the list spelled it. The client picks `<n>`. The bridge never builds a path from client text: a name that is not in a freshly built list is refused with `<n>X not available`. |
| `<n>X` | Close session `<n>`. The bridge denies any open permission request, stops a running turn, stops the session and answers `<n>X closed`. No `!STOP` is needed first. |
| `<n>P <line>` | One line of a prompt. A line ending in `\` continues on the next `<n>P` line; the bridge joins the lines with line breaks. Commands such as `/new`, `/edits on` and `/help` work and affect only this session. |
| `<n>A <id> y` / `n` / `a` | Answer permission request `<id>` of session `<n>` |
| `<n>!STOP` | Interrupt session `<n>`'s running turn |

### Flow control and limits

There is one `W`/`!ACK` window for the whole link, shared by every session, because it protects the client's serial buffer. Every bridge line counts except `#OK`, `W` and `<n>>`, and that includes the project list, so the client must answer a `W` during the handshake too. A session waiting for a permission answer never holds up the other sessions' output.

Payloads are split at 200 bytes as in v1, so a session line is still at most 255 bytes. The bridge allows `--max-sessions` sessions at once (default 6) and refuses another with `<n>X too many tabs`.

### A typical exchange

```
C: #HELLO SE30 2
B: #OK se30bridge 2 files=rw
B: P[
B: P claude30
B: P website
B: P]
C: 1O claude30
C: 2O website
B: 1O claude30
B: 1>
B: 2O website
B: 2>
C: 2P run the tests and fix what fails
B: 2U Bash npm test
C: 1P explain linebuf.c
B: 1T linebuf.c collects bytes into lines...
B: 2Q 1 Bash npm test -- --fix
B: 2? 1
B: W
C: !ACK
B: 1R success $0.0110
B: 1>
C: 2A 1 y
B: 2R success $0.0420
B: 2>
C: !QUIT
```

## Files

File lines are session lines. Each request names a path relative to the session's project folder, so the bridge resolves it against that folder and nothing else. `<x>` is a transfer number the client chooses; every retry uses a fresh one, so late lines from an earlier try are dropped as unknown. Paths come last on a line because they may contain spaces. They use `/` between folders, and the empty path is the project folder itself.

What the bridge allows depends on `--files`: `rw` (the default) allows listing, reading and saving, `ro` allows listing and reading, and `off` refuses every file line with `files off`.

### What the bridge shows

A listing never shows, and a request is refused as `hidden` for:

- `.git`, `.ssh`, and names matching `.env*`, `*.pem`, `*.key`, `id_rsa*` or `id_ed25519*` in any letter case, even when git tracks them;
- anything the project's git ignore rules exclude (when the project is a git repository and git is installed). If git fails inside a repository, the paths it was asked about count as ignored;
- these rules apply both to the path as sent and to the real path after symbolic links are resolved, so a link cannot show a hidden file under another name;
- a symbolic link whose target is outside the project folder (refused as `outside project`);
- names holding a control character (refused as `bad name`), or that Mac Roman cannot spell.

Requests are refused as `outside project` for absolute paths, `.` or `..` components, empty components and NUL bytes.

### Lines

| Client → bridge | Bridge → client |
|---|---|
| `<n>L <path>` | `<n>L[ <path>`, then one `<n>L <d\|f> <size> <name>` per entry, then `<n>L]`, or `<n>L] cut` after 500 entries |
| `<n>G <x> <first> <count> <budget> <path>` | `<n>H <x> <first> <count> <total> <token> <mode>`, then `<count>` `<n>B <line>` lines, then `<n>Z <x> <bytes> <crc>` |
| `<n>F <x> <token> <path>` | `<n>K <x> 0`: the save may start |
| `<n>S[ <x> <seq>`, `<n>S <line>` lines, `<n>S] <x> <seq> <bytes> <crc>` | `<n>K <x> <seq>`, or `<n>N <x> <seq> resend` |
| `<n>S. <x> <bytes> <crc> [force]` | `<n>K <x> saved <token>`, or `<n>N <x> changed`, or `<n>N <x> <reason>` |
| any refused request | `<n>N <x> <reason>` (`<n>N - <reason>` for a listing) |

**Listing.** Folders come first, then files, each sorted by name. `<size>` is the size in bytes (0 for folders).

**Reading.** Lines are numbered from 0. `G` asks for up to `<count>` lines from `<first>`, and at most `<budget>` bytes. The bridge sends at most 200 lines and 4,096 bytes, and always at least one line when one exists. `H` gives the actual first line and count, the file's total line count, and the version token. `<count>` of 0 asks only for `H` and the trailer, which checks whether the file changed. A `B` line longer than 200 bytes continues on `<n>+` lines, joined with nothing.

`<mode>` is `e` when the file may be edited and saved, or `r:<reason>` when it opens read-only:

| Reason | Meaning |
|---|---|
| `big` | Over the edit limit: 28,672 bytes of Mac Roman text or 2,000 lines |
| `long` | Over 32,000 lines, or over 8 MB; only the first 32,000 lines (of the first 8 MB) are sent, and `<total>` counts the lines sent |
| `encoding` | Not UTF-8; shown as Latin-1 |
| `chars` | Holds characters Mac Roman lacks |
| `control` | Holds control characters other than tab |
| `endings` | Mixes line endings |
| `ro` | The bridge runs with `--files ro` |

Editable lines are the file's exact text in Mac Roman, tabs included. Read-only lines go through the same stand-ins as Claude's prose, every remaining control character but tab becomes `?`, and lines over 2,000 bytes are cut and end in `…`. A file with a NUL byte in its first 8 KB is refused as `binary`.

**The trailer and CRCs.** `<bytes>` and `<crc>` cover exactly the piece's lines as Mac Roman bytes joined with CR (no CR after the last line). The CRC is CRC-32 as in zlib (`crc32("123456789")` is `cbf43926`), written as 8 lowercase hex digits. A client that sees a wrong count or CRC, no trailer within 20 seconds, or a serial error during the piece asks again with a new transfer number. Before a retry after a timeout it sends one spare `!ACK`, in case a `W` was lost. After 3 tries it gives up.

**The version token** is an opaque string that changes whenever the file's bytes change.

**Saving.** The client sends `F` with the token it read, then the window's text in blocks of about 2 KB, split at line breaks. Each block is a run of `<n>S` lines (an empty line is `<n>S` with an empty payload), framed by `S[` and `S]`; the `S]` count and CRC cover the block's lines joined with CR. The bridge answers each block with `K`, or with `N ... resend` when the count or CRC is wrong, and the client sends that block again. Blocks are numbered from 1.

`S.` commits the save. Its count and CRC cover the whole text, which is every block's lines joined with CR. The bridge then checks the whole-file CRC, converts CR back to the file's own line ending, keeps the file's final newline (or its absence) and encodes the text as UTF-8. If the file's current token is not the one `F` named, it answers `changed` and keeps the blocks, so the client can send `S.` again with `force` to replace the file anyway. Otherwise it writes a temporary file beside the original, copies the file's permissions, flushes it to disk and renames it over the original, so the file is always either the old or the complete new version. A commit sent again for a save that already finished gets the same answer again. A save left idle for 2 minutes is dropped.

After a save, the bridge tells Claude at the start of that session's next turn which files the person saved.

Refusal reasons: `not found`, `hidden`, `outside project`, `bad name`, `binary`, `read-only`, `too big`, `changed`, `damaged` (the whole-file CRC did not match), `control` (the saved text holds a control character other than tab), `files off`, `busy`, `failed <detail>`.

### One save with a damaged block

```
C: 2F 7 9f2c41a0b7e3 README.md
B: 2K 7 0
C: 2S[ 7 1  ... 2S] 7 1 2048 1a2b3c4d
B: 2K 7 1
C: 2S[ 7 2  ... 2S] 7 2 1310 55aa0f13
B: 2N 7 2 resend
C: 2S[ 7 2  ... 2S] 7 2 1310 55aa0f13
B: 2K 7 2
C: 2S. 7 3359 c0ffee11
B: 2K 7 saved 77d0e2c41b09
```
