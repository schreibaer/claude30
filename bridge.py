#!/usr/bin/env python3
"""
se30bridge - talk to Claude Code from a Macintosh SE/30 (or any dumb terminal).

Runs on the modern host. stdin/stdout are the serial link (socat hands us the
TCP connection from the WiFi232). Two modes:

  tty mode  (default) - for ZTerm or any VT100/dumb terminal. Human readable,
                        80-col wrapped, pager, single-key permission prompts.
  app mode            - tagged line protocol for a native SE/30 client.
                        "#HELLO SE30 1" enters version 1 (one conversation);
                        "#HELLO SE30 2" enters version 2: up to six
                        conversations ("tabs") in different projects, plus
                        browsing, reading and saving project files.

See PROTOCOL.md for details.

    python bridge.py --cwd ~/code/myproject           # real Claude
    python bridge.py --projects ~/code                # tabs: any folder in ~/code
    python bridge.py --demo                           # no Claude, test the link
"""

import argparse
import asyncio
import contextlib
import difflib
import errno
import fcntl
import fnmatch
import hashlib
import json
import os
import re
import secrets
import shutil
import signal
import stat
import sys
import tempfile
import textwrap
import time
import unicodedata
import uuid
import zlib

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
        self.closed = False
        self._next_ok = 0.0
        # One write at a time, so paced chunks of two lines never interleave.
        self._lock = asyncio.Lock()

    async def open(self):
        loop = asyncio.get_running_loop()
        self.reader = asyncio.StreamReader()
        proto = asyncio.StreamReaderProtocol(self.reader)
        await loop.connect_read_pipe(lambda: proto, os.fdopen(0, "rb", 0))
        os.set_blocking(1, True)

    async def send(self, data: bytes):
        async with self._lock:
            for i in range(0, len(data), 64):
                if self.closed:
                    raise EOFError
                chunk = data[i:i + 64]
                if self.rate:
                    now = time.monotonic()
                    if self._next_ok > now:
                        await asyncio.sleep(self._next_ok - now)
                    self._next_ok = max(now, self._next_ok) + len(chunk) / self.rate
                try:
                    os.write(1, chunk)
                except (BrokenPipeError, ConnectionResetError, OSError):
                    self.closed = True
                    raise EOFError

    async def read(self, n=256):
        data = await self.reader.read(n)
        if not data:
            raise EOFError
        return data


# --------------------------------------------------------------------------
# Terminal side: input dispatch, rendering, pager, prompts
# --------------------------------------------------------------------------

CTRL_C = 0x03


class EnterV2(Exception):
    """The client sent the protocol version 2 HELLO."""


def split_payload(tag, line):
    """App mode: (tag, payload) pairs for one line, split at 200 bytes.
    After the first piece the tag is "+". Prose splits at a space."""
    out = []
    first = True
    while True:
        if len(line) <= 200:
            out.append((tag if first else "+", line))
            return out
        cut = line.rfind(" ", 0, 200)
        cut = 200 if cut <= 0 else cut
        out.append((tag if first else "+", line[:cut]))
        line = line[cut:].lstrip() if tag == "T" else line[cut:]
        first = False


def strip_marks(s, keep):
    """Prose as the app got it before !MD: bold marks removed. An app that
    sent !MD (keep) gets them, and code lines with them, as Claude wrote."""
    return s if keep else s.replace("**", "").replace("__", "")


def app_text_lines(md, fence):
    """Claude's prose as app-mode (tag, payload) pairs. fence is an object
    whose _in_fence attribute tracks code fences across calls; its keep_marks
    attribute, when true, leaves ** and __ in the text (see !MD)."""
    out = []
    keep = bool(getattr(fence, "keep_marks", False))
    for line in strip_marks(md, keep).split("\n"):
        if line.strip().startswith("```"):
            fence._in_fence = not fence._in_fence
            out.append(("C[" if fence._in_fence else "C]", ""))
            continue
        out += split_payload("C" if fence._in_fence else "T", line)
    return out


class StreamChunker:
    """One streamed text block: its deltas in, app-mode (tag, payload) pairs
    out, so that the app draws the same text as app_text_lines gives for the
    whole block. Prose goes out in pieces ending at a space (a T, then +
    pieces the app joins with one space) once about MIN bytes of whole words
    wait, or after WAIT seconds when a whole word does. A line that is or may
    become a code fence, and every line inside a fence, waits for its newline
    so code stays one complete line per C. fence is an object with an
    _in_fence attribute, as for app_text_lines."""

    MIN = 100
    WAIT = 0.5

    def __init__(self, fence, clock=time.monotonic):
        self.fence = fence
        self.clock = clock
        self.line = ""          # the current line's text not sent yet
        self.opened = False     # the T of the current line is out
        self.last = clock()     # when a piece last went out

    def feed(self, delta, force=False):
        """The pieces that are ready now. force sends every whole word."""
        out = []
        while True:
            nl = delta.find("\n")
            if nl < 0:
                break
            self.line += delta[:nl]
            delta = delta[nl + 1:]
            self._end_line(out)
        self.line += delta
        self._partial(out, force)
        return out

    def finish(self):
        """The block ended: the rest, as a last line without a newline."""
        out = []
        self._end_line(out)
        return out

    def _plain(self, s):
        return strip_marks(s, bool(getattr(self.fence, "keep_marks", False)))

    def _end_line(self, out):
        line, opened = self.line, self.opened
        self.line, self.opened = "", False
        if not opened and line.strip().startswith("```"):
            self.fence._in_fence = not self.fence._in_fence
            out.append(("C[" if self.fence._in_fence else "C]", ""))
        elif self.fence._in_fence:
            out += split_payload("C", self._plain(line))
        else:
            text = self._plain(line)
            if not opened:
                out += split_payload("T", text)
            elif text.strip():
                out += split_payload("+", text.lstrip())
        self.last = self.clock()

    def _partial(self, out, force):
        if self.fence._in_fence:
            return
        head = self.line.lstrip()
        if not self.opened and (not head or "```".startswith(head) or head.startswith("```")):
            return      # a fence line, or one that may become one
        i = self.line.rfind(" ")
        if i <= 0:
            return
        ready = self.line[:i]
        now = self.clock()
        if not (force or len(ready) >= self.MIN or (now - self.last >= self.WAIT and ready.strip())):
            return
        self.line = self.line[i:].lstrip()
        while ready:
            if len(ready) > 200:
                cut = ready.rfind(" ", 0, 200)
                cut = 200 if cut <= 0 else cut
                piece, ready = ready[:cut], ready[cut:].lstrip()
            else:
                piece, ready = ready, ""
            piece = self._plain(piece)
            if piece.strip():
                out.append(("+" if self.opened else "T", piece))
                self.opened = True
        self.last = now


class BlockStream:
    """One text block of Claude's answer, from its first delta to the
    AssistantMessage that carries it whole. While the term's stream switch
    is on, the block goes out as pieces as it grows; with it off, the whole
    block goes out when it ends. Turning the switch off mid-block leaves the
    block streamed: the rest is sent at its end. Turning it on mid-block
    sends what has been written so far at once."""

    def __init__(self, term, clock=time.monotonic):
        self.term = term
        self.chunker = StreamChunker(term, clock)
        self.text = ""
        self.fed = 0            # how much of text the chunker has seen
        self.streamed = False   # some of it went out as pieces
        self.flushed = False    # the chunker was finished
        self.claimed = False    # its AssistantMessage came

    def _on(self):
        return bool(getattr(self.term, "stream", False))

    async def _feed(self, force=False):
        if self.flushed or self.fed >= len(self.text):
            return
        new, self.fed = self.text[self.fed:], len(self.text)
        pairs = self.chunker.feed(new, force)
        if pairs:
            await self.term.stream_pieces(pairs)

    async def delta(self, d):
        self.text += d
        if not self.claimed and self._on() and self.text.strip():
            was = self.streamed
            self.streamed = True
            await self._feed(force=not was)

    async def switched(self):
        """The stream switch just changed."""
        if not self.claimed and self._on() and self.text.strip():
            self.streamed = True
            await self._feed(force=True)

    async def flush(self):
        if not self.streamed or self.flushed:
            return
        await self._feed()
        self.flushed = True
        pairs = self.chunker.finish()
        if pairs:
            await self.term.stream_pieces(pairs)

    async def end(self, whole):
        """Its AssistantMessage came, carrying the whole text."""
        self.claimed = True
        if self.streamed:
            await self.flush()
        else:
            await self.term.text(whole)


class Terminal:
    """tty mode and v1 app mode, plus the input routing and the one writer
    that v2 sessions share."""

    def __init__(self, link, args):
        self.link = link
        self.cols = args.cols
        self.rows = args.rows
        self.tty_charset = args.charset
        self.mode = "tty"                 # "tty", "app" (v1) or "app2" (v2)
        self.keys = asyncio.Queue()       # tty mode: single bytes
        self.lines = asyncio.Queue()      # app modes: complete lines
        self.acks = asyncio.Semaphore(0)  # app mode flow control
        self.window = args.window
        self.sent_in_window = 0
        self.eof = False
        self.busy = False                 # a Claude turn is running (tty, v1)
        self.on_interrupt = None          # set by the session
        self.lines_since_input = 0
        self.skip_output = False
        self._app_buf = b""
        self._in_fence = False
        self._wlock = asyncio.Lock()
        self.typed = asyncio.Event()      # tty mode: the first key arrived
        self.idle_timeout = args.idle_timeout
        self.last_rx = time.monotonic()   # when the last byte arrived
        self._watchdog = None             # armed by the first !PING

    @property
    def charset(self):
        return "macroman" if self.mode != "tty" else self.tty_charset

    # ---- input ---------------------------------------------------------

    async def dispatch(self):
        """Background task: route incoming bytes."""
        try:
            while True:
                data = await self.link.read()
                self.last_rx = time.monotonic()
                for b in data:
                    await self._route(b)
        except EOFError:
            self.lose_link()

    def lose_link(self):
        """The call is over: end of input, a signal, or the app went silent.
        Nothing more is sent, a writer waiting for !ACK gives up, and every
        reader sees the end, so the normal shutdown runs."""
        if self.eof:
            return
        self.eof = True
        self.link.closed = True
        self.acks.release()
        self.keys.put_nowait(None)
        self.lines.put_nowait(None)

    async def _route(self, b):
        if self.mode == "tty":
            self.typed.set()
            if b == CTRL_C and self.busy:
                self.skip_output = False
                if self.on_interrupt:
                    asyncio.ensure_future(self.on_interrupt())
                await self.keys.put(CTRL_C)  # also wakes a pending pager/prompt
                return
            await self.keys.put(b)
            return
        # app modes: assemble lines
        if b == 0x0A:
            return
        if b != 0x0D:
            self._app_buf += bytes([b])
            return
        line = decode_input(self._app_buf, "macroman")
        self._app_buf = b""
        if line == "!ACK":
            self.acks.release()
        elif line == "!PING" and self.mode == "app2":
            self._pong()
        elif line == "!STOP" and self.mode == "app":
            if self.busy and self.on_interrupt:
                asyncio.ensure_future(self.on_interrupt())
        else:
            await self.lines.put(line)

    def _pong(self):
        """Answers a heartbeat at once: one whole line, outside the window
        and its lock, so it gets through while a writer waits for !ACK.
        The first one arms the idle watchdog; apps that never ping are never
        timed out."""
        async def send():
            with contextlib.suppress(EOFError):
                await self.link.send(b"#PONG\r\n")
        asyncio.ensure_future(send())
        if self._watchdog is None and self.idle_timeout:
            self._watchdog = asyncio.ensure_future(self._watch())

    async def _watch(self):
        while not self.eof:
            left = self.last_rx + self.idle_timeout - time.monotonic()
            if left <= 0:
                log(f"nothing from the app for {self.idle_timeout:g} s; ending the call")
                self.lose_link()
                return
            await asyncio.sleep(left)

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
        if line.strip() == "#HELLO SE30 1":
            self.mode = "app"
            await self.tag("#OK", "se30bridge 1")
            return await self.read_line()
        if line.strip() == "#HELLO SE30 2":
            self.mode = "app2"
            # Bytes typed after HELLO were queued as tty keys: make them lines.
            pending = []
            while not self.keys.empty():
                pending.append(self.keys.get_nowait())
            for k in pending:
                if k is None:
                    await self.lines.put(None)
                else:
                    await self._route(k)
            raise EnterV2
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

    async def send_line(self, data: bytes, counted=True):
        """App modes: one whole line, with window-based flow control. Shielded,
        so a cancelled caller can't leave a W on the wire without its wait."""
        await asyncio.shield(self._send_line(data, counted))

    async def _send_line(self, data, counted):
        async with self._wlock:
            await self.link.send(data + b"\r\n")
            if not counted:
                return
            self.sent_in_window += 1
            if self.window and self.sent_in_window >= self.window:
                await self.link.send(b"W\r\n")
                self.sent_in_window = 0
                if not self.eof:
                    await self.acks.acquire()

    async def tag(self, t, payload=""):
        """v1 app mode: one tagged line."""
        line = t if not payload else f"{t} {payload}"
        await self.send_line(transliterate(line, "macroman"),
                             counted=t not in (">", "#OK", "W"))

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
        if self.mode == "app":
            for t, payload in app_text_lines(md, self):
                await self.tag(t, payload)
            return
        md = md.replace("**", "").replace("__", "")
        for line in md.split("\n"):
            if line.strip().startswith("```"):
                self._in_fence = not self._in_fence
                await self.out_line("-" * min(40, self.cols - 1))
                continue
            if self._in_fence:
                w = self.cols - 1
                for i in range(0, max(len(line), 1), w):
                    await self.out_line(line[i:i + w])
            else:
                await self._tty_wrap(line)
        if not self._in_fence:
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

    async def tool(self, name, summary):
        if self.mode == "app":
            await self.tag("U", f"{name} {summary}")
        else:
            await self.out_line(clip(f"  [{name}] {summary}", self.cols - 1))

    # Subagents: ZTerm, the v1 app and the old v2 apps get one line when a
    # subagent starts and one when it ends, and nothing between.
    async def agent_start(self, k, typ, task):
        await self.tool("Agent", f"{typ}: {clip(task, 120)}")

    async def agent_step(self, k, steps, tool, summary):
        pass

    async def agent_end(self, k, typ, status, steps, summary):
        await self.tool("Agent", f"{typ} {status}: {clip(summary, 120)}")

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

    async def result(self, status, cost, meter=None):
        cost_s = f"${cost:.4f}" if cost is not None else "-"
        if self.mode == "app":
            await self.tag("R", f"{status} {cost_s}")
        else:
            self.skip_output = False
            pct = meter.percent() if meter else None
            ctx = f", ctx {pct}%" if pct is not None else ""
            await self.out_line(f"({status}, {cost_s}{ctx})")

    async def usage(self, meter):
        """tty and v1 have no usage line; the result line says it."""

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
        if inp.get("replace_all"):
            path += " (all occurrences)"   # the diff shows one match; the edit changes every one
        return path, diff
    if name == "MultiEdit":
        path = rel(inp.get("file_path"))
        if any(e.get("replace_all") for e in inp.get("edits", [])):
            path += " (all occurrences)"
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
    if name == "ExitPlanMode":
        return "Approve plan", str(inp.get("plan") or "").split("\n")
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
    "back. {style}"
)

# The style sentence for both prompts. A client that sent !MD draws Markdown
# (see PROTOCOL.md, Markdown prose); ZTerm, tty mode and older apps show it
# as typed.
STYLE_PLAIN = "Avoid tables, emoji and decorative Unicode; plain ASCII is best."
STYLE_MD = (
    "The screen draws Markdown: **bold**, *italic*, `code`, # headings, - lists, "
    "> quotes, --- rules, [links](url) and fenced code; tables only when narrow, "
    "in a fixed-width font. No strikethrough, emoji or decorative Unicode; plain "
    "ASCII is best."
)


def style_sentence(term):
    return STYLE_MD if getattr(term, "keep_marks", False) else STYLE_PLAIN


# A chat (v2 only) is a conversation outside any project: its own empty
# folder, web search and fetch as the only tools, and this short prompt in
# place of Claude Code's own.
CHAT_PROMPT = (
    "You are chatting with someone on a 1989 Macintosh SE/30 over a slow serial "
    "link: a small monochrome screen about {cols} columns wide. This is a casual "
    "conversation, not a coding session. You can look things up with web search "
    "and web fetch. You have no other tools: you cannot read or change files or "
    "run commands. Keep replies short and conversational. Prefer short paragraphs. "
    "{style} Don't repeat large amounts of text back."
)
CHAT_TOOLS = ["WebSearch", "WebFetch"]


AGENT_DIRS = (("project", None), ("user", "~/.claude/agents"))
AGENT_FILES_MAX = 50


def read_agent_file(path):
    """(name, description) from an agent file's frontmatter, or None when it
    cannot be read. Only the first 4 KB are read."""
    try:
        with open(path, "rb") as f:
            head = f.read(4096).decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return None
    lines = head.splitlines()
    if not lines or lines[0].strip() != "---":
        return None
    meta = {}
    for line in lines[1:]:
        if line.strip() == "---":
            break
        m = re.match(r"^(name|description):\s*(.*)$", line)
        if m:
            meta[m.group(1)] = m.group(2).strip().strip("\"'")
    else:
        return None
    stem = os.path.splitext(os.path.basename(path))[0]
    return (meta.get("name") or stem, meta.get("description") or "")


def agents_lines(cwd, init_names):
    """The /agents listing: each agent once, with where it is defined."""
    found = {}
    for where, d in AGENT_DIRS:
        d = os.path.join(cwd, ".claude", "agents") if d is None else os.path.expanduser(d)
        try:
            root = os.path.realpath(d)
            names = sorted(n for n in os.listdir(d) if n.endswith(".md"))[:AGENT_FILES_MAX]
        except OSError:
            continue
        for fn in names:
            path = os.path.join(d, fn)
            real = os.path.realpath(path)
            if not (real == root or real.startswith(root + os.sep)) or not os.path.isfile(real):
                continue                       # a link that leaves the folder
            got = read_agent_file(path)
            name, desc = got if got else (os.path.splitext(fn)[0],
                                          "(unreadable)")
            found.setdefault(name, (where, desc or "(no description)"))
    out = [f"{n} ({w}): {clip(d, 100)}" for n, (w, d) in found.items()]
    if init_names:
        out += [f"{n} (built in)" for n in init_names if n not in found]
    else:
        out.append("Built-in agents are listed after the first reply.")
    out.append("New or changed agent files apply after /new.")
    return [clip(l, 120) for l in out]


class AgentTracker:
    """Subagents Claude has started in this conversation. The main
    conversation's Agent tool call starts one; everything the subagent then
    says or runs carries its parent_tool_use_id and is counted here but never
    sent to the SE/30, which gets a start, throttled progress and an end."""

    STEP_EVERY = 5.0      # seconds between progress lines for one subagent
    MAX_OPEN = 99

    def __init__(self, term, clock=time.monotonic):
        self.term = term
        self.clock = clock
        self.running = {}       # tool_use_id -> dict
        self.ended = set()      # tool_use_ids already ended this turn
        self.background = {}    # tool_use_id -> dict, launched in the background
        self.interrupted = False

    def _free_slot(self):
        used = {a["k"] for a in self.running.values()}
        return next((k for k in range(1, self.MAX_OPEN + 1) if k not in used), None)

    async def start(self, tool_use_id, inp):
        k = self._free_slot()
        if k is None or tool_use_id in self.running:
            return
        typ = re.sub(r"\s+", "-", str(inp.get("subagent_type") or "general-purpose").strip())
        task = " ".join(str(inp.get("description") or inp.get("prompt") or "").split())
        self.running[tool_use_id] = {"k": k, "type": typ, "task": task, "steps": 0,
                                     "tool": "", "summary": "", "sent": (0, ""),
                                     "last": self.clock(), "task_id": None,
                                     "bg": bool(inp.get("run_in_background"))}
        await self.term.agent_start(k, typ, task)

    def owns(self, tool_use_id):
        return (tool_use_id in self.running or tool_use_id in self.ended
                or tool_use_id in self.background)

    def bind(self, tool_use_id, task_id):
        a = self.running.get(tool_use_id) or self.background.get(tool_use_id)
        if a and task_id:
            a["task_id"] = task_id

    async def step(self, tool_use_id, name=None, summary="", steps=None, force=False):
        """One more tool the subagent used (or the CLI's own count and tool)."""
        b = self.background.get(tool_use_id)
        if b:
            # Counted for its end line; its line is not live while it runs.
            if name is not None:
                b["steps"] = max(b["steps"] + (1 if steps is None else 0), steps or 0)
            elif steps is not None:
                b["steps"] = max(b["steps"], steps)
            return
        a = self.running.get(tool_use_id)
        if not a:
            return
        if name is not None:
            a["steps"] = max(a["steps"] + (1 if steps is None else 0), steps or 0)
            a["tool"], a["summary"] = name, summary
        elif steps is not None:
            a["steps"] = max(a["steps"], steps)
        now = self.clock()
        if (a["steps"], a["tool"]) == a["sent"] or (not force and now - a["last"] < self.STEP_EVERY):
            return
        a["sent"] = (a["steps"], a["tool"])
        a["last"] = now
        await self.term.agent_step(a["k"], a["steps"], a["tool"] or "-", a["summary"])

    async def end(self, tool_use_id, status, summary=""):
        b = self.background.pop(tool_use_id, None)
        if b:
            # A background subagent ended, maybe turns later: its own line
            # went with the turn that started it, so it gets a new one.
            k = self._free_slot()
            if k is None:
                return
            await self.term.agent_start(k, b["type"], b["task"])
            await self.term.agent_end(k, b["type"], status, b["steps"], summary or b["task"])
            return
        a = self.running.pop(tool_use_id, None)
        if not a:
            return
        self.ended.add(tool_use_id)
        await self.term.agent_end(a["k"], a["type"], status, a["steps"], summary or a["task"])

    async def end_result(self, tool_use_id, content, is_error):
        """The Agent tool's own result, when the CLI sent no notification."""
        a = self.running.get(tool_use_id)
        if a and a["bg"] and not is_error:
            # Only the launch was confirmed. Close its line now, since the
            # turn may end long before it does; its result comes as a
            # notification, between turns or in a later one.
            del self.running[tool_use_id]
            self.background[tool_use_id] = a
            await self.term.agent_end(a["k"], a["type"], "background", a["steps"],
                                      "runs on, its result follows")
            return
        status = "failed" if is_error else "done"
        if is_error and self.interrupted:
            status = "stopped"
        await self.end(tool_use_id, status, first_line(content))

    async def close_all(self):
        for tid in list(self.running):
            await self.end(tid, "stopped")

    def label_for(self, agent_id):
        """What to put in front of a permission request: nothing for the main
        conversation, the subagent's type when it can be told, else "subagent"."""
        if not agent_id:
            return ""
        for a in list(self.running.values()) + list(self.background.values()):
            if a["task_id"] == agent_id:
                return a["type"]
        if len(self.running) == 1:
            return next(iter(self.running.values()))["type"]
        return "subagent"


def result_text(content):
    if isinstance(content, list):
        content = " ".join(x.get("text", "") for x in content if isinstance(x, dict))
    return str(content or "")


def first_line(content):
    for line in result_text(content).splitlines():
        if line.strip():
            return line.strip()
    return ""


def new_conversation(cfg):
    """tty and v1: the next session starts a fresh conversation."""
    cfg.claude_id = None
    cfg.resume = False
    cfg.always = set()
    cfg.plan = False
    cfg.turns = 0
    cfg.meter = UsageMeter()


CHANGE = object()   # wakes a Channel's prompt loop to apply a settings change
WAKE = object()     # ... or to show what Claude sent between turns


class UsageMeter:
    """What one conversation has used and cost, for the usage line (v2),
    the tty result line and /usage. Context is the size of the latest main
    conversation answer: its input, cached input and output tokens, over the
    window the CLI reports for that model. Cost is a running total: the CLI's
    total_cost_usd runs per process (it starts again at zero in a restarted
    or resumed CLI and after a conversation reset), so the meter adds each
    turn's difference. Nothing here asks Claude for anything."""

    def __init__(self):
        self.total = 0.0
        self.cost_known = False
        self.turns = 0
        self.turn_cost = None   # the last turn's own cost, None when unknown
        self.used = None        # tokens in context after the last answer
        self.window = None      # the model's context window
        self._proc = 0.0        # the CLI process's total_cost_usd at the last result
        self._answer = None     # (tokens, model) of this turn's latest answer

    def new_process(self):
        """A new CLI process (or a conversation reset) counts from zero."""
        self._proc = 0.0

    def answer(self, usage, model):
        """A main-conversation answer's usage. A subagent's never comes here."""
        if not isinstance(usage, dict):
            return
        try:
            used = sum(int(usage.get(k) or 0) for k in (
                "input_tokens", "cache_creation_input_tokens",
                "cache_read_input_tokens", "output_tokens"))
        except (TypeError, ValueError):
            return
        if used > 0:
            self._answer = (used, model)

    def result(self, total_cost, model_usage):
        """A turn ended. total_cost is the CLI's running total_cost_usd."""
        self.turns += 1
        self.turn_cost = None
        if isinstance(total_cost, (int, float)) and total_cost >= 0:
            delta = total_cost - self._proc
            if delta < 0:
                delta = total_cost   # the process started again
            self._proc = float(total_cost)
            self.total += delta
            self.turn_cost = delta
            self.cost_known = True
        if self._answer:
            self.used, model = self._answer
            self._answer = None
            self.window = self._window_for(model, model_usage)

    @staticmethod
    def _window_for(model, model_usage):
        if not model or not isinstance(model_usage, dict):
            return None
        entry = model_usage.get(model)
        if entry is None:
            entry = next((e for e in model_usage.values()
                          if isinstance(e, dict) and e.get("canonicalModel") == model), None)
        window = entry.get("contextWindow") if isinstance(entry, dict) else None
        return int(window) if isinstance(window, (int, float)) and window > 0 else None

    def percent(self):
        """Whole percent of the window in use, or None when either is unknown."""
        if self.used is None or not self.window:
            return None
        return max(0, min(100, self.used * 100 // self.window))

    def wire(self):
        """The payload of a v2 usage line, or None when there is nothing to say."""
        if self.used is None and not self.cost_known:
            return None
        return (f"{self.used if self.used is not None else '-'} "
                f"{self.window if self.window else '-'} "
                f"{f'{self.total:.4f}' if self.cost_known else '-'}")

    def to_record(self):
        return {"cost": round(self.total, 6) if self.cost_known else None,
                "turns": self.turns, "used": self.used, "window": self.window}

    @classmethod
    def from_record(cls, rec):
        m = cls()
        if not isinstance(rec, dict):
            return m

        def num(key, kind, low):
            v = rec.get(key)
            ok = isinstance(v, (int, float)) and not isinstance(v, bool) and v >= low
            return kind(v) if ok else None
        cost = num("cost", float, 0)
        if cost is not None:
            m.total, m.cost_known = cost, True
        m.turns = num("turns", int, 0) or 0
        m.used = num("used", int, 1)
        m.window = num("window", int, 1)
        return m


def _group(n):
    return f"{n:,}"


async def usage_report(session, meter):
    """The lines /usage prints. The one place that asks Claude Code for
    anything: a local request for its context breakdown, with the meter's
    own reading as the fallback."""
    info = None
    getter = getattr(session, "context_usage", None)
    if getter:
        try:
            info = await asyncio.wait_for(getter(), 5)
        except Exception:  # noqa: BLE001
            info = None
    lines = []
    used, window = meter.used, meter.window
    if isinstance(info, dict):
        total, raw = info.get("totalTokens"), info.get("rawMaxTokens") or info.get("maxTokens")
        if isinstance(total, int) and isinstance(raw, int) and raw > 0:
            used, window = total, raw
    if used is not None and window:
        lines.append(f"Context: {_group(used)} of {_group(window)} tokens "
                     f"({max(0, min(100, used * 100 // window))}%).")
    elif used is not None:
        lines.append(f"Context: {_group(used)} tokens.")
    else:
        lines.append("Context: no reading yet.")
    cats = info.get("categories") if isinstance(info, dict) else None
    if isinstance(cats, list):
        shown = [c for c in cats if isinstance(c, dict) and isinstance(c.get("tokens"), int)
                 and c["tokens"] > 0 and not any(w in str(c.get("name", "")).lower()
                                                  for w in ("free", "buffer"))]
        shown.sort(key=lambda c: -c["tokens"])
        for c in shown[:4]:
            lines.append(f"  {clip(str(c.get('name', '?')), 40)}: {_group(c['tokens'])}")
    if meter.cost_known:
        lines.append(f"Cost: ${meter.total:.4f} over {meter.turns} "
                     f"turn{'s' if meter.turns != 1 else ''}.")
    else:
        lines.append(f"Cost: unknown over {meter.turns} turn{'s' if meter.turns != 1 else ''}.")
    return lines


# Claude Code commands (PROTOCOL.md, "Commands and plan mode"). The bridge
# passes /compact, project and user commands and skills to Claude Code and
# refuses every other built-in: a typed line must never reach a command that
# changes permissions, directories, hooks, MCP servers or memory.
CMD_HINTS = {
    "permissions": "Permissions stay on the host.",
    "add-dir": "Directories stay on the host.",
    "hooks": "Hooks stay on the host.",
    "mcp": "MCP servers stay on the host.",
    "memory": "Memory files stay on the host.",
    "config": "Claude Code settings stay on the host.",
    "login": "It needs the terminal.", "logout": "It needs the terminal.",
    "resume": "It needs the terminal; Claude30 resumes a conversation by itself.",
    "model": "Use /model.", "effort": "Use /effort.", "usage": "Use /usage.",
    "agents": "Use /agents.", "context": "Use /usage.", "init": "Ask Claude to write CLAUDE.md.",
}


def command_files(cwd):
    """Names of the project's and the user's own commands (.claude/commands)."""
    names = set()
    for root in (os.path.join(cwd or ".", ".claude", "commands"),
                 os.path.expanduser("~/.claude/commands")):
        for dirpath, _dirs, files in os.walk(root):
            rel = os.path.relpath(dirpath, root)
            for f in files:
                if f.endswith(".md"):
                    stem = f[:-3]
                    names.add(stem if rel == "." else rel.replace(os.sep, ":") + ":" + stem)
    return names


def set_session_commands(session, names, skills, cwd, terminal=()):
    """Records which slash commands a session passes to Claude Code. The CLI
    lists every command, built-ins included, so the pass-through is an
    allowlist: /compact, the project's and user's command files, the CLI's
    skills (its init message names them) and plugin commands (`plugin:name`).
    Everything else the CLI lists is refused."""
    names = [str(n) for n in names or [] if n]
    session.known_commands = sorted(set(names) | set(terminal or ()))
    allowed = {"compact"} | {n for n in command_files(cwd) if n in names}
    allowed |= {n for n in names if ":" in n}
    allowed |= {str(n) for n in skills or [] if str(n) in names}
    allowed -= set(terminal or ())
    session.allowed_commands = allowed


def route_prompt(prompt, allowed, known):
    """What a typed line is. Returns (kind, text): "send" (a prompt, text
    is what to send), "command" (a Claude Code command, sent as it is),
    "clear", "commands", "plan" (text: the words after /plan) or "refuse"
    (text: the notice). Only a first word that starts with / is a command;
    /usr/bin is missing is a prompt."""
    words = prompt.split()
    if not words or not words[0].startswith("/") or len(words[0]) < 2:
        return "send", prompt
    name = words[0][1:]
    low = name.lower()
    if low == "clear":
        return "clear", ""
    if low == "commands":
        return "commands", ""
    if low == "plan":
        return "plan", " ".join(words[1:]).lower()
    if name in allowed:
        return "command", prompt
    if low in CMD_HINTS or name in known:
        hint = CMD_HINTS.get(low, "Claude30 does not pass it on.")
        return "refuse", f"/{name} is not available here. {hint}"
    return "send", " " + prompt     # text that merely starts with a slash


def commands_lines(session):
    own = "/new /clear /edits /agents /usage /model /effort /plan /commands /help /quit"
    lines = ["Bridge: " + own]
    names = sorted(getattr(session, "allowed_commands", ()))
    if not names:
        return lines + ["Claude Code: none listed yet."]
    cur = "Claude Code:"
    for n in names:
        if len(cur) + len(n) + 2 > 74:
            lines.append(cur)
            cur = "  "
        cur += f" /{n}"
    return lines + [cur]


class SessionConfig:
    """What one conversation runs with. Each v2 session has its own, so a
    /edits in one tab never changes another."""

    def __init__(self, cwd, accept_edits=False, model=None):
        self.cwd = cwd
        self.accept_edits = accept_edits
        self.model = model            # what the session runs with
        self.default_model = model    # what "bridge default" means
        self.effort = None            # None: the CLI decides
        # v2 only: the Claude conversation this session runs, and whether
        # to continue it (resume) or start it under that id.
        self.claude_id = None
        self.resume = False
        self.chat = False     # v2 only: a chat, not a project (see CHAT_PROMPT)
        self.partial = False  # v2 only: ask for partial messages, to stream answers
        self.always = set()   # "Tool" or "Bash:<exact command>"
        self.plan = False     # plan mode is on (kept across restarts of the CLI)
        self.turns = 0
        self.meter = UsageMeter()   # what the conversation used and cost


class ClaudeSession:
    def __init__(self, term, cfg):
        self.term = term      # a Terminal (tty, v1) or a Channel (v2)
        self.cfg = cfg
        self.client = None
        self.always = cfg.always
        self.next_pid = 1
        # The SDK runs each can_use_tool request as its own task, so two tool
        # calls can ask at once. Both prompts would read answers from the same
        # input queue and each would discard the other's answer, leaving the
        # turn stuck. Ask one at a time.
        self._perm_lock = asyncio.Lock()
        self.agents = AgentTracker(term)
        self.init_agents = []     # agent names from the CLI's init message
        self.known_commands = []  # every slash command the CLI lists
        self.allowed_commands = set()   # the ones passed through (see set_commands)
        # Text blocks of the message being streamed, by the API's block
        # index. Their deltas are only sent while the term's stream switch is
        # on (see BlockStream); the lock keeps a switch change from running
        # in the middle of a delta.
        self._blocks = {}
        self._slock = asyncio.Lock()

    def options(self):
        """What the SDK starts the Claude CLI with. A chat gets no tools but
        the web ones, no settings or MCP servers, and its own short prompt."""
        from claude_agent_sdk import ClaudeAgentOptions
        if self.cfg.chat:
            opts = ClaudeAgentOptions(
                cwd=self.cfg.cwd,
                permission_mode="default",
                tools=list(CHAT_TOOLS),
                can_use_tool=self._can_use_tool,
                system_prompt=CHAT_PROMPT.format(cols=self.term.cols,
                                                 style=style_sentence(self.term)),
                setting_sources=[],
                strict_mcp_config=True,
            )
        else:
            opts = ClaudeAgentOptions(
                cwd=self.cfg.cwd,
                permission_mode=("plan" if self.cfg.plan else
                                 "acceptEdits" if self.cfg.accept_edits else "default"),
                allowed_tools=["Read", "Glob", "Grep", "TodoWrite"],
                can_use_tool=self._can_use_tool,
                system_prompt={"type": "preset", "preset": "claude_code",
                               "append": SYSTEM_APPEND.format(
                                   cols=self.term.cols, style=style_sentence(self.term))},
                setting_sources=["user", "project", "local"],
            )
        if self.cfg.model:
            opts.model = self.cfg.model
        if self.cfg.effort:
            opts.effort = self.cfg.effort
        if self.cfg.partial:
            opts.include_partial_messages = True
        if self.cfg.claude_id:
            if self.cfg.resume:
                opts.resume = self.cfg.claude_id
            else:
                opts.session_id = self.cfg.claude_id
        return opts

    async def start(self):
        from claude_agent_sdk import ClaudeSDKClient
        opts = self.options()
        self.cfg.meter.new_process()
        self.client = ClaudeSDKClient(options=opts)
        await self.client.connect()
        try:
            info = await self.client.get_server_info() or {}
            self.set_commands([c.get("name") for c in info.get("commands") or []
                               if isinstance(c, dict)], None, self.cfg.cwd)
        except Exception:  # noqa: BLE001
            pass   # the init message of the first turn fills the list
        self._msgs = asyncio.Queue()
        self._in_turn = False
        self._pump = asyncio.ensure_future(self._pump_messages())
        self.term.on_interrupt = self.interrupt

    async def stop(self):
        pump, self._pump = getattr(self, "_pump", None), None
        if pump:
            pump.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await pump
        if self.client:
            client, self.client = self.client, None
            await client.disconnect()

    async def interrupt(self):
        self.agents.interrupted = True
        if self.client:
            await self.client.interrupt()

    async def set_model(self, model):
        """Switches the model of the running conversation (None: default)."""
        if self.client:
            await self.client.set_model(model)

    async def set_permission_mode(self, mode):
        """Switches the running conversation's permission mode (plan, ...)."""
        if self.client:
            await self.client.set_permission_mode(mode)

    def set_commands(self, names, skills, cwd, terminal=()):
        set_session_commands(self, names, skills, cwd, terminal)

    async def context_usage(self):
        """Claude Code's own context breakdown (a local request to the CLI)."""
        if not self.client:
            return None
        return await self.client.get_context_usage()

    async def _can_use_tool(self, tool_name, input_data, context):
        from claude_agent_sdk.types import PermissionResultAllow, PermissionResultDeny
        key = f"Bash:{input_data.get('command', '')}" if tool_name == "Bash" else tool_name
        async with self._perm_lock:
            # Checked inside the lock: an "always" given to the request ahead
            # of this one may already cover it.
            if key in self.always:
                return PermissionResultAllow(updated_input=input_data)
            summary, detail = describe_tool(tool_name, input_data, self.cfg.cwd)
            label = self.agents.label_for(getattr(context, "agent_id", None))
            if label:
                summary = f"({label}) {summary}"
            pid = self.next_pid
            self.next_pid += 1
            ans = await self.term.ask_permission(pid, tool_name, summary, detail)
            if ans == "a" and tool_name != "ExitPlanMode":
                self.always.add(key)
        if ans in ("y", "a") and tool_name == "ExitPlanMode" and self.cfg.plan:
            # Approving the plan leaves plan mode inside this turn, so Claude
            # can carry it out now.
            from claude_agent_sdk.types import PermissionUpdate
            mode = "acceptEdits" if self.cfg.accept_edits else "default"
            self.cfg.plan = False
            hook = getattr(self.term, "plan_exited", None)
            if hook:
                await hook()
            return PermissionResultAllow(updated_input=input_data, updated_permissions=[
                PermissionUpdate(type="setMode", mode=mode, destination="session")])
        if ans in ("y", "a"):
            return PermissionResultAllow(updated_input=input_data)
        return PermissionResultDeny(message="The user denied this action.", interrupt=False)

    async def _pump_messages(self):
        """Reads everything Claude sends, in turns and between them: a
        background subagent ends between turns, and Claude then starts a
        turn of its own to report it."""
        try:
            async for msg in self.client.receive_messages():
                self._msgs.put_nowait(msg)
                if not self._in_turn:
                    self._wake()
        except Exception as e:  # noqa: BLE001
            self._msgs.put_nowait(e)
        else:
            self._msgs.put_nowait(EOFError("Claude stopped"))

    def _wake(self):
        """Tells a v2 channel that output arrived between turns. tty and v1
        show it before the next turn instead."""
        wake = getattr(self.term, "wake", None)
        if wake:
            wake()

    async def _next(self):
        msg = await self._msgs.get()
        if isinstance(msg, BaseException):
            raise msg
        return msg

    async def turn(self, prompt):
        ag = self.agents
        if not self._msgs.empty():
            await self.follow()
        ag.interrupted = False
        ag.ended.clear()
        self._in_turn = True
        try:
            await self.client.query(prompt)
            while not await self._handle(await self._next()):
                pass
        finally:
            await ag.close_all()
            self._in_turn = False
            if not self._msgs.empty():
                self._wake()

    async def follow(self):
        """Shows what arrived between turns. Background subagents' progress
        and ends are applied as they come; when Claude starts a turn by
        itself, it runs to its result. Returns whether there was a turn."""
        from claude_agent_sdk import AssistantMessage, ResultMessage, StreamEvent, UserMessage
        ag = self.agents
        turned = False
        self._in_turn = True
        try:
            while turned or not self._msgs.empty():
                msg = await self._next()
                if not turned and (isinstance(msg, ResultMessage) or (
                        isinstance(msg, (AssistantMessage, UserMessage, StreamEvent))
                        and not getattr(msg, "parent_tool_use_id", None))):
                    turned = True
                    ag.interrupted = False
                    ag.ended.clear()
                if await self._handle(msg):
                    break
        finally:
            if turned:
                await ag.close_all()
            self._in_turn = False
            if not self._msgs.empty():
                self._wake()
        return turned

    async def _handle(self, msg):
        """One message from Claude. Returns True at the end of a turn."""
        from claude_agent_sdk import (AssistantMessage, ResultMessage, StreamEvent,
                                      SystemMessage, TextBlock, ToolUseBlock, UserMessage)
        try:
            from claude_agent_sdk import ToolResultBlock
        except ImportError:
            ToolResultBlock = None
        try:
            from claude_agent_sdk import ConversationResetMessage
        except ImportError:
            ConversationResetMessage = None
        ag = self.agents
        if isinstance(msg, StreamEvent):
            if not getattr(msg, "parent_tool_use_id", None):
                await self._stream_event(msg.event)
        elif isinstance(msg, AssistantMessage):
            parent = getattr(msg, "parent_tool_use_id", None)
            if not parent:
                self.cfg.meter.answer(getattr(msg, "usage", None), getattr(msg, "model", None))
            for block in msg.content:
                if parent:
                    # A subagent's own work: counted, never sent.
                    if isinstance(block, ToolUseBlock):
                        s, _ = describe_tool(block.name, block.input or {}, self.cfg.cwd)
                        await ag.step(parent, block.name, s)
                elif isinstance(block, TextBlock) and block.text.strip():
                    await self._text_block(block.text)
                elif isinstance(block, ToolUseBlock):
                    if block.name in ("Agent", "Task"):
                        await ag.start(block.id, block.input or {})
                    else:
                        s, _ = describe_tool(block.name, block.input or {}, self.cfg.cwd)
                        await self.term.tool(block.name, s)
        elif isinstance(msg, UserMessage) and ToolResultBlock:
            if getattr(msg, "parent_tool_use_id", None):
                return False
            content = msg.content if isinstance(msg.content, list) else []
            for block in content:
                if not isinstance(block, ToolResultBlock):
                    continue
                if ag.owns(block.tool_use_id):
                    await ag.end_result(block.tool_use_id, block.content, bool(block.is_error))
                elif block.is_error:
                    first = first_line(block.content) or "error"
                    await self.term.tool_error(first)
        elif ConversationResetMessage and isinstance(msg, ConversationResetMessage):
            self.cfg.meter.new_process()   # the CLI's running totals start again
        elif isinstance(msg, SystemMessage):
            await self._system(msg)
        elif isinstance(msg, ResultMessage):
            await self._end_blocks()
            await ag.close_all()
            if getattr(msg, "session_id", None):
                self.cfg.claude_id = msg.session_id
            reason = getattr(msg, "terminal_reason", None) or ""
            status = "interrupted" if reason.startswith("aborted") else msg.subtype
            meter = self.cfg.meter
            meter.result(getattr(msg, "total_cost_usd", None), getattr(msg, "model_usage", None))
            await self.term.result(status, meter.turn_cost, meter)
            await self.term.usage(meter)
            return True
        return False

    # ---- streamed text -------------------------------------------------

    async def _stream_event(self, ev):
        """One raw API stream event of the main conversation. Only text
        blocks matter; the AssistantMessage that follows each carries the
        same text whole, and decides (in _text_block) whether it was sent."""
        kind = ev.get("type")
        async with self._slock:
            if kind == "message_start":
                await self._end_blocks_locked()
            elif kind == "content_block_start":
                if (ev.get("content_block") or {}).get("type") == "text":
                    self._blocks[ev.get("index")] = BlockStream(self.term)
            elif kind == "content_block_delta":
                b = self._blocks.get(ev.get("index"))
                d = ev.get("delta") or {}
                if b and d.get("type") == "text_delta":
                    await b.delta(d.get("text") or "")
            elif kind == "content_block_stop":
                b = self._blocks.get(ev.get("index"))
                if b:
                    await b.flush()

    async def _text_block(self, text):
        """A text block of the main conversation, whole. Prints it unless
        its deltas already went out."""
        async with self._slock:
            for b in self._blocks.values():
                if not b.claimed and b.text == text:
                    return await b.end(text)
        await self.term.text(text)

    async def _end_blocks(self):
        async with self._slock:
            await self._end_blocks_locked()

    async def _end_blocks_locked(self):
        """The message or turn ended: send what a streamed block still holds
        (an interrupt can end a turn before its blocks end)."""
        blocks, self._blocks = list(self._blocks.values()), {}
        for b in blocks:
            await b.flush()

    async def stream_changed(self):
        """The tab's stream switch changed: a block being written starts
        streaming at once when it was switched on."""
        async with self._slock:
            for b in self._blocks.values():
                await b.switched()

    async def _system(self, msg):
        data = msg.data if isinstance(msg.data, dict) else {}
        if msg.subtype == "init":
            names = data.get("agents")
            if isinstance(names, list):
                self.init_agents = [str(n) for n in names]
            if isinstance(data.get("slash_commands"), list):
                self.set_commands(data["slash_commands"], data.get("skills"), self.cfg.cwd,
                                  data.get("terminal_slash_commands") or ())
            return
        if msg.subtype == "compact_boundary":
            post = (data.get("compact_metadata") or {}).get("post_tokens")
            if isinstance(post, int) and post > 0:
                self.cfg.meter.answer({"input_tokens": post}, None)
            await self.term.notice("Conversation compacted.")
            await self.term.usage(self.cfg.meter)
            return
        tid = data.get("tool_use_id")
        if not tid:
            return
        ag = self.agents
        if msg.subtype == "task_started":
            ag.bind(tid, data.get("task_id"))
        elif msg.subtype == "task_progress":
            usage = data.get("usage") or {}
            await ag.step(tid, steps=usage.get("tool_uses"))
        elif msg.subtype == "task_notification":
            status = {"completed": "done", "failed": "failed", "stopped": "stopped"}.get(
                data.get("status"), "done")
            await ag.end(tid, status, " ".join(str(data.get("summary") or "").split()))


# What the demo's "streaming" prompt sends (a few paragraphs and a code block).
LONG_DEMO = (
    "The SE/30 has a 512x342 pixel display and a 68030 running at 16 MHz, which is a lot of "
    "machine for a console that only has to show text. Claude writes its answer a few words at "
    "a time, and with streaming on those words appear here while it is still being written, so "
    "there is something to read long before the whole answer is done. With streaming off, the "
    "same paragraph arrives in one piece when Claude has finished writing it.\n\n"
    "A second paragraph follows, a little shorter, so that a pause between paragraphs shows "
    "too: every blank line is part of the text and is kept.\n"
    "```python\ndef parse(s):\n    return [t for t in s.split() if t]\n```\n"
    "`parse()` splits the text on white space and drops the empty pieces, which is all the "
    "parser needs for now, and this last paragraph starts with inline code to check that it "
    "streams like any other text.")


# An answer with every element the app styles (prompt word "styles").
STYLES_DEMO = (
    "# Styles demo\n"
    "## A second level heading\n"
    "### A third level heading\n"
    "\n"
    "Text with **bold**, *italic*, ***bold italic***, __bold too__, _italic too_ and `inline code` "
    "side by side, a [link to the docs](https://example.com/docs) and https://example.com/bare "
    "on a line, and snake_case_name, 2 * 3 and ~~strike~~ shown as typed. Escaped: \\*star\\*.\n"
    "*Note: this asterisk never closes\n"
    "\n"
    "- first item with **bold**\n"
    "  - nested item with `code`\n"
    "- second item\n"
    "1. numbered one\n"
    "2. numbered two\n"
    "\n"
    "> a quoted *line*\n"
    "\n"
    "---\n"
    "\n"
    "| name | value |\n"
    "|------|:-----:|\n"
    "| a    | 1     |\n"
    "\n"
    "```python\ndef __init__(self, **kwargs):\n    pass\n```\n"
    "That is all of it.")


class DemoSession:
    """Fake Claude, for testing the serial link and the client."""

    def __init__(self, term, cfg):
        self.term = term
        self.cfg = cfg
        self._stop = False
        self._child = None
        self.init_agents = ["Explore", "general-purpose", "Plan"]
        self.known_commands = ["clear", "compact", "config", "hello", "permissions"]
        self.allowed_commands = {"compact", "hello"}
        self.agents = AgentTracker(term)
        self._bg = None           # the background subagent's timer
        self._bg_done = False     # ... which ended between turns
        self._block = None        # the text block a "long" turn is streaming
        self._proc_total = 0.0    # the made-up CLI's running cost
        self._full = False        # the prompt has the word "full": context near 85%

    async def _result(self, status="success"):
        """Ends a turn with made-up usage that goes through the real meter:
        each turn adds 0.0123 and 4000 tokens of context, and a prompt with
        the word "full" jumps the context to 85% of a 200,000-token window."""
        m = self.cfg.meter
        if status == "success":
            used = 170000 if self._full else min((m.used or 8000) + 4000, 199000)
            m.answer({"input_tokens": used}, "demo")
            self._proc_total += 0.0123
            m.result(self._proc_total, {"demo": {"contextWindow": 200000}})
        else:
            m.result(self._proc_total, None)
        await self.term.result(status, m.turn_cost, m)
        await self.term.usage(m)

    async def context_usage(self):
        m = self.cfg.meter
        if m.used is None:
            return None   # nothing to report before the first turn
        return {"totalTokens": m.used, "rawMaxTokens": m.window or 200000,
                "categories": [{"name": "Messages", "tokens": max(m.used - 12000, 0)},
                               {"name": "System prompt", "tokens": 8000},
                               {"name": "Tools", "tokens": 3000},
                               {"name": "Memory files", "tokens": 1000},
                               {"name": "Free space", "tokens": 100000}]}

    async def start(self):
        # Test hook: a slow start with a child process that, like the real
        # CLI, reads its stdin until the bridge goes away.
        delay = float(os.environ.get("SE30_DEMO_START_DELAY", "0") or 0)
        if delay:
            self._child = await asyncio.create_subprocess_exec(
                "cat", stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.DEVNULL)
            await asyncio.sleep(delay)
        self.cfg.meter.new_process()
        self.term.on_interrupt = self.interrupt

    async def stop(self):
        if self._bg:
            self._bg.cancel()
            self._bg = None
        if self._child and self._child.returncode is None:
            self._child.kill()
            await self._child.wait()
        self._child = None

    async def interrupt(self):
        self._stop = True

    async def _background_turn(self):
        """A fake background subagent that ends about 1.5 s after the turn."""
        t = self.term
        ag = self.agents
        await t.text("Starting a subagent in the background.")
        await ag.start("b1", {"subagent_type": "Explore", "description": "Survey the tests",
                              "run_in_background": True})
        await ag.end_result("b1", "Async agent launched successfully.", False)
        await t.text("It runs on; ask me anything meanwhile.")
        await self._result()
        self._bg = asyncio.ensure_future(self._background_runs())

    async def _background_runs(self):
        await asyncio.sleep(1.5)
        for name in ("Glob", "Grep", "Read"):
            await self.agents.step("b1", name, "tests")
        self._bg_done = True
        wake = getattr(self.term, "wake", None)
        if wake:
            wake()

    async def follow(self):
        """Like Claude after a background subagent ends: its end, then a turn."""
        if not self._bg_done:
            return False
        self._bg_done = False
        await self.agents.end("b1", "done", "Found 12 test files")
        await self.term.text("The background subagent is done: there are 12 test files.")
        await self._result()
        return True

    async def _agents_turn(self):
        """Two overlapping fake subagents, one of which asks permission."""
        t = self.term
        ag = AgentTracker(t)
        ag.STEP_EVERY = 0.2
        await t.text("Starting two subagents.")
        await ag.start("d1", {"subagent_type": "Explore", "description": "Find where the parser is tested"})
        await ag.start("d2", {"subagent_type": "general-purpose", "description": "Check the docs for the parser"})
        ag.bind("d2", "demo-2")
        script = [("d1", "Grep", "def parse"), ("d2", "Read", "docs/parser.md"),
                  ("d1", "Read", "tests/test_parser.py"), ("d2", "Glob", "docs/*.md"),
                  ("d1", "Grep", "parse(")]
        for tid, name, summ in script[:3]:
            if self._stop:
                break
            await asyncio.sleep(0.3)
            await ag.step(tid, name, summ)
        if not self._stop:
            summary, detail = describe_tool("Bash", {"command": "git log -5"}, self.cfg.cwd)
            label = ag.label_for("demo-2")
            ans = await t.ask_permission(1, "Bash", f"({label}) {summary}", detail)
            await t.tool("Bash", f"git log -5 ({'allowed' if ans in 'ya' else 'denied'})")
        for tid, name, summ in script[3:]:
            if self._stop:
                break
            await asyncio.sleep(0.3)
            await ag.step(tid, name, summ)
        if self._stop:
            await ag.close_all()
            await self._result("interrupted")
            return
        await ag.end("d2", "done", "The parser is documented in docs/parser.md")
        await ag.end("d1", "done", "Tests are in tests/test_parser.py")
        await t.text("Both subagents are done.")
        await self._result()


    async def _chat_turn(self, prompt):
        """A chat: a short answer, and a web search when asked to look
        something up, which needs a permission like the real thing."""
        t = self.term
        turn = f"turn {self.cfg.turns}, " if self.cfg.turns else ""
        picked = f"model {self.cfg.model}, " if self.cfg.model != self.cfg.default_model else ""
        await t.text(f"You said: “{prompt}” — demo chat, {picked}{turn}no tokens spent.")
        if "search" in prompt.lower().split():
            summary, detail = describe_tool("WebSearch", {"query": prompt}, self.cfg.cwd)
            if "WebSearch" in self.cfg.always:
                ans = "y"
            else:
                ans = await t.ask_permission(1, "WebSearch", summary, detail)
                if ans == "a":
                    self.cfg.always.add("WebSearch")
            await t.tool("WebSearch", summary + (" (allowed)" if ans in "ya" else " (denied)"))
        await self._result()

    async def set_model(self, model):
        pass   # the demo reads cfg.model each turn

    async def stream_changed(self):
        if self._block:
            await self._block.switched()

    async def _long_turn(self, prompt, text=None):
        """One long text block (the prompt has the word "streaming", or
        "styles" for STYLES_DEMO), fed in small slices so that streaming shows.
        With the word "tool" a tool line comes in the middle of it."""
        t = self.term
        text = text or LONG_DEMO
        delay = float(os.environ.get("SE30_DEMO_STREAM_DELAY", "0.005") or 0)
        self._block = block = BlockStream(t)
        try:
            for n, i in enumerate(range(0, len(text), 12)):
                if self._stop:
                    await block.flush()
                    return await self._result("interrupted")
                await asyncio.sleep(delay)
                await block.delta(text[i:i + 12])
                if n == 30 and "tool" in prompt.lower().split():
                    await t.tool("Read", "src/parser.py")
            await block.end(text)
        finally:
            self._block = None
        await self._result()

    async def set_permission_mode(self, mode):
        pass   # the demo reads cfg.plan each turn

    async def _plan_turn(self, prompt):
        """Plan mode: a plan, then the request to leave plan mode."""
        t = self.term
        await t.text(f"Plan for “{prompt}”: 1. read src/parser.py 2. add a test 3. fix parse().")
        summary, detail = describe_tool("ExitPlanMode", {
            "plan": "1. Read src/parser.py\n2. Add a test\n3. Fix parse()"}, self.cfg.cwd)
        ans = await t.ask_permission(1, "ExitPlanMode", summary, detail)
        if ans in ("y", "a"):
            self.cfg.plan = False
            hook = getattr(t, "plan_exited", None)
            if hook:
                await hook()
            await t.text("Plan approved. Carrying it out.")
        else:
            await t.text("Staying in plan mode.")
        await self._result()

    async def turn(self, prompt):
        self._stop = False
        t = self.term
        words = prompt.split()
        if words[:1] == ["/compact"]:
            m = self.cfg.meter
            m.answer({"input_tokens": max((m.used or 8000) // 2, 2000)}, "demo")
            await t.notice("Conversation compacted.")
            await self._result()
            return
        if words[:1] == ["/hello"]:
            await t.text("HELLO (a project command).")
            await self._result()
            return
        if self.cfg.plan and not self.cfg.chat:
            return await self._plan_turn(prompt)
        self._full = "full" in prompt.lower().split()
        if self._bg_done:
            await self.follow()
        if "streaming" in prompt.lower().split():
            return await self._long_turn(prompt)
        if "styles" in prompt.lower().split():
            return await self._long_turn(prompt, STYLES_DEMO)
        if "agents" in prompt.lower().split():
            return await self._agents_turn()
        if "background" in prompt.lower().split():
            return await self._background_turn()
        if self.cfg.chat:
            return await self._chat_turn(prompt)
        where = os.path.basename(self.cfg.cwd.rstrip("/")) or self.cfg.cwd
        edits = "auto" if self.cfg.accept_edits else "ask"
        turn = f"turn {self.cfg.turns}, " if self.cfg.turns else ""
        picked = ""
        if self.cfg.model != self.cfg.default_model:
            picked += f"model {self.cfg.model}, "
        if self.cfg.effort:
            picked += f"effort {self.cfg.effort}, "
        await t.text(f"You said: “{prompt}” — demo mode in {where}, edits {edits}, "
                     f"{picked}{turn}no tokens spent.")
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
                await self._result("interrupted")
                return
            await t.text(f"Pager test line {i} of 30.")
        summary, detail = describe_tool("Edit", {
            "file_path": os.path.join(self.cfg.cwd, "src/parser.py"),
            "old_string": "def parse(s):\n    return s.split()\n",
            "new_string": "def parse(s):\n    return [t for t in s.split() if t]\n",
        }, self.cfg.cwd)
        if "Edit" in self.cfg.always:
            ans = "y"
        else:
            ans = await t.ask_permission(1, "Edit", summary, detail)
            if ans == "a":
                self.cfg.always.add("Edit")
        await t.tool("Edit", summary + (" (allowed)" if ans in "ya" else " (denied)"))
        await self._result()


def make_session(args, out, cfg):
    return (DemoSession if args.demo else ClaudeSession)(out, cfg)


EFFORTS = ("low", "medium", "high", "xhigh", "max")
MODEL_NAME = re.compile(r"^[A-Za-z0-9._\[\]-]{1,31}$")
MAX_MODELS = 16


def check_settings(args, model, effort):
    """Checks a model and effort from the wire or a command ("-" or None
    is the bridge default). Returns (model, effort, problems): a value
    that is not offered falls back to the default and is reported."""
    problems = []
    if model in (None, "-", "default"):
        model = None
    elif model not in args.models:
        problems.append(f"Unknown model {clip(model, 40)}; using the bridge default.")
        model = None
    if effort in (None, "-", "default"):
        effort = None
    elif effort not in EFFORTS:
        problems.append(f"Unknown effort {clip(effort, 40)}; using the default.")
        effort = None
    return model, effort, problems


def setting_command(args, words, model, effort):
    """Parses /model or /effort (words, lower case) against the tab's
    current picks. Returns (model, effort, lines, changed): the new picks,
    what to tell the person, and whether to change anything."""
    kind = words[0]
    if kind == "/model":
        what, choices, cur = "Model", list(args.models), model
        default = args.model or "the CLI default"
    else:
        what, choices, cur = "Effort", list(EFFORTS), effort
        default = "the CLI default"
    if len(words) == 1:
        shown = cur or f"default ({default})"
        return model, effort, [f"{what}: {shown}.",
                               f"Choices: {' '.join(choices)} or default."], False
    val = words[1]
    new_model, new_effort = (val, effort) if kind == "/model" else (model, val)
    m, e, problems = check_settings(args, new_model, new_effort)
    if problems:
        return model, effort, [problems[0].split(";")[0] + ".",
                               f"Choices: {' '.join(choices)} or default."], False
    return m, e, [], True


def wire_settings(model, effort):
    return f"{model or '-'} {effort or '-'}"


HELP = """Commands:
  /new         start a fresh conversation
  /edits on    auto-accept file edits (off asks each time)
  /agents      list the subagents this project offers
  /clear       same as /new
  /plan on     plan mode: Claude plans and asks before it edits (off to leave)
  /commands    the commands Claude Code offers here (/compact and your own)
  /usage       context used and what this conversation cost
  /model NAME  the model for the next turn (alone: show choices)
  /effort LVL  low, medium, high, xhigh or max (alone: show choices)
  /help        this text
  /quit        disconnect
Type a prompt and press return. End a line with \\ to continue it.
While Claude works: ctrl-C stops it. At --More--: space, return or q."""


CHAT_HELP = """Commands:
  /new         start a fresh conversation
  /usage       context used and what this conversation cost
  /model NAME  the model for the next turn (alone: show choices)
  /effort LVL  low, medium, high, xhigh or max (alone: show choices)
  /help        this text
  /quit        close this chat
This is a chat: Claude can search the web but has no files or tools.
A chat is deleted when you close its tab or quit."""


# --------------------------------------------------------------------------
# Projects
# --------------------------------------------------------------------------

def log(msg):
    print(f"se30bridge: {msg}", file=sys.stderr, flush=True)


# --------------------------------------------------------------------------
# One call at a time
# --------------------------------------------------------------------------

def _wait_for_lock(fd, seconds):
    end = time.monotonic() + seconds
    while True:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except OSError as e:
            if e.errno not in (errno.EAGAIN, errno.EACCES):
                raise
        if time.monotonic() >= end:
            return False
        time.sleep(0.05)


def take_call_lock(state_dir):
    """The WiFi232 holds one call at a time, so a new call means the previous
    one is dead, even if its bridge never noticed (a Wi-Fi drop leaves the TCP
    connection half open). Takes the lock in the state folder, ending the
    bridge that holds it: SIGTERM first, so it stops its sessions, then
    SIGKILL. Only the holder of the lock is ever signalled, so a stale pid in
    the file is never touched. Returns the open lock file, kept for the life
    of the process."""
    os.makedirs(state_dir, mode=0o700, exist_ok=True)
    fd = os.open(os.path.join(state_dir, "call.lock"), os.O_RDWR | os.O_CREAT, 0o600)
    # The old bridge may wait 5 s for its turn to stop and 5 s more while the
    # CLI writes out the conversation.
    wait = float(os.environ.get("SE30_EVICT_WAIT", "12") or 12)
    if not _wait_for_lock(fd, 0):
        pid = 0
        with contextlib.suppress(ValueError, OSError):
            pid = int(os.pread(fd, 32, 0).decode().strip() or 0)
        log(f"ending the previous call (bridge pid {pid or 'unknown'})")
        for sig, seconds in ((signal.SIGTERM, wait), (signal.SIGKILL, 5)):
            if pid > 0 and pid != os.getpid():
                with contextlib.suppress(ProcessLookupError, PermissionError):
                    os.kill(pid, sig)
            if _wait_for_lock(fd, seconds):
                break
        else:
            log("the previous bridge would not stop; giving up on this call")
            raise SystemExit(1)
    os.ftruncate(fd, 0)
    os.pwrite(fd, str(os.getpid()).encode(), 0)
    return fd


def project_name_ok(name):
    if not name or len(name.encode("utf-8")) > 120:
        return False
    if any(ord(c) < 32 or ord(c) == 127 for c in name):
        return False
    try:
        name.encode("mac_roman")
    except UnicodeEncodeError:
        return False
    return True


def list_projects(args):
    """[(name, path)] the client may open: the --cwd folder when it was given,
    then the non-hidden subfolders of each --projects root, or the current
    folder alone when neither option was given. A name seen before wins."""
    found = []
    if args.cwd:
        found.append(args.cwd)
    for root in args.projects or []:
        try:
            names = sorted(os.listdir(root), key=str.casefold)
        except OSError as e:
            log(f"can't list --projects {root}: {e}")
            continue
        for name in names:
            path = os.path.join(root, name)
            if not name.startswith(".") and os.path.isdir(path):
                found.append(path)
    if not args.cwd and not args.projects:
        found.append(os.getcwd())
    out, seen = [], set()
    for path in found:
        name = os.path.basename(path.rstrip("/")) or path
        if name == CHAT_NAME:
            log(f"skipping project {path}: {CHAT_NAME!r} is the name a chat is opened with")
            continue
        if not project_name_ok(name):
            log(f"skipping project {path!r}: its name can't be sent in Mac Roman")
            continue
        if name in seen:
            log(f"skipping project {path}: another project is already called {name!r}")
            continue
        seen.add(name)
        out.append((name, path))
    return sorted(out, key=lambda p: p[0].casefold())


# --------------------------------------------------------------------------
# Files: browse, read and save inside a session's project folder
# --------------------------------------------------------------------------

HIDE_PATTERNS = (".git", ".ssh", ".env*", "*.pem", "*.key", "id_rsa*", "id_ed25519*")
EDIT_MAX_BYTES = 28672        # Mac Roman bytes, CR line breaks
EDIT_MAX_LINES = 2000
READ_MAX_LINES = 32000
SAVE_MAX_BYTES = 32000        # the client's TextEdit holds at most this
PIECE_MAX_LINES = 200
PIECE_MAX_BYTES = 4096
LIST_MAX = 500
RO_LINE_MAX = 2000
SAVE_IDLE_SECS = 120
CONFLICT_IDLE_SECS = 3600     # a save waiting on the Replace/Cancel alert
READ_CAP_BYTES = 8 << 20      # bigger files show only their start, read-only
_CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
_CONTROL_BYTES = re.compile(rb"[\x00-\x08\x0a-\x1f\x7f]")


class Refused(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def hidden_name(name):
    # Case-blind: on a macOS host ".ENV" opens the same file as ".env".
    name = name.casefold()
    return any(fnmatch.fnmatchcase(name, p) for p in HIDE_PATTERNS)


def bad_name(name):
    return any(ord(c) < 32 or ord(c) == 127 for c in name)


def inside(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def crc_hex(data):
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


def file_token(data):
    return hashlib.sha1(data).hexdigest()[:12]


class FileText:
    """One file as the client sees it: lines, mode, line ending."""

    def __init__(self, data, files_mode, cut=False):
        """cut: data is only the start of a bigger file."""
        self.token = file_token(data)
        if b"\0" in data[:8192]:
            raise Refused("binary")
        reason = None
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            text = data.decode("latin-1")
            reason = "encoding"
        crlf = text.count("\r\n")
        kinds = [k for k, n in (("\r\n", crlf), ("\n", text.count("\n") - crlf),
                                ("\r", text.count("\r") - crlf)) if n]
        self.eol = kinds[0] if len(kinds) == 1 else "\n"
        if len(kinds) > 1:
            reason = reason or "endings"
        self.final = bool(text) and text[-1] in "\r\n"
        lines = re.split(r"\r\n|\r|\n", text) if text else []
        if self.final:
            lines.pop()
        if cut:
            if lines:
                lines.pop()       # the last line may end mid-way
            reason = reason or "long"
        if len(lines) > READ_MAX_LINES:
            lines = lines[:READ_MAX_LINES]
            reason = "long"
        exact = None
        if reason is None:
            try:
                exact = [l.encode("mac_roman") for l in lines]
            except UnicodeEncodeError:
                reason = "chars"
        if reason is None and any(_CONTROL.search(l) for l in lines):
            reason = "control"
        if reason is None:
            size = sum(len(b) for b in exact) + max(len(lines) - 1, 0)
            if size > EDIT_MAX_BYTES or len(lines) > EDIT_MAX_LINES:
                reason = "big"
        if reason is None and files_mode != "rw":
            reason = "ro"
        self.mode = "e" if reason is None else f"r:{reason}"
        self._exact = exact if reason in (None, "ro") else None
        self._text = lines
        self._shown = {}

    def __len__(self):
        return len(self._text)

    def line(self, i):
        """Line i as the Mac Roman bytes the client is sent."""
        if self._exact is not None:
            return self._exact[i]
        b = self._shown.get(i)
        if b is None:
            b = _CONTROL_BYTES.sub(b"?", transliterate(self._text[i], "macroman"))
            if len(b) > RO_LINE_MAX:
                b = b[:RO_LINE_MAX] + b"\xc9"
            self._shown[i] = b
        return b


class Save:
    def __init__(self, rel, token):
        self.rel = rel
        self.token = token
        self.blocks = {}
        self.touched = time.monotonic()
        self.conflict = False


async def git_ignored(root, paths):
    """The subset of paths (relative to root, folders ending in /) that git's
    ignore rules exclude, or None when git could not answer (outside a
    repository, git missing, or an error)."""
    if not paths:
        return set()
    try:
        proc = await asyncio.create_subprocess_exec(
            "git", "check-ignore", "--stdin", "-z", cwd=root,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.DEVNULL)
        out, _ = await proc.communicate(
            b"".join(p.encode("utf-8", "surrogateescape") + b"\0" for p in paths))
    except OSError:
        return None
    if proc.returncode not in (0, 1):
        return None
    return {p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p}


class FileService:
    """File requests for one v2 session, confined to its project folder.
    Every path goes through resolve() before anything reads or writes it."""

    def __init__(self, ch, root, mode):
        self.ch = ch
        self.root = root
        self.real_root = os.path.realpath(root)
        self.mode = mode
        self.queue = asyncio.Queue()
        self.task = None
        self.ignored = {}        # real relative path -> bool, refreshed by listings
        self.in_repo = None      # is the project inside a git work tree
        self.cache = {}          # relative path -> ((mtime, size), FileText)
        self.saves = {}          # transfer number -> Save
        self.done = {}           # transfer number -> final answer of a commit
        self.block = None        # [x, seq, lines] being received

    def start(self):
        self.task = asyncio.ensure_future(self._run())

    async def stop(self):
        if self.task:
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self.task
            self.task = None

    async def _run(self):
        while True:
            tag, payload = await self.queue.get()
            try:
                await self.handle(tag, payload)
            except EOFError:
                return
            except Exception as e:  # noqa: BLE001
                log(f"file request {tag} failed: {e!r}")

    # ---- the guard -------------------------------------------------------

    def real_parts(self, real):
        """The components of a resolved path below the project folder."""
        if real == self.real_root:
            return []
        return os.path.relpath(real, self.real_root).split(os.sep)

    async def ignored_set(self, paths):
        """git_ignored for paths relative to the real project folder. When
        git fails inside a repository, every path counts as ignored (and is
        not cached): a broken check must not show secrets."""
        hits = await git_ignored(self.real_root, paths)
        if hits is not None:
            return hits, True
        if self.in_repo is None:
            try:
                proc = await asyncio.create_subprocess_exec(
                    "git", "rev-parse", "--is-inside-work-tree", cwd=self.real_root,
                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
                out, _ = await proc.communicate()
                self.in_repo = proc.returncode == 0 and out.strip() == b"true"
            except OSError:
                self.in_repo = False
        if self.in_repo:
            return set(paths), False
        return set(), True

    async def _is_ignored(self, parts):
        """parts are the real path's components below the project folder."""
        prefixes = []
        for i in range(1, len(parts) + 1):
            p = "/".join(parts[:i]) + ("/" if i < len(parts) else "")
            prefixes.append(p)
        unknown = [p for p in prefixes if p not in self.ignored]
        if unknown:
            # The last part may be a folder: ask both ways.
            asked = unknown + ([unknown[-1] + "/"] if unknown[-1] == prefixes[-1] else [])
            hits, ok = await self.ignored_set(asked)
            if not ok:
                return True
            for p in unknown:
                self.ignored[p] = p in hits or (p == prefixes[-1] and p + "/" in hits)
        return any(self.ignored[p] for p in prefixes)

    async def resolve(self, rel, want):
        """(real path) of rel inside the project, or raises Refused."""
        if self.mode == "off":
            raise Refused("files off")
        if "\0" in rel or rel.startswith("/"):
            raise Refused("outside project")
        parts = rel.split("/") if rel else []
        if any(p in ("", ".", "..") for p in parts):
            raise Refused("outside project")
        if any(bad_name(p) for p in parts):
            raise Refused("bad name")
        if any(hidden_name(p) for p in parts):
            raise Refused("hidden")
        real = os.path.realpath(os.path.join(self.root, *parts))
        if not inside(real, self.real_root):
            raise Refused("outside project")
        # Judge what the path really names, so a symlink cannot reach a
        # hidden or ignored file under another name.
        rparts = self.real_parts(real)
        if any(hidden_name(p) for p in rparts):
            raise Refused("hidden")
        if rparts and await self._is_ignored(rparts):
            raise Refused("hidden")
        if want == "dir" and not os.path.isdir(real):
            raise Refused("not found")
        if want == "file" and not os.path.isfile(real):
            raise Refused("not found")
        return real

    def _load(self, rel, real):
        st = os.stat(real)
        key = (st.st_mtime_ns, st.st_size)
        hit = self.cache.get(rel)
        if hit and hit[0] == key:
            return hit[1]
        with open(real, "rb") as f:
            data = f.read(READ_CAP_BYTES + 1)
        cut = len(data) > READ_CAP_BYTES
        ft = FileText(data[:READ_CAP_BYTES] if cut else data, self.mode, cut)
        self.cache.pop(rel, None)
        while len(self.cache) >= 8:
            self.cache.pop(next(iter(self.cache)))
        self.cache[rel] = (key, ft)
        return ft

    # ---- requests ----------------------------------------------------------

    async def handle(self, tag, payload):
        now = time.monotonic()
        for x in [x for x, s in self.saves.items()
                  if now - s.touched > (CONFLICT_IDLE_SECS if s.conflict else SAVE_IDLE_SECS)]:
            del self.saves[x]
        if tag == "L":
            await self._list(payload)
        elif tag == "G":
            await self._get(payload)
        elif tag == "F":
            await self._save_begin(payload)
        elif tag == "S[":
            parts = payload.split()
            if len(parts) == 2 and parts[0] in self.saves:
                self.block = [parts[0], parts[1], []]
        elif tag == "S":
            if self.block is not None:
                self.block[2].append(payload)
        elif tag == "S]":
            await self._block_end(payload)
        elif tag == "S.":
            await self._commit(payload)

    async def _refuse(self, x, reason):
        await self.ch.tag("N", f"{x} {reason}")

    async def _list(self, rel):
        try:
            real = await self.resolve(rel, "dir")
            names = os.listdir(real)
        except Refused as r:
            return await self._refuse("-", r.reason)
        except OSError as e:
            return await self._refuse("-", f"failed {e.strerror or e}")
        found = []
        for name in names:
            if bad_name(name) or hidden_name(name):
                continue
            try:
                name.encode("mac_roman")
            except UnicodeEncodeError:
                continue
            target = os.path.realpath(os.path.join(real, name))
            if not inside(target, self.real_root) or target == self.real_root:
                continue
            tparts = self.real_parts(target)
            if any(hidden_name(p) for p in tparts):
                continue
            if os.path.isdir(target):
                found.append(("d", 0, name, "/".join(tparts)))
            elif os.path.isfile(target):
                found.append(("f", os.path.getsize(target), name, "/".join(tparts)))
        paths = [t + ("/" if k == "d" else "") for k, _, _, t in found]
        hits, ok = await self.ignored_set(paths)
        shown = []
        for (kind, size, name, t), p in zip(found, paths):
            ign = p in hits
            if ok and t.count("/") == len(self.real_parts(real)):
                self.ignored[t] = ign      # a direct child: cache it
            if not ign:
                shown.append((kind, size, name))
        shown.sort(key=lambda e: (e[0] != "d", e[2].casefold()))
        await self.ch.tag("L[", rel)
        for kind, size, name in shown[:LIST_MAX]:
            await self.ch.tag("L", f"{kind} {size} {name}")
        await self.ch.tag("L]", "cut" if len(shown) > LIST_MAX else "")

    async def _get(self, payload):
        parts = payload.split(" ", 4)
        if len(parts) < 5 or not all(p.isdigit() for p in parts[1:4]):
            return
        x, first, count, budget, rel = parts[0], int(parts[1]), int(parts[2]), int(parts[3]), parts[4]
        try:
            real = await self.resolve(rel, "file")
            ft = self._load(rel, real)
        except Refused as r:
            return await self._refuse(x, r.reason)
        except OSError as e:
            return await self._refuse(x, f"failed {e.strerror or e}")
        count = min(count, PIECE_MAX_LINES)
        budget = max(1, min(budget, PIECE_MAX_BYTES))
        out, size, i = [], 0, first
        while i < len(ft) and len(out) < count:
            b = ft.line(i)
            add = len(b) + (1 if out else 0)
            if out and size + add > budget:
                break
            out.append(b)
            size += add
            i += 1
        await self.ch.tag("H", f"{x} {first} {len(out)} {len(ft)} {ft.token} {ft.mode}")
        for b in out:
            await self.ch.send_split("B", b)
        data = b"\r".join(out)
        await self.ch.tag("Z", f"{x} {len(data)} {crc_hex(data)}")

    async def _save_begin(self, payload):
        parts = payload.split(" ", 2)
        if len(parts) < 3:
            return
        x, token, rel = parts
        if self.mode == "off":
            return await self._refuse(x, "files off")
        if self.mode != "rw":
            return await self._refuse(x, "read-only")
        try:
            real = await self.resolve(rel, "file")
            ft = self._load(rel, real)
        except Refused as r:
            return await self._refuse(x, r.reason)
        except OSError as e:
            return await self._refuse(x, f"failed {e.strerror or e}")
        if ft.mode != "e":
            return await self._refuse(x, "read-only")
        if x not in self.saves and len(self.saves) >= 4:
            return await self._refuse(x, "busy")
        self.saves[x] = Save(rel, token)
        await self.ch.tag("K", f"{x} 0")

    async def _block_end(self, payload):
        parts = payload.split()
        blk, self.block = self.block, None
        if len(parts) != 4:
            return
        x, seq, nbytes, crc = parts
        save = self.saves.get(x)
        if save is None:
            return await self._refuse(x, "not found")
        save.touched = time.monotonic()
        ok = blk is not None and blk[0] == x and blk[1] == seq and seq.isdigit()
        if ok:
            data = b"\r".join(l.encode("mac_roman") for l in blk[2])
            ok = str(len(data)) == nbytes and crc_hex(data) == crc.lower()
        if not ok:
            return await self.ch.tag("N", f"{x} {seq} resend")
        save.blocks[int(seq)] = blk[2]
        await self.ch.tag("K", f"{x} {seq}")

    async def _commit(self, payload):
        parts = payload.split()
        if len(parts) not in (3, 4):
            return
        x, nbytes, crc = parts[:3]
        force = len(parts) == 4 and parts[3] == "force"
        if x in self.done:
            return await self.ch.tag(*self.done[x])
        save = self.saves.get(x)
        if save is None:
            return await self._refuse(x, "not found")
        save.touched = time.monotonic()
        seqs = sorted(save.blocks)
        lines = [l for s in seqs for l in save.blocks[s]]
        data = b"\r".join(l.encode("mac_roman") for l in lines)
        if not seqs or seqs != list(range(1, len(seqs) + 1)) \
                or str(len(data)) != nbytes or crc_hex(data) != crc.lower():
            del self.saves[x]
            return await self._refuse(x, "damaged")
        if any(_CONTROL.search(l) for l in lines):
            del self.saves[x]
            return await self._refuse(x, "control")
        try:
            real = await self.resolve(save.rel, "file")
            with open(real, "rb") as f:
                current = f.read()
            if file_token(current) != save.token and not force:
                save.conflict = True    # kept while the person decides
                return await self._refuse(x, "changed")
            now = FileText(current, "rw")
            out = (now.eol.join(lines) + (now.eol if now.final else "")).encode("utf-8")
            if len(data) > SAVE_MAX_BYTES:
                raise Refused("too big")
            write_atomic(real, out)
        except Refused as r:
            del self.saves[x]
            return await self._refuse(x, r.reason)
        except OSError as e:
            del self.saves[x]
            return await self._refuse(x, f"failed {e.strerror or e}")
        del self.saves[x]
        self.cache.pop(save.rel, None)
        self.done[x] = ("K", f"{x} saved {file_token(out)}")
        while len(self.done) > 16:
            self.done.pop(next(iter(self.done)))
        self.ch.note_edit(save.rel)
        await self.ch.tag(*self.done[x])


def write_atomic(real, data):
    """Replaces real with data: a temporary file beside it, the same
    permissions, flushed to disk, then renamed over it."""
    folder, base = os.path.split(real)
    stale = f".{base}.se30-"
    for name in os.listdir(folder):
        if name.startswith(stale) and name.endswith(".tmp"):
            with contextlib.suppress(OSError):
                os.unlink(os.path.join(folder, name))
    mode = stat.S_IMODE(os.stat(real).st_mode)
    fd, tmp = tempfile.mkstemp(prefix=stale, suffix=".tmp", dir=folder)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fchmod(f.fileno(), mode)
            os.fsync(f.fileno())
        os.replace(tmp, real)
    except BaseException:
        with contextlib.suppress(OSError):
            os.unlink(tmp)
        raise


# --------------------------------------------------------------------------
# Protocol version 2: several sessions over one call
# --------------------------------------------------------------------------

V2_FILE_TAGS = ("L", "G", "F", "S[", "S", "S]", "S.")

RESUME_TOKEN = re.compile(r"[0-9a-f]{16}")
RESUME_MAX_AGE = 30 * 86400      # Claude Code's default transcript retention
CHAT_NAME = "*"                  # opens a chat: never a project's name
CHAT_MAX_AGE = 7 * 86400         # an unused chat is deleted at the next call
CHAT_DIR = re.compile(r"[0-9a-f]{16}")
CLAUDE_ID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
CUT_NOTE = ("[The link to the SE/30 dropped during your previous turn, which was "
            "stopped there. The person did not see the end of it.]")


def chats_root(state_dir):
    return os.path.join(state_dir, "chats")


def chat_folder_ok(state_dir, path):
    """Is path a chat folder this bridge made: a direct child of the chats
    folder with a generated name, and not a symlink?"""
    if not isinstance(path, str):
        return False
    root = chats_root(state_dir)
    return (os.path.dirname(path) == root and bool(CHAT_DIR.fullmatch(os.path.basename(path)))
            and not os.path.islink(path))


def delete_transcript(claude_id):
    """Removes Claude Code's saved transcript of one conversation: the
    <id>.jsonl (and <id> folder) inside whichever direct child of its
    projects folder holds it, then that child if it is now empty. Nothing
    is followed through a symlink, and a failure is logged, not raised."""
    if not isinstance(claude_id, str) or not CLAUDE_ID.fullmatch(claude_id):
        return
    base = os.path.join(os.environ.get("CLAUDE_CONFIG_DIR") or os.path.expanduser("~/.claude"),
                        "projects")
    try:
        children = [e.path for e in os.scandir(base) if e.is_dir(follow_symlinks=False)]
    except OSError:
        return
    for child in children:
        file = os.path.join(child, claude_id + ".jsonl")
        folder = os.path.join(child, claude_id)
        try:
            removed = False
            if os.path.lexists(file):
                if os.path.islink(file) or not os.path.isfile(file):
                    log(f"not deleting {file}: it is not a plain file")
                else:
                    os.unlink(file)
                    removed = True
            if os.path.lexists(folder) and not os.path.islink(folder) and os.path.isdir(folder):
                shutil.rmtree(folder, ignore_errors=True)
                removed = True
            if removed and not os.listdir(child):
                os.rmdir(child)
        except OSError as e:
            log(f"could not delete the transcript {claude_id}: {e}")


def forget_chat(state_dir, folder, claude_id):
    """Deletes a chat's folder and Claude's transcript of it."""
    if chat_folder_ok(state_dir, folder):
        shutil.rmtree(folder, ignore_errors=True)
    delete_transcript(claude_id)


def sweep_chats(store, state_dir):
    """At the start of a call: deletes chats nobody used for CHAT_MAX_AGE
    (the app crashed, or quit offline) and any chat folder no record names."""
    cutoff = time.time() - CHAT_MAX_AGE
    changed = False
    for token, rec in list(store.recs.items()):
        if rec.get("project") == CHAT_NAME and rec.get("used", 0) < cutoff:
            forget_chat(state_dir, rec.get("cwd"), rec.get("claude_id"))
            del store.recs[token]
            changed = True
    named = {os.path.basename(r.get("cwd") or "") for r in store.recs.values()
             if r.get("project") == CHAT_NAME}
    try:
        folders = [e for e in os.scandir(chats_root(state_dir))]
    except OSError:
        folders = []
    for e in folders:
        if e.name not in named:
            if chat_folder_ok(state_dir, e.path):
                shutil.rmtree(e.path, ignore_errors=True)
            else:
                log(f"leaving {e.path}: it is not a chat folder")
    if changed:
        store.save()


class ResumeStore:
    """sessions.json in the state folder: one record per conversation a v2
    session runs, under an opaque token the app keeps. The bridge resumes
    only conversations it started, only in the project they started in.
    Only the holder of the call lock writes it."""

    def __init__(self, state_dir):
        self.path = os.path.join(state_dir, "sessions.json")
        self.recs = {}
        try:
            with open(self.path, encoding="utf-8") as f:
                recs = json.load(f)
        except (OSError, ValueError):
            recs = {}
        cutoff = time.time() - RESUME_MAX_AGE
        if isinstance(recs, dict):
            self.recs = {t: r for t, r in recs.items()
                         if isinstance(r, dict)
                         and (r.get("project") == CHAT_NAME or r.get("used", 0) >= cutoff)}

    def get(self, token, project):
        rec = self.recs.get(token) if RESUME_TOKEN.fullmatch(token or "") else None
        return rec if rec and rec.get("project") == project else None

    def put(self, token, rec):
        rec["used"] = time.time()
        self.recs[token] = rec
        self.save()

    def delete(self, token):
        if self.recs.pop(token, None) is not None:
            self.save()

    def save(self):
        try:
            os.makedirs(os.path.dirname(self.path), mode=0o700, exist_ok=True)
            os.close(os.open(self.path, os.O_WRONLY | os.O_CREAT, 0o600))
            write_atomic(self.path, json.dumps(self.recs, indent=1).encode())
        except OSError as e:
            log(f"could not save resume records: {e}")


class Channel:
    """One v2 session: a conversation in one project. It has the same output
    methods as Terminal, with every line prefixed by the session digit."""

    def __init__(self, v2, sid, name, path, token=None, preset=None):
        args = v2.args
        self.v2 = v2
        self.want_token = token   # the conversation the app asked to resume
        self.token = None
        self.cut = False          # the resumed conversation's last turn was cut off
        self.term = v2.term
        self.sid = sid
        self.name = name
        self.chat = name == CHAT_NAME
        self.chat_dir = None      # a chat's own folder, once it has one
        self.cols = self.term.cols
        self.cfg = SessionConfig(path, args.accept_edits, args.model)
        self.cfg.chat = self.chat
        self.cfg.partial = True       # so answers can stream when the tab asks
        self.stream = False           # the tab's switch (see BlockStream)
        self._para_open = False       # the last line sent was a paragraph the app can continue
        # The tab's own model and effort (None: the bridge default). The
        # client owns them and presets them before opening; a change made
        # during a turn waits in `pending` for the turn to end.
        self.pick_model, self.pick_effort, self.preset_problems = \
            check_settings(args, *(preset or (None, None)))
        self.cfg.model = self.pick_model or args.model
        self.cfg.effort = self.pick_effort
        self.pending = None
        self.pending_plan = None   # a plan mode change waiting for the turn to end
        self.prompts = asyncio.Queue()
        self._wake_queued = False
        self.answers = asyncio.Queue()
        self.on_interrupt = None
        self.busy = False
        self.closing = False
        self.idle = asyncio.Event()
        self.idle.set()
        self.session = None
        self.task = None
        self.edited = []
        self._in_fence = False
        # A chat has no project folder to show: its file service refuses.
        self.files = FileService(self, path, "off" if self.chat else args.files)
        # A line and its "+" continuations go out together: the client
        # routes "+" by the session's last line, and prose and file pieces
        # come from different tasks.
        self.group = asyncio.Lock()

    # ---- output ------------------------------------------------------------

    @property
    def keep_marks(self):
        """The app sent !MD: it styles ** and __ itself."""
        return self.v2.md_optin

    async def tag(self, t, payload=""):
        self._para_open = False
        line = f"{self.sid}{t}" + (f" {payload}" if payload else "")
        await self.term.send_line(transliterate(line, "macroman"), counted=t != ">")

    async def stream_pieces(self, pairs):
        """Pieces of a streamed paragraph (see StreamChunker). The app joins
        a + to the session's last T, C or B line, so a + goes out only when
        nothing else was sent since the paragraph's last piece; otherwise
        it opens a new paragraph with a T."""
        async with self.group:
            for t, payload in pairs:
                if t == "+" and not self._para_open:
                    t = "T"
                await self.tag(t, payload)
                self._para_open = t in ("T", "+", "C")

    def set_stream(self, on):
        """The app turned this tab's streaming on or off."""
        self.stream = on
        change = getattr(self.session, "stream_changed", None)
        if change:
            asyncio.ensure_future(change())   # never hold up the input router

    async def send_split(self, t, data: bytes):
        """Raw Mac Roman bytes as one line plus "+" continuations of 200 bytes."""
        self._para_open = False
        async with self.group:
            for i in range(0, max(len(data), 1), 200):
                chunk = data[i:i + 200]
                head = f"{self.sid}{t if i == 0 else '+'}".encode()
                await self.term.send_line(head + (b" " + chunk if chunk else b""))

    async def text(self, md):
        async with self.group:
            for t, payload in app_text_lines(md, self):
                await self.tag(t, payload)

    async def tool(self, name, summary):
        await self.tag("U", f"{name} {summary}")

    # Subagents. An app that sent !AGENTS gets J+ (started), J~ (progress)
    # and J- (ended) to draw one live line each; everyone else gets the
    # plain start and end lines.
    async def agent_start(self, k, typ, task):
        if self.v2.agents_optin:
            await self.tag("J+", f"{k} {typ} {clip(task, 120)}")
        else:
            await self.tool("Agent", f"{typ}: {clip(task, 120)}")

    async def agent_step(self, k, steps, tool, summary):
        if self.v2.agents_optin:
            await self.tag("J~", f"{k} {steps} {tool} {clip(summary, 120)}")

    async def agent_end(self, k, typ, status, steps, summary):
        if self.v2.agents_optin:
            await self.tag("J-", f"{k} {status} {steps} {clip(summary, 120)}")
        else:
            await self.tool("Agent", f"{typ} {status}: {clip(summary, 120)}")

    async def tool_error(self, msg):
        await self.tag("E", clip(msg, 200))

    async def notice(self, msg):
        await self.tag("I", msg)

    async def result(self, status, cost, meter=None):
        await self.tag("R", f"{status} {f'${cost:.4f}' if cost is not None else '-'}")

    async def usage(self, meter):
        """What the conversation has used and cost, for an app that sent !USAGE."""
        wire = meter.wire() if self.v2.usage_optin else None
        if wire:
            await self.tag("$", wire)

    async def ask_permission(self, pid, name, summary, detail_lines):
        await self.tag("Q", f"{pid} {name} {summary}")
        if detail_lines:
            await self.tag("D[")
            for d in detail_lines:
                await self.tag("D", d)
            await self.tag("D]")
        await self.tag("?", str(pid))
        while True:
            if self.closing:
                return "n"
            ans = await self.answers.get()
            if ans is None:
                raise EOFError
            parts = ans.split()
            if len(parts) == 2 and parts[0] == str(pid) and parts[1] in ("y", "n", "a"):
                return parts[1]

    # ---- the conversation --------------------------------------------------

    def note_edit(self, rel):
        if rel not in self.edited:
            self.edited.append(rel)

    async def _restart(self):
        """A fresh conversation. Returns False (and says why) when it could
        not start; the session is then closed, which frees its slot."""
        await self.session.stop()
        self.v2.store.delete(self.token)
        if self.chat:
            delete_transcript(self.cfg.claude_id)   # nothing can resume it now
        self._new_conversation()
        self.session = make_session(self.v2.args, self, self.cfg)
        try:
            await self.session.start()
        except Exception as e:  # noqa: BLE001
            first = (str(e).strip().splitlines() or ["error"])[0]
            await self.tool_error(f"Could not start a new conversation: {first}")
            self.v2.close_later(self.sid)
            return False
        await self.tag("Y", f"{self.token} new")
        await self.send_plan()
        return True

    # ---- model and effort --------------------------------------------------

    async def send_settings(self, model, effort):
        await self.tag("M", wire_settings(model, effort))

    async def change(self, model, effort):
        """A change from the app (or a typed command): confirmed at once,
        applied between turns."""
        m, e, problems = check_settings(self.v2.args, model, effort)
        for p in problems:
            await self.tool_error(p)
        self.pending = (m, e)
        if self.busy:
            await self.notice("Model and effort change when this turn ends.")
            await self.send_settings(m, e)
        else:
            # Confirmed when applied, together with the fresh >: an effort
            # change restarts Claude, which can take a while, and a > that
            # came long after the confirmation could end a turn the person
            # started in between.
            self.prompts.put_nowait(CHANGE)

    async def _apply_pending(self):
        """Applies a waiting change. False when the session could not be
        restarted (it is then closed)."""
        if not self.pending:
            return True
        model, effort = self.pending
        self.pending = None
        old_model, old_effort = self.pick_model, self.pick_effort
        if (model, effort) == (old_model, old_effort):
            return True
        self.pick_model, self.pick_effort = model, effort
        self.cfg.model = model or self.v2.args.model
        self.cfg.effort = effort
        try:
            if effort != old_effort:
                await self._resume_restart()
            else:
                try:
                    await self.session.set_model(self.cfg.model)
                except Exception:  # noqa: BLE001
                    await self._resume_restart()
        except Exception as e:  # noqa: BLE001
            first = (str(e).strip().splitlines() or ["error"])[0]
            await self.tool_error(f"Could not change model or effort: {first}")
            self.v2.close_later(self.sid)
            return False
        return True

    async def _resume_restart(self):
        """Starts the session's Claude again on the same conversation."""
        await self.session.stop()
        self.cfg.resume = self.cfg.turns > 0
        self.session = make_session(self.v2.args, self, self.cfg)
        await self.session.start()

    async def _settings_command(self, words):
        m, e, lines, changed = setting_command(
            self.v2.args, words, self.pick_model, self.pick_effort)
        for line in lines:
            await self.notice(line)
        if changed:
            self.pending = (m, e)
            await self.send_settings(m, e)
            if not await self._apply_pending():
                return False
            await self.notice(f"{'Model' if words[0] == '/model' else 'Effort'} set.")
        return True

    # ---- plan mode ---------------------------------------------------------

    async def send_plan(self):
        """The confirmed mode, for an app that sent !CMDS."""
        if self.v2.cmds_optin:
            await self.tag("@", "plan" if self.cfg.plan else "default")

    async def plan_exited(self):
        """The person approved Claude's plan: the tab leaves plan mode now."""
        self._save(turn_running=True)
        await self.send_plan()
        await self.notice("Plan approved. Plan mode is off.")

    async def set_plan(self, on):
        if self.chat:
            await self.notice("Plan mode does not apply in a chat.")
        elif self.busy:
            self.pending_plan = on
            await self.notice("Plan mode changes after this turn.")
        else:
            await self._apply_plan(on)

    async def _apply_plan(self, on):
        self.pending_plan = None
        if on != self.cfg.plan:
            mode = "plan" if on else ("acceptEdits" if self.cfg.accept_edits else "default")
            try:
                await self.session.set_permission_mode(mode)
            except Exception as e:  # noqa: BLE001
                first = (str(e).strip().splitlines() or ["error"])[0]
                await self.tool_error(f"Could not change plan mode: {first}")
                return
            self.cfg.plan = on
            self._save(turn_running=False)
            if not self.v2.cmds_optin:
                await self.notice(f"Plan mode {'on' if on else 'off'}.")
        await self.send_plan()

    # ---- resume records ----------------------------------------------------

    def _chat_folder(self, folder=None):
        """A chat's own folder: the one its record names, else a new one."""
        root = chats_root(self.v2.args.state_dir)
        os.makedirs(root, mode=0o700, exist_ok=True)
        if folder is None:
            folder = os.path.join(root, secrets.token_hex(8))
        os.makedirs(folder, mode=0o700, exist_ok=True)
        self.chat_dir = self.cfg.cwd = folder

    def discard_chat(self):
        """Deletes a closed chat's folder and Claude's transcript of it."""
        if self.chat:
            forget_chat(self.v2.args.state_dir, self.chat_dir, self.cfg.claude_id)

    def _new_conversation(self):
        self.token = secrets.token_hex(8)
        if self.chat and not self.chat_dir:
            self._chat_folder()
        self.cfg.claude_id = str(uuid.uuid4())
        self.cfg.resume = False
        self.cfg.always = set()
        self.cfg.plan = False
        self.cfg.turns = 0
        self.cfg.meter = UsageMeter()
        self.cut = False
        self._save(turn_running=False)

    def _save(self, turn_running):
        rec = {
            "project": self.name, "claude_id": self.cfg.claude_id,
            "accept_edits": self.cfg.accept_edits, "always": sorted(self.cfg.always),
            "turns": self.cfg.turns, "turn_running": turn_running, "plan": self.cfg.plan,
            "usage": self.cfg.meter.to_record()}
        if self.chat:
            rec["cwd"] = self.chat_dir
        self.v2.store.put(self.token, rec)

    def _take_record(self):
        """Sets up the conversation the app asked for. Returns "resumed",
        "cut", or None when there is nothing to resume."""
        rec = self.v2.store.get(self.want_token, self.name)
        if not rec:
            return None
        if self.chat:
            if not chat_folder_ok(self.v2.args.state_dir, rec.get("cwd")):
                return None
            # Recreated when it was deleted: the conversation may still be
            # saved under that path, and when it is not, the resume fails
            # and a new one starts.
            self._chat_folder(rec["cwd"])
        self.token = self.want_token
        self.cfg.claude_id = rec.get("claude_id") or str(uuid.uuid4())
        self.cfg.accept_edits = bool(rec.get("accept_edits"))
        self.cfg.always = set(rec.get("always") or [])
        self.cfg.plan = bool(rec.get("plan")) and not self.chat
        self.cfg.turns = int(rec.get("turns") or 0)
        self.cfg.meter = UsageMeter.from_record(rec.get("usage"))
        # A conversation that never ran a turn has nothing saved to resume:
        # it starts under the same id instead.
        self.cfg.resume = self.cfg.turns > 0
        self.cut = bool(rec.get("turn_running"))
        return "cut" if self.cut else "resumed"

    async def _start(self):
        """Starts the conversation; resumes the one the app asked for when
        it can, else a new one. Returns how it started, or raises."""
        how = self._take_record() if self.want_token else None
        if how is None:
            self._new_conversation()
            how = "new"
        self.session = make_session(self.v2.args, self, self.cfg)
        try:
            await self.session.start()
        except Exception as e:  # noqa: BLE001
            if how == "new":
                raise
            first = (str(e).strip().splitlines() or ["error"])[0]
            log(f"session {self.sid}: could not resume ({first}); starting a new conversation")
            self.v2.store.delete(self.token)
            self._new_conversation()
            how = "new"
            self.session = make_session(self.v2.args, self, self.cfg)
            await self.session.start()
        return how

    async def _read_prompt(self, announce=True):
        if announce:
            await self.tag(">")
        parts = []
        while True:
            line = await self.prompts.get()
            if line is None:
                raise EOFError
            if line is WAKE:
                self._wake_queued = False
                if parts:
                    continue     # mid-prompt: its turn shows it first
                return WAKE
            if line is CHANGE:
                if parts:
                    # Mid-prompt: don't drop the lines already read. The
                    # change applies before this prompt's turn; confirm it now.
                    if self.pending:
                        await self.send_settings(*self.pending)
                    continue
                return CHANGE
            if line.endswith("\\"):
                parts.append(line[:-1])
                continue
            parts.append(line)
            return "\n".join(parts).strip()

    async def run(self):
        try:
            how = await self._start()
        except Exception as e:  # noqa: BLE001
            first = (str(e).strip().splitlines() or ["error"])[0]
            await self.files.stop()
            self.v2.forget(self)
            if self.token:
                self.v2.store.delete(self.token)
            self.discard_chat()
            await self.tag("X", clip(f"could not start: {first}", 200))
            return
        try:
            await self.tag("O", self.name)
            await self.tag("Y", f"{self.token} {how}")
            await self.send_plan()
            await self.usage(self.cfg.meter)   # a resumed conversation's reading
            for p in self.preset_problems:
                await self.tool_error(p)
            await self.send_settings(self.pick_model, self.pick_effort)
            announce = True
            while True:
                prompt = await self._read_prompt(announce)
                announce = True
                if prompt is CHANGE:
                    if self.pending_plan is not None:
                        await self._apply_plan(self.pending_plan)
                    if not await self._apply_pending():
                        return
                    await self.send_settings(self.pick_model, self.pick_effort)
                    # A prompt typed meanwhile is already queued: its turn
                    # ends with its own >, so no fresh one now.
                    announce = self.prompts.empty()
                    continue
                if prompt is WAKE:
                    announce = await self._follow()
                    continue
                if not prompt:
                    continue
                cmd = prompt.lower().split()
                if cmd[0] == "/quit":
                    self.v2.close_later(self.sid)
                    return
                if cmd[0] == "/help":
                    for line in (CHAT_HELP if self.chat else HELP).split("\n"):
                        await self.notice(line)
                    continue
                if self.chat and cmd[0] in ("/agents", "/edits"):
                    await self.notice(f"{cmd[0]} does not apply in a chat.")
                    continue
                if cmd[0] == "/agents":
                    for line in agents_lines(self.cfg.cwd, getattr(self.session, "init_agents", [])):
                        await self.notice(line)
                    continue
                if cmd[0] == "/usage":
                    for line in await usage_report(self.session, self.cfg.meter):
                        await self.notice(line)
                    continue
                if cmd[0] == "/new":
                    if not await self._restart():
                        return
                    await self.notice("New conversation.")
                    continue
                if cmd[0] == "/edits" and len(cmd) == 2:
                    self.cfg.accept_edits = cmd[1] == "on"
                    if not await self._restart():
                        return
                    await self.notice(f"Auto-accept edits {'on' if self.cfg.accept_edits else 'off'}"
                                      " (new conversation).")
                    continue
                if cmd[0] in ("/model", "/effort"):
                    if not await self._settings_command(cmd):
                        return
                    continue
                kind, text = route_prompt(prompt, getattr(self.session, "allowed_commands", ()),
                                          getattr(self.session, "known_commands", ()))
                if kind == "commands":
                    for line in commands_lines(self.session):
                        await self.notice(line)
                    continue
                if kind == "plan":
                    if text in ("on", "off"):
                        await self.set_plan(text == "on")
                    elif self.chat:
                        await self.notice("Plan mode does not apply in a chat.")
                    else:
                        await self.notice(f"Plan mode is {'on' if self.cfg.plan else 'off'}. "
                                          "Use /plan on or /plan off.")
                    continue
                if kind == "clear":
                    if not await self._restart():
                        return
                    await self.notice("New conversation.")
                    continue
                if kind == "refuse" or (kind == "command" and self.chat
                                        and prompt.split()[0] != "/compact"):
                    await self.notice(text if kind == "refuse"
                                      else f"{prompt.split()[0]} does not apply in a chat.")
                    continue
                if not await self._apply_pending():
                    return
                if kind == "send":
                    prompt = text
                if kind == "command":
                    pass   # sent alone: an edited-files or cut note waits for the next prompt
                elif self.edited:
                    prompt = ("[The person edited these files from the SE/30 since your "
                              f"last turn: {', '.join(self.edited)}. Re-read them before "
                              "you edit them.]\n" + prompt)
                    self.edited = []
                if self.cut and kind != "command":
                    prompt = CUT_NOTE + "\n" + prompt
                self.busy = True
                self.idle.clear()
                self._in_fence = False
                self.cfg.turns += 1
                # Marked running until it ends with the link up, so a turn
                # stopped by a drop (or a killed bridge) reads as cut off.
                self._save(turn_running=True)
                try:
                    await self.session.turn(prompt)
                except EOFError:
                    raise
                except Exception as e:  # noqa: BLE001
                    await self.notice(f"Error: {e}")
                finally:
                    self.busy = False
                    self.idle.set()
                if self.pending_plan is not None:
                    self.prompts.put_nowait(CHANGE)
                if not self.term.eof and not self.closing:
                    if kind != "command":
                        self.cut = False
                    self.cfg.resume = True
                    self._save(turn_running=False)
        except EOFError:
            pass

    def wake(self):
        """Called by the session when Claude sent something between turns."""
        if not self._wake_queued:
            self._wake_queued = True
            self.prompts.put_nowait(WAKE)

    async def _follow(self):
        """Shows what Claude sent between turns, such as the turn it starts
        by itself when a background subagent ends. The SE/30 did not ask for
        that turn, so it ends with > only when no prompt of its own is
        waiting (that prompt's turn ends with its own >). Returns whether to
        send > now."""
        follow = getattr(self.session, "follow", None)
        if not follow:
            return False
        self.busy = True
        self.idle.clear()
        self._in_fence = False
        try:
            turned = await follow()
        except EOFError:
            raise
        except Exception as e:  # noqa: BLE001
            await self.notice(f"Error: {e}")
            turned = False
        finally:
            self.busy = False
            self.idle.set()
        if turned and not self.term.eof and not self.closing:
            self.cfg.resume = True
            self._save(turn_running=False)
        return turned and self.prompts.empty()

    async def shutdown(self):
        """Deny any open request, stop a running turn, stop the session."""
        self.closing = True
        self.answers.put_nowait("")
        if self.busy and self.on_interrupt:
            with contextlib.suppress(Exception):
                await self.on_interrupt()
        if self.task and not self.task.done():
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(self.idle.wait(), 5)
            self.task.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await self.task
        await self.files.stop()
        if self.session:
            with contextlib.suppress(Exception):
                await self.session.stop()
        rec = self.v2.store.recs.get(self.token) if self.token else None
        if rec is not None:
            # Keep grants given during the last turn for the next call.
            rec["always"] = sorted(self.cfg.always)
            rec["usage"] = self.cfg.meter.to_record()   # a result that came just before a drop
            self.v2.store.put(self.token, rec)


class V2:
    """Routes a v2 call's lines to its sessions."""

    def __init__(self, term, args):
        self.term = term
        self.args = args
        self.channels = {}
        self.presets = {}   # sid -> (model, effort) for a tab not open yet
        self.stream_presets = {}   # sid -> whether it streams, for a tab not open yet
        self.store = ResumeStore(args.state_dir)
        self.agents_optin = False   # the app sent !AGENTS: it draws subagent lines
        self.md_optin = False       # the app sent !MD: it styles ** and __ itself
        self.usage_optin = False    # the app sent !USAGE: it draws context and cost
        self.cmds_optin = False     # the app sent !CMDS: it takes <n>@ lines
        self.no_md = bool(getattr(args, "no_md", False))   # --no-md: do not offer it
        self.closing = set()        # close() tasks still running
        sweep_chats(self.store, args.state_dir)

    def forget(self, ch):
        if self.channels.get(ch.sid) is ch:
            del self.channels[ch.sid]

    async def send_models(self):
        await self.term.send_line(b"M[")
        for name in self.args.models:
            await self.term.send_line(f"M {name}".encode())
        await self.term.send_line(
            f"M] {wire_settings(self.args.model, None)}".encode())

    async def send_list(self):
        await self.term.send_line(b"P[")
        for name, _ in list_projects(self.args):
            await self.term.send_line(transliterate(f"P {name}", "macroman"))
        await self.term.send_line(b"P]")

    def close_later(self, sid):
        task = asyncio.ensure_future(self.close(sid))
        self.closing.add(task)
        task.add_done_callback(self.closing.discard)

    async def close(self, sid):
        ch = self.channels.get(sid)
        if ch is None or ch.closing:
            return
        await ch.shutdown()
        self.forget(ch)
        self.store.delete(ch.token)
        # Before the answer: after !QUIT the link may be gone by then.
        ch.discard_chat()
        await ch.tag("X", "closed")

    async def open(self, sid, name, token=None):
        if sid in self.channels:
            return await self.term.send_line(f"{sid}X already open".encode())
        if len(self.channels) >= self.args.max_sessions:
            return await self.term.send_line(f"{sid}X too many tabs".encode())
        path = chats_root(self.args.state_dir) if name == CHAT_NAME \
            else dict(list_projects(self.args)).get(name)
        if path is None:
            return await self.term.send_line(f"{sid}X not available".encode())
        ch = Channel(self, sid, name, path, token, self.presets.pop(sid, None))
        ch.stream = self.stream_presets.pop(sid, False)
        self.channels[sid] = ch
        ch.files.start()
        ch.task = asyncio.ensure_future(ch.run())

    async def session_line(self, sid, rest):
        sp = rest.find(" ")
        tag, payload = (rest, "") if sp < 0 else (rest[:sp], rest[sp + 1:])
        if tag == "O":
            return await self.open(sid, payload)
        if tag == "Y":
            token, _, name = payload.partition(" ")
            return await self.open(sid, name, token)
        if tag == "X":
            if sid in self.channels:
                self.close_later(sid)
            else:
                await self.term.send_line(f"{sid}X closed".encode())
            return
        if tag == "M":
            words = (payload.split() + ["-", "-"])[:2]
            ch = self.channels.get(sid)
            if ch is None:
                self.presets[sid] = tuple(words)
            elif not ch.closing:
                await ch.change(*words)
            return
        if tag == "@":
            ch = self.channels.get(sid)
            if ch is not None and not ch.closing and payload in ("plan", "default"):
                await ch.set_plan(payload == "plan")
            return
        if tag == "V":
            if payload in ("on", "off"):
                ch = self.channels.get(sid)
                if ch is None:
                    self.stream_presets[sid] = payload == "on"
                elif not ch.closing:
                    ch.set_stream(payload == "on")
            return
        ch = self.channels.get(sid)
        if ch is None or ch.closing:
            return
        if tag == "P":
            ch.prompts.put_nowait(payload)
        elif tag == "A":
            ch.answers.put_nowait(payload)
        elif tag == "!STOP":
            if ch.busy and ch.on_interrupt:
                asyncio.ensure_future(ch.on_interrupt())
        elif tag in V2_FILE_TAGS:
            ch.files.queue.put_nowait((tag, payload))

    async def run(self):
        try:
            words = "resume ping models agents chat stream" + ("" if self.no_md else " md") + " usage cmds"
            await self.term.send_line(f"#OK se30bridge 2 files={self.args.files} {words}".encode(),
                                      counted=False)
            await self.send_models()
            await self.send_list()
            while True:
                line = await self.term.lines.get()
                if line is None or line == "!QUIT":
                    break
                if line == "!AGENTS":
                    self.agents_optin = True
                    continue
                if line == "!MD":
                    self.md_optin = not self.no_md
                    continue
                if line == "!USAGE":
                    self.usage_optin = True
                    continue
                if line == "!CMDS":
                    self.cmds_optin = True
                    continue
                if line == "!LIST":
                    await self.send_list()
                elif line[:1] in ("1", "2", "3", "4", "5", "6") and line:
                    await self.session_line(int(line[0]), line[1:])
        except EOFError:
            pass
        finally:
            # A close the app asked for before it quit finishes first: it
            # deletes a chat, which a plain shutdown keeps for the next call.
            await asyncio.gather(*self.closing, return_exceptions=True)
            chans = list(self.channels.values())
            self.channels.clear()
            await asyncio.gather(*(c.shutdown() for c in chans), return_exceptions=True)


# --------------------------------------------------------------------------
# Main loop
# --------------------------------------------------------------------------

async def start_default(term, session):
    """Starts the tty/v1 session in the background; True if it started."""
    try:
        await session.start()
    except Exception as e:  # noqa: BLE001
        with contextlib.suppress(EOFError):
            await term.notice(f"Could not start Claude: {e}")
        return False
    with contextlib.suppress(EOFError):
        await term.notice("Type /help for commands.")
    return True


async def main_async(args):
    link = Link(args.baud)
    await link.open()
    term = Terminal(link, args)
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGTERM, signal.SIGHUP):
        loop.add_signal_handler(sig, term.lose_link)
    asyncio.ensure_future(term.dispatch())

    cfg = SessionConfig(args.cwd or os.getcwd(), args.accept_edits, args.model)
    picks = [None, None]   # /model and /effort picks; None is the default
    session = make_session(args, term, cfg)
    starting = None
    try:
        await term.notice(f"se30bridge {'(demo) ' if args.demo else ''}- {cfg.cwd}")
        await term.notice("Starting Claude..." if not args.demo else "Ready.")
        # tty and v1 start their session while the first line is typed; a v2
        # HELLO stops it, so v2 never waits for (or keeps) a CLI it won't use.
        starting = asyncio.ensure_future(start_default(term, session))
        # Show the first prompt after the start notice, as before, unless the
        # client is already typing (an app-mode HELLO).
        typed = asyncio.ensure_future(term.typed.wait())
        await asyncio.wait({starting, typed}, return_when=asyncio.FIRST_COMPLETED)
        typed.cancel()
        while True:
            try:
                prompt = await term.read_prompt()
            except EnterV2:
                starting.cancel()
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await starting
                await session.stop()
                session = None
                await V2(term, args).run()
                return
            if not prompt:
                continue
            if not await starting:
                await term.notice("Bye.")
                return
            cmd = prompt.lower().split()
            if cmd[0] == "/quit":
                await term.notice("Bye.")
                break
            if cmd[0] == "/help":
                for line in HELP.split("\n"):
                    await term.notice(line)
                continue
            if cmd[0] == "/agents":
                for line in agents_lines(cfg.cwd, getattr(session, "init_agents", [])):
                    await term.notice(line)
                continue
            if cmd[0] == "/usage":
                for line in await usage_report(session, cfg.meter):
                    await term.notice(line)
                continue
            if cmd[0] == "/new":
                await session.stop()
                new_conversation(cfg)
                session = make_session(args, term, cfg)
                await session.start()
                await term.notice("New conversation.")
                continue
            if cmd[0] in ("/model", "/effort"):
                m, e, lines, changed = setting_command(args, cmd, picks[0], picks[1])
                for line in lines:
                    await term.notice(line)
                if changed:
                    old_model, old_effort = picks
                    picks[:] = [m, e]
                    cfg.model = m or args.model
                    cfg.effort = e
                    # Between turns: the same conversation carries on, with
                    # its grants, under the new effort (a restart) or model.
                    if e != old_effort:
                        await session.stop()
                        cfg.resume = bool(cfg.turns and cfg.claude_id)
                        session = make_session(args, term, cfg)
                        await session.start()
                    else:
                        await session.set_model(cfg.model)
                    await term.notice(f"{'Model' if cmd[0] == '/model' else 'Effort'} set.")
                continue
            kind, text = route_prompt(prompt, getattr(session, "allowed_commands", ()),
                                      getattr(session, "known_commands", ()))
            if kind == "commands":
                for line in commands_lines(session):
                    await term.notice(line)
                continue
            if kind == "plan":
                if text in ("on", "off"):
                    on = text == "on"
                    mode = "plan" if on else ("acceptEdits" if cfg.accept_edits else "default")
                    try:
                        await session.set_permission_mode(mode)
                        cfg.plan = on
                        await term.notice(f"Plan mode {'on' if on else 'off'}.")
                    except Exception as e:  # noqa: BLE001
                        await term.notice(f"Could not change plan mode: {e}")
                else:
                    await term.notice(f"Plan mode is {'on' if cfg.plan else 'off'}. "
                                      "Use /plan on or /plan off.")
                continue
            if kind == "clear":
                await session.stop()
                new_conversation(cfg)
                session = make_session(args, term, cfg)
                await session.start()
                await term.notice("New conversation.")
                continue
            if kind == "refuse":
                await term.notice(text)
                continue
            if kind == "send":
                prompt = text
            if cmd[0] == "/edits" and len(cmd) == 2:
                cfg.accept_edits = cmd[1] == "on"
                new_conversation(cfg)
                await session.stop()
                session = make_session(args, term, cfg)
                await session.start()
                await term.notice(f"Auto-accept edits {'on' if cfg.accept_edits else 'off'}"
                                  " (new conversation).")
                continue
            term.busy = True
            term.skip_output = False
            term._in_fence = False
            if term.mode == "tty":
                await term.out_line("...")
            cfg.turns += 1
            try:
                await session.turn(prompt)
            except EOFError:
                raise
            except Exception as e:  # noqa: BLE001
                await term.notice(f"Error: {e}")
            finally:
                term.busy = False
            cfg.resume = True
    except EOFError:
        pass
    finally:
        if starting and not starting.done():
            starting.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await starting
        if session:
            with contextlib.suppress(Exception):
                await session.stop()


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--cwd", default=None,
                   help="project for tty and v1 mode, and one more project for tabs "
                        "(default: the current folder)")
    p.add_argument("--projects", action="append", metavar="DIR",
                   help="offer every subfolder of DIR as a project for tabs (repeatable)")
    p.add_argument("--max-sessions", type=int, default=6, help="tabs open at once")
    p.add_argument("--files", choices=["rw", "ro", "off"], default="rw",
                   help="what the SE/30 may do with project files (tabs only)")
    p.add_argument("--demo", action="store_true", help="fake Claude, to test the link")
    p.add_argument("--no-md", action="store_true",
                   help="do not offer the md word, as an older bridge does (for tests)")
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
    p.add_argument("--model", default=None,
                   help="the model a tab runs when it has not picked one")
    p.add_argument("--models", nargs="+", default=["opus", "sonnet", "haiku"],
                   metavar="NAME",
                   help="the models a tab can pick (up to 16; shown on the SE/30)")
    p.add_argument("--idle-timeout", type=float, default=180,
                   help="end the call when an app that sends heartbeats has been "
                        "silent this many seconds (0 = never)")
    p.add_argument("--state-dir", default="~/.se30bridge",
                   help="where the bridge keeps its call lock and resume records "
                        "(one call at a time per folder)")
    args = p.parse_args()
    args.models = [m for item in args.models for m in item.split(",") if m]
    if (not args.models or len(args.models) > MAX_MODELS
            or not all(MODEL_NAME.match(m) and m != "-" for m in args.models)):
        p.error(f"--models takes 1 to {MAX_MODELS} names of letters, digits, . _ - [ ]")
    if args.model and not MODEL_NAME.match(args.model):
        p.error("--model must be one name without spaces")
    args.state_dir = os.path.abspath(os.path.expanduser(args.state_dir))
    if args.cwd:
        args.cwd = os.path.abspath(os.path.expanduser(args.cwd))
    args.projects = [os.path.abspath(os.path.expanduser(d)) for d in args.projects or []]
    args.call_lock = take_call_lock(args.state_dir)
    try:
        asyncio.run(main_async(args))
    except (KeyboardInterrupt, SystemExit):
        pass


if __name__ == "__main__":
    main()
