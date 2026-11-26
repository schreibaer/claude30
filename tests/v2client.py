"""A scripted protocol version 2 client for the bridge tests.

Runs bridge.py (or the stress bridge) as a subprocess over pipes, sends the
v2 HELLO, acknowledges every W unless told not to, and collects the lines it
receives so a test can wait for them. Standard library only.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "bridge.py")


class Client:
    def __init__(self, *bridge_args, script=BRIDGE, env=None, auto_ack=True, state_dir=None):
        e = dict(os.environ)
        e.update(env or {})
        # Each bridge gets its own state folder unless a test shares one, so
        # bridges in different tests never end each other's calls.
        if script == BRIDGE:
            if state_dir is None:
                state_dir = tempfile.mkdtemp(prefix="se30state")
                self._own_state = state_dir
            bridge_args = ("--state-dir", state_dir, *bridge_args)
        self.state_dir = state_dir
        self.p = subprocess.Popen([sys.executable, "-u", script, "--baud", "0", *bridge_args],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=e, bufsize=0)
        self.lines = []            # every line received, as bytes, in order
        self.raw = bytearray()
        self.cond = threading.Condition()
        self.eof = False
        self.auto_ack = auto_ack
        self.ws = 0
        self.stderr = b""
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._stderr, daemon=True).start()

    def _stderr(self):
        while True:
            chunk = self.p.stderr.read1(4096) if hasattr(self.p.stderr, "read1") \
                else self.p.stderr.read(1)
            if not chunk:
                return
            self.stderr += chunk

    def stderr_now(self, settle=0.2):
        """What the bridge has logged so far."""
        time.sleep(settle)
        return self.stderr

    def _reader(self):
        buf = bytearray()
        while True:
            b = self.p.stdout.read(1)
            if not b:
                with self.cond:
                    self.eof = True
                    self.cond.notify_all()
                return
            self.raw += b
            if b == b"\n":
                continue
            if b != b"\r":
                buf += b
                continue
            line = bytes(buf)
            buf.clear()
            with self.cond:
                self.lines.append(line)
                if line == b"W":
                    self.ws += 1
                    if self.auto_ack:
                        self.send("!ACK")
                self.cond.notify_all()

    def send(self, line):
        data = line if isinstance(line, bytes) else line.encode("mac_roman")
        try:
            self.p.stdin.write(data + b"\r")
            self.p.stdin.flush()
        except (BrokenPipeError, ValueError):
            pass

    def text(self):
        return [l.decode("mac_roman") for l in self.lines]

    def wait_for(self, pred, timeout=10, start=0):
        """Waits until pred(line) is true for a line at index >= start.
        Returns that line's index."""
        end = time.monotonic() + timeout
        with self.cond:
            i = start
            while True:
                while i < len(self.lines):
                    if pred(self.lines[i].decode("mac_roman")):
                        return i
                    i += 1
                left = end - time.monotonic()
                if left <= 0 or self.eof:
                    raise AssertionError(
                        f"timed out; last lines: {self.text()[-15:]!r}\n"
                        f"stderr: {self.stderr.decode(errors='replace')[-2000:]}")
                self.cond.wait(left)

    def wait_line(self, line, timeout=10, start=0):
        return self.wait_for(lambda l: l == line, timeout, start)

    def hello(self, agents=False, md=False):
        self.send(b"")
        self.send("#HELLO SE30 2")
        self.wait_for(lambda l: l.startswith("#OK se30bridge 2"))
        i = self.wait_line("P]")
        if agents:
            self.send("!AGENTS")
        if md:
            self.send("!MD")
        return i

    def projects(self, start=0):
        i = self.wait_line("P[", start=start)
        j = self.wait_line("P]", start=i)
        return [l[2:] for l in self.text()[i + 1:j] if l.startswith("P ")]

    def open(self, sid, name):
        start = len(self.lines)
        self.send(f"{sid}O {name}")
        i = self.wait_for(lambda l: l == f"{sid}O {name}" or l.startswith(f"{sid}X "),
                          start=start)
        return self.text()[i]

    def session(self, sid, start=0):
        """Payloads of this session's lines, as 'TAG payload' without the digit."""
        return [l[1:] for l in self.text()[start:] if l[:1] == str(sid)]

    def turn(self, sid, prompt, answer="y", timeout=20):
        """Sends a prompt and answers its permission requests; returns the
        index of the line that ended the turn."""
        start = len(self.lines)
        self.send(f"{sid}P {prompt}")
        i = start
        while True:
            i = self.wait_for(lambda l: l == f"{sid}>" or l.startswith(f"{sid}? "),
                              timeout, i)
            line = self.text()[i]
            if line == f"{sid}>":
                return i
            self.send(f"{sid}A {line.split()[1]} {answer}")
            i += 1

    def reap(self):
        """Kills the bridge if it still runs and closes the pipes."""
        if self.p.poll() is None:
            self.p.kill()
        self.p.wait()
        for f in (self.p.stdin, self.p.stdout, self.p.stderr):
            try:
                f.close()
            except (OSError, ValueError):
                pass

    def close(self, timeout=10):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()
            raise AssertionError("the bridge did not exit after stdin closed")
        finally:
            for f in (self.p.stdout, self.p.stderr):
                try:
                    f.close()
                except OSError:
                    pass
            own = getattr(self, "_own_state", None)
            if own:
                shutil.rmtree(own, ignore_errors=True)
