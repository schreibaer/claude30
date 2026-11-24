#!/usr/bin/env python3
"""
se30bridge - talk to Claude Code from a Macintosh SE/30 (or any dumb terminal).

Runs on the modern host. stdin/stdout are the serial link (socat hands us the
TCP connection from the WiFi232). Two modes:

  tty mode  (default) - for ZTerm or any VT100/dumb terminal. Human readable,
                        80-col wrapped, pager, single-key permission prompts.
  app mode            - tagged line protocol for a native SE/30 client.
                        Entered when the client sends "#HELLO SE30 1".

See PROTOCOL.md for details.

    python bridge.py --cwd ~/code/myproject           # real Claude
    python bridge.py --demo                           # no Claude, test the link
"""

import argparse
import asyncio
import difflib
import os
import sys
import textwrap
import time
import unicodedata

PROTOCOL_VERSION = 1

# --------------------------------------------------------------------------
# Character set handling
# --------------------------------------------------------------------------

# Characters Mac Roman can't show (or that look bad) -> ASCII stand-ins.
_COMMON = {
    " ": " ", "→": "->", "←": "<-", "↑": "^", "↓": "v",
    "⇒": "=>", "↔": "<->", "✓": "v", "✔": "v", "✗": "x",
    "✘": "x", "≤": "<=", "≥": ">=", "≠": "!=", "×": "x",
    "−": "-", "●": "*", "○": "o", "▪": "*", "★": "*",
    "☐": "[ ]", "☑": "[x]", "☒": "[x]", "​": "",
    "️": "", "│": "|", "─": "-",
}
# Extra stand-ins only for 7-bit ASCII (Mac Roman has these natively).
_ASCII_ONLY = {
    "‘": "'", "’": "'", "“": '"', "”": '"',
    "–": "-", "—": "--", "…": "...", "•": "*",
    "°": " deg", "©": "(c)", "®": "(R)", "™": "(TM)",
}


def _boxchar(ch):
    o = ord(ch)
    if 0x2500 <= o <= 0x257F:  # box drawing
        name = unicodedata.name(ch, "")
        if "HORIZONTAL" in name and "VERTICAL" not in name:
            return "-"
        if "VERTICAL" in name and "HORIZONTAL" not in name:
            return "|"
        return "+"
    if 0x2580 <= o <= 0x259F:  # block elements
        return "#"
    return None


def transliterate(s, charset):
    out = []
    for ch in s:
        if ch in _COMMON:
            out.append(_COMMON[ch])
            continue
        if charset == "ascii" and ch in _ASCII_ONLY:
            out.append(_ASCII_ONLY[ch])
            continue
        b = _boxchar(ch)
        if b is not None:
            out.append(b)
            continue
        if ord(ch) >= 0x1F000:  # emoji & pictographs: drop
            continue
        out.append(ch)
    s = "".join(out)
    if charset == "ascii":
        s = unicodedata.normalize("NFKD", s)
        s = "".join(c for c in s if not unicodedata.combining(c))
        return s.encode("ascii", "replace")
    return s.encode("mac_roman", "replace")


def decode_input(b, charset):
    return b.decode("mac_roman" if charset == "macroman" else "latin-1", "replace")


# --------------------------------------------------------------------------
# The serial link
# --------------------------------------------------------------------------

class Link:
    """Raw byte link on stdin/stdout, with optional pacing to the baud rate."""

    def __init__(self, baud):
        # ~10 bits per byte on the wire; leave 10% headroom.
        self.rate = (baud / 10.0) * 0.9 if baud else 0
        self.reader = None
        self._next_ok = 0.0

    async def open(self):
        loop = asyncio.get_running_loop()
        self.reader = asyncio.StreamReader()
        proto = asyncio.StreamReaderProtocol(self.reader)
        await loop.connect_read_pipe(lambda: proto, os.fdopen(0, "rb", 0))
        os.set_blocking(1, True)

    async def send(self, data: bytes):
        for i in range(0, len(data), 64):
            chunk = data[i:i + 64]
            if self.rate:
                now = time.monotonic()
                if self._next_ok > now:
                    await asyncio.sleep(self._next_ok - now)
                self._next_ok = max(now, self._next_ok) + len(chunk) / self.rate
            try:
                os.write(1, chunk)
            except (BrokenPipeError, ConnectionResetError):
                raise SystemExit(0)

    async def read(self, n=256):
        data = await self.reader.read(n)
        if not data:
            raise EOFError
        return data


# --------------------------------------------------------------------------
# Terminal side: input dispatch, rendering, pager, prompts
# --------------------------------------------------------------------------

CTRL_C = 0x03


class Terminal:
    def __init__(self, link, args):
        self.link = link
        self.cols = args.cols
        self.rows = args.rows
        self.tty_charset = args.charset
        self.mode = "tty"
        self.keys = asyncio.Queue()       # tty mode: single bytes
        self.lines = asyncio.Queue()      # app mode: complete lines
        self.acks = asyncio.Semaphore(0)  # app mode flow control
        self.window = args.window
        self.sent_in_window = 0
        self.busy = False                 # a Claude turn is running
        self.on_interrupt = None          # set by the session
        self.lines_since_input = 0
        self.skip_output = False
        self._app_buf = b""
        self._in_fence = False

    @property
    def charset(self):
        return "macroman" if self.mode == "app" else self.tty_charset

    # ---- input ---------------------------------------------------------

    async def dispatch(self):
        """Background task: route incoming bytes."""
        try:
            while True:
                data = await self.link.read()
                for b in data:
                    await self._route(b)
        except EOFError:
            await self.keys.put(None)
            await self.lines.put(None)

    async def _route(self, b):
        if self.mode == "tty":
            if b == CTRL_C and self.busy:
                self.skip_output = False
                if self.on_interrupt:
                    asyncio.ensure_future(self.on_interrupt())
                await self.keys.put(CTRL_C)  # also wakes a pending pager/prompt
                return
            await self.keys.put(b)
            return
        # app mode: assemble lines
        if b == 0x0A:
            return
        if b != 0x0D:
            self._app_buf += bytes([b])
            return
        line = decode_input(self._app_buf, "macroman")
        self._app_buf = b""
        if line == "!ACK":
            self.acks.release()
        elif line == "!STOP":
            if self.busy and self.on_interrupt:
                asyncio.ensure_future(self.on_interrupt())
        else:
            await self.lines.put(line)

    async def read_key(self):
        k = await self.keys.get()
        if k is None:
            raise EOFError
        return k

    async def read_line(self, prompt="> "):
        """Line editor with echo (tty) or a plain line read (app)."""
        if self.mode == "app":
            await self.tag(">")
            line = await self.lines.get()
            if line is None:
                raise EOFError
            self.lines_since_input = 0
            return line
        await self.raw(prompt)
        buf = bytearray()
        while True:
            k = await self.read_key()
            if k == 0x0D:
                await self.raw("\r\n")
                break
            if k in (0x08, 0x7F):
                if buf:
                    buf.pop()
                    await self.link.send(b"\b \b")
            elif k == 0x15:  # ctrl-U
                await self.link.send(b"\b \b" * len(buf))
                buf.clear()
            elif k == CTRL_C:
                buf.clear()
                await self.raw(" ^C\r\n" + prompt)
            elif k >= 0x20 and k != 0x7F or k == 0x09:
                buf.append(k)
                await self.link.send(bytes([k]))
        self.lines_since_input = 0
        line = decode_input(bytes(buf), self.tty_charset)
        if line.strip() == f"#HELLO SE30 {PROTOCOL_VERSION}":
            self.mode = "app"
            await self.tag("#OK", f"se30bridge {PROTOCOL_VERSION}")
            return await self.read_line()
        return line

    async def read_prompt(self):
        """Read a prompt; a trailing backslash continues onto the next line."""
        parts = []
        line = await self.read_line("> ")
        while line.endswith("\\"):
            parts.append(line[:-1])
            line = await self.read_line(". ")
        parts.append(line)
        return "\n".join(parts).strip()

    # ---- output primitives --------------------------------------------

    async def raw(self, s):
        await self.link.send(transliterate(s, self.charset))

    async def tag(self, t, payload=""):
        """App mode: one tagged line, with window-based flow control."""
        line = t if not payload else f"{t} {payload}"
        await self.raw(line + "\r\n")
        if t in (">", "#OK", "W"):
            return
        self.sent_in_window += 1
        if self.window and self.sent_in_window >= self.window:
            await self.raw("W\r\n")
            self.sent_in_window = 0
            await self.acks.acquire()

    async def out_line(self, s=""):
        """tty mode: one physical line, through the pager."""
        if self.skip_output:
            return
        await self.raw(s + "\r\n")
        self.lines_since_input += 1
        if self.lines_since_input >= self.rows - 1:
            await self._more()

    async def _more(self):
        msg = "--More-- [space] page, [return] line, [q] skip"
        await self.raw(msg)
        k = await self.read_key()
        await self.raw("\r" + " " * len(msg) + "\r")
        if k == 0x0D:
            self.lines_since_input = self.rows - 2
        elif k in (ord("q"), ord("Q"), CTRL_C):
            self.skip_output = True
            self.lines_since_input = 0
        else:
            self.lines_since_input = 0

    # ---- semantic output ----------------------------------------------

    async def text(self, md):
        """Claude's prose (markdown-ish)."""
        md = md.replace("**", "").replace("__", "")
        for line in md.split("\n"):
            if line.strip().startswith("```"):
                self._in_fence = not self._in_fence
                if self.mode == "app":
                    await self.tag("C[" if self._in_fence else "C]")
                else:
                    await self.out_line("-" * min(40, self.cols - 1))
                continue
            if self.mode == "app":
                await self._app_text(line)
            elif self._in_fence:
                w = self.cols - 1
                for i in range(0, max(len(line), 1), w):
                    await self.out_line(line[i:i + w])
            else:
                await self._tty_wrap(line)
        if self.mode == "tty" and not self._in_fence:
            await self.out_line()

    async def _tty_wrap(self, line):
        if not line.strip():
            await self.out_line()
            return
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]
        hang = indent
        for bullet in ("- ", "* ", "+ "):
            if stripped.startswith(bullet):
                hang = indent + "  "
        if stripped[:3].rstrip(". ").isdigit() and ". " in stripped[:4]:
            hang = indent + "   "
        for w in textwrap.wrap(line, self.cols - 1, subsequent_indent=hang,
                               break_long_words=True, break_on_hyphens=False) or [""]:
            await self.out_line(w)

    async def _app_text(self, line):
        tag = "C" if self._in_fence else "T"
        # Split long paragraphs at spaces; "+" marks a continuation.
        first = True
        while True:
            if len(line) <= 200:
                await self.tag(tag if first else "+", line)
                return
            cut = line.rfind(" ", 0, 200)
            cut = 200 if cut <= 0 else cut
            await self.tag(tag if first else "+", line[:cut])
            line = line[cut:].lstrip() if tag == "T" else line[cut:]
            first = False

    async def tool(self, name, summary):
        if self.mode == "app":
            await self.tag("U", f"{name} {summary}")
        else:
            await self.out_line(clip(f"  [{name}] {summary}", self.cols - 1))

    async def tool_error(self, msg):
        if self.mode == "app":
            await self.tag("E", clip(msg, 200))
        else:
            await self.out_line(clip(f"  ! {msg}", self.cols - 1))

    async def notice(self, msg):
        if self.mode == "app":
            await self.tag("I", msg)
        else:
            self.skip_output = False
            await self.out_line(msg)

    async def result(self, status, cost):
        cost_s = f"${cost:.4f}" if cost is not None else "-"
        if self.mode == "app":
            await self.tag("R", f"{status} {cost_s}")
        else:
            self.skip_output = False
            await self.out_line(f"({status}, {cost_s})")

    async def ask_permission(self, pid, name, summary, detail_lines):
        """Returns 'y', 'n' or 'a'."""
        if self.mode == "app":
            await self.tag("Q", f"{pid} {name} {summary}")
            if detail_lines:
                await self.tag("D[")
                for d in detail_lines:
                    await self.tag("D", d)
                await self.tag("D]")
            await self.tag("?", str(pid))
            while True:
                line = await self.lines.get()
                if line is None:
                    raise EOFError
                parts = line.split()
                if len(parts) == 3 and parts[0] == "A" and parts[1] == str(pid) \
                        and parts[2] in ("y", "n", "a"):
                    return parts[2]
        # tty
        self.skip_output = False
        self.lines_since_input = 0
        await self.out_line()
        await self.out_line(clip(f"? {name}: {summary}", self.cols - 1))
        for d in detail_lines[: max(self.rows * 3, 10)]:
            await self.out_line(clip("  " + d, self.cols - 1))
        if len(detail_lines) > self.rows * 3:
            await self.out_line(f"  ... {len(detail_lines) - self.rows * 3} more lines")
        while True:
            await self.raw("Allow? [y]es [n]o [a]lways: ")
            k = await self.read_key()
            ch = chr(k).lower() if k < 128 else ""
            if k == CTRL_C:
                ch = "n"
            if ch in ("y", "n", "a"):
                await self.raw(ch + "\r\n")
                self.lines_since_input = 0
                return ch
            await self.raw("\r\n")


def clip(s, n):
    return s if len(s) <= n else s[: n - 3] + "..."


# --------------------------------------------------------------------------
# Describing tool calls compactly
# --------------------------------------------------------------------------

def describe_tool(name, inp, cwd):
    """(one-line summary, detail lines for a permission prompt)"""
    def rel(p):
        if not p:
            return "?"
        try:
            r = os.path.relpath(p, cwd)
            return p if r.startswith("../..") else r
        except ValueError:
            return p

    if name == "Bash":
        cmd = inp.get("command", "")
        detail = cmd.split("\n") if "\n" in cmd or len(cmd) > 60 else []
        return cmd.split("\n")[0], detail
    if name == "Edit":
        path = rel(inp.get("file_path"))
        old = inp.get("old_string", "").splitlines()
        new = inp.get("new_string", "").splitlines()
        diff = list(difflib.unified_diff(old, new, lineterm="", n=2))[2:]
        return path, diff
    if name == "MultiEdit":
        path = rel(inp.get("file_path"))
        diff = []
        for e in inp.get("edits", []):
            diff += list(difflib.unified_diff(e.get("old_string", "").splitlines(),
                                              e.get("new_string", "").splitlines(),
                                              lineterm="", n=1))[2:]
        return path, diff
    if name == "Write":
        path = rel(inp.get("file_path"))
        lines = inp.get("content", "").splitlines()
        return f"{path} ({len(lines)} lines)", ["+" + l for l in lines]
    if name in ("Read", "NotebookRead"):
        return rel(inp.get("file_path") or inp.get("notebook_path")), []
    if name in ("Glob", "Grep"):
        return inp.get("pattern", ""), []
    if name in ("WebFetch",):
        return inp.get("url", ""), []
    if name in ("WebSearch",):
        return inp.get("query", ""), []
    if name in ("Task", "Agent"):
        return inp.get("description", ""), []
    if name == "TodoWrite":
        todos = inp.get("todos", [])
        return f"{len(todos)} items", []
    first = next(iter(inp.values()), "") if inp else ""
    return str(first)[:60], []


# --------------------------------------------------------------------------
# Sessions
# --------------------------------------------------------------------------

SYSTEM_APPEND = (
    "The user is working from a 1989 Macintosh SE/30 over a slow serial link: "
    "a small monochrome screen about {cols} columns wide. Keep replies concise. "
    "Prefer short paragraphs and short code excerpts; don't repeat large files "
    "back. Avoid tables, emoji and decorative Unicode; plain ASCII is best."
)


class ClaudeSession:
    def __init__(self, term, args):
        self.term = term
        self.args = args
        self.client = None
        self.always = set()   # "Tool" or "Bash:<exact command>"
        self.next_pid = 1

    async def start(self):
        from claude_agent_sdk import ClaudeAgentOptions, ClaudeSDKClient
        opts = ClaudeAgentOptions(
            cwd=self.args.cwd,
            permission_mode="acceptEdits" if self.args.accept_edits else "default",
            allowed_tools=["Read", "Glob", "Grep", "TodoWrite"],
            can_use_tool=self._can_use_tool,
            system_prompt={"type": "preset", "preset": "claude_code",
                           "append": SYSTEM_APPEND.format(cols=self.term.cols)},
            setting_sources=["user", "project", "local"],
        )
        if self.args.model:
            opts.model = self.args.model
        self.client = ClaudeSDKClient(options=opts)
        await self.client.connect()
        self.term.on_interrupt = self.interrupt

    async def stop(self):
        if self.client:
            await self.client.disconnect()
            self.client = None

    async def interrupt(self):
        if self.client:
            await self.client.interrupt()

    async def _can_use_tool(self, tool_name, input_data, context):
        from claude_agent_sdk.types import PermissionResultAllow, PermissionResultDeny
        key = f"Bash:{input_data.get('command', '')}" if tool_name == "Bash" else tool_name
        if key in self.always:
            return PermissionResultAllow(updated_input=input_data)
        summary, detail = describe_tool(tool_name, input_data, self.args.cwd)
        pid = self.next_pid
        self.next_pid += 1
        ans = await self.term.ask_permission(pid, tool_name, summary, detail)
        if ans == "a":
            self.always.add(key)
        if ans in ("y", "a"):
            return PermissionResultAllow(updated_input=input_data)
        return PermissionResultDeny(message="The user denied this action.", interrupt=False)

    async def turn(self, prompt):
        from claude_agent_sdk import (AssistantMessage, ResultMessage, TextBlock,
                                      ToolUseBlock, UserMessage)
        try:
            from claude_agent_sdk import ToolResultBlock
        except ImportError:
            ToolResultBlock = None
        await self.client.query(prompt)
        async for msg in self.client.receive_response():
            if isinstance(msg, AssistantMessage):
                for block in msg.content:
                    if isinstance(block, TextBlock) and block.text.strip():
                        await self.term.text(block.text)
                    elif isinstance(block, ToolUseBlock):
                        s, _ = describe_tool(block.name, block.input or {}, self.args.cwd)
                        await self.term.tool(block.name, s)
            elif isinstance(msg, UserMessage) and ToolResultBlock:
                content = msg.content if isinstance(msg.content, list) else []
                for block in content:
                    if isinstance(block, ToolResultBlock) and block.is_error:
                        c = block.content
                        if isinstance(c, list):
                            c = " ".join(x.get("text", "") for x in c if isinstance(x, dict))
                        first = (str(c or "error").strip().splitlines() or ["error"])[0]
                        await self.term.tool_error(first)
            elif isinstance(msg, ResultMessage):
                status = "interrupted" if getattr(msg, "terminal_reason", None) else msg.subtype
                await self.term.result(status, getattr(msg, "total_cost_usd", None))


class DemoSession:
    """Fake Claude, for testing the serial link and the client."""

    def __init__(self, term, args):
        self.term = term
        self.args = args
        self._stop = False

    async def start(self):
        self.term.on_interrupt = self.interrupt

    async def stop(self):
        pass

    async def interrupt(self):
        self._stop = True

    async def turn(self, prompt):
        self._stop = False
        t = self.term
        await t.text(f"You said: “{prompt}” — this is demo mode, no tokens spent.")
        await t.tool("Read", "src/parser.py")
        await asyncio.sleep(0.5)
        await t.text(
            "Here’s a long paragraph to check word wrapping on a narrow screen. "
            "The SE/30 has a 512×342 pixel display, which in Monaco 9 gives roughly "
            "80 columns by 24 rows → plenty for a conversation if lines wrap nicely.\n\n"
            "Some list items:\n"
            "- first item, which is deliberately long enough that it has to wrap onto "
            "a second line with a hanging indent\n"
            "- second item ✓\n\n"
            "```python\ndef parse(s):\n    return [t for t in s.split() if t]  # a fairly long comment that goes past the edge of the screen\n```\n"
            "Box drawing: ┌──┐ │ok│ └──┘  emoji: \U0001F680"
        )
        for i in range(1, 31):
            if self._stop:
                await t.result("interrupted", 0.0)
                return
            await t.text(f"Pager test line {i} of 30.")
        summary, detail = describe_tool("Edit", {
            "file_path": os.path.join(self.args.cwd, "src/parser.py"),
            "old_string": "def parse(s):\n    return s.split()\n",
            "new_string": "def parse(s):\n    return [t for t in s.split() if t]\n",
        }, self.args.cwd)
        ans = await t.ask_permission(1, "Edit", summary, detail)
        await t.tool("Edit", summary + (" (allowed)" if ans in "ya" else " (denied)"))
        await t.result("success", 0.0)


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------

HELP = """Commands:
  /new         start a fresh conversation
  /edits on    auto-accept file edits (off asks each time)
  /help        this text
  /quit        disconnect
Type a prompt and press return. End a line with \\ to continue it.
While Claude works: ctrl-C stops it. At --More--: space, return or q."""


async def main_async(args):
    link = Link(args.baud)
    await link.open()
    term = Terminal(link, args)
    asyncio.ensure_future(term.dispatch())

    def make_session():
        return (DemoSession if args.demo else ClaudeSession)(term, args)

    session = make_session()
    await term.notice(f"se30bridge {'(demo) ' if args.demo else ''}- {args.cwd}")
    await term.notice("Starting Claude..." if not args.demo else "Ready.")
    try:
        await session.start()
    except Exception as e:  # noqa: BLE001
        await term.notice(f"Could not start Claude: {e}")
        return
    await term.notice("Type /help for commands.")

    try:
        while True:
            prompt = await term.read_prompt()
            if not prompt:
                continue
            cmd = prompt.lower().split()
            if cmd[0] == "/quit":
                await term.notice("Bye.")
                break
            if cmd[0] == "/help":
                for line in HELP.split("\n"):
                    await term.notice(line)
                continue
            if cmd[0] == "/new":
                await session.stop()
                session = make_session()
                await session.start()
                await term.notice("New conversation.")
                continue
            if cmd[0] == "/edits" and len(cmd) == 2:
                args.accept_edits = cmd[1] == "on"
                await session.stop()
                session = make_session()
                await session.start()
                await term.notice(f"Auto-accept edits {'on' if args.accept_edits else 'off'}"
                                  " (new conversation).")
                continue
            term.busy = True
            term.skip_output = False
            term._in_fence = False
            if term.mode == "tty":
                await term.out_line("...")
            try:
                await session.turn(prompt)
            except EOFError:
                raise
            except Exception as e:  # noqa: BLE001
                await term.notice(f"Error: {e}")
            finally:
                term.busy = False
    except EOFError:
        pass
    finally:
        await session.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--cwd", default=os.getcwd(), help="project directory for Claude")
    p.add_argument("--demo", action="store_true", help="fake Claude, to test the link")
    p.add_argument("--baud", type=int, default=57600,
                   help="pace output to this serial speed (0 = no pacing)")
    p.add_argument("--cols", type=int, default=80)
    p.add_argument("--rows", type=int, default=24)
    p.add_argument("--charset", choices=["ascii", "macroman"], default="ascii",
                   help="tty mode output charset (app mode is always macroman)")
    p.add_argument("--window", type=int, default=8,
                   help="app mode: lines per flow-control window (0 = off)")
    p.add_argument("--accept-edits", action="store_true",
                   help="auto-approve file edits")
    p.add_argument("--model", default=None)
    args = p.parse_args()
    args.cwd = os.path.abspath(os.path.expanduser(args.cwd))
    try:
        asyncio.run(main_async(args))
    except (KeyboardInterrupt, SystemExit):
        pass


if __name__ == "__main__":
    main()
