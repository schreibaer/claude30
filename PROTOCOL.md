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
