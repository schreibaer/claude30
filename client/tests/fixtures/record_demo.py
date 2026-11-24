#!/usr/bin/env python3
"""Record what bridge.py --demo sends over protocol version 2, for the
protocol core tests.

Runs the bridge on a pipe (no modem, no pacing) with two projects, alpha and
beta, plays the client side, and writes every byte the bridge sent to
demo_session.bin:

  1. HELLO 2 handshake and the project list (the tty banner before it is kept)
  2. session 1 opened in alpha, session 2 in beta
  3. a two-line prompt on session 1 (backslash continuation), then "second"
     on session 2, so the two demo turns interleave
  4. one !ACK per W, "<n>A 1 y" for each permission request
  5. !QUIT

Usage: python3 record_demo.py [path/to/bridge.py]
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "bridge.py")


def main():
    root = tempfile.mkdtemp(prefix="se30demo")
    for name in ("alpha", "beta"):
        os.mkdir(os.path.join(root, name))
    p = subprocess.Popen([sys.executable, "-u", BRIDGE, "--demo", "--baud", "0",
                          "--projects", root],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    out = bytearray()
    buf = bytearray()

    def send(s):
        p.stdin.write(s)
        p.stdin.flush()

    def lines():
        while True:
            b = p.stdout.read(1)
            if not b:
                return
            out.extend(b)
            if b == b"\n":
                continue
            if b == b"\r":
                yield bytes(buf)
                buf.clear()
            else:
                buf.extend(b)

    def until(it, want):
        for line in it:
            if line == b"W":
                send(b"!ACK\r")
            if line == want:
                return

    send(b"\r#HELLO SE30 2\r")
    it = lines()
    until(it, b"P]")
    send(b"1O alpha\r")
    until(it, b"1>")
    send(b"2O beta\r")
    until(it, b"2>")
    send(b"1P hello\\\r1P there\r2P second\r")
    readies = set()
    for line in it:
        if line == b"W":
            send(b"!ACK\r")
        elif line in (b"1? 1", b"2? 1"):
            send(line[:1] + b"A 1 y\r")
        elif line in (b"1>", b"2>"):
            readies.add(line)
            if len(readies) == 2:
                break
    send(b"!QUIT\r")
    for line in it:
        # "I Bye." can complete a window; the bridge then waits for !ACK
        # before it exits, so keep acknowledging until it closes the link.
        if line == b"W":
            send(b"!ACK\r")
    p.wait(timeout=10)
    with open(os.path.join(HERE, "demo_session.bin"), "wb") as f:
        f.write(out)
    print(f"wrote {len(out)} bytes")


if __name__ == "__main__":
    main()
