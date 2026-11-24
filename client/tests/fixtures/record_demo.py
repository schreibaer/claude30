#!/usr/bin/env python3
"""Record what bridge.py --demo sends in app mode, for the protocol core tests.

Runs the bridge on a pipe (no modem, no pacing), plays the client side, and
writes every byte the bridge sent to demo_session.bin:

  1. HELLO handshake (the tty banner before it is kept)
  2. a two-line prompt sent with backslash continuation
  3. the demo turn: one !ACK per W, then "A 1 y" for the permission request
  4. /quit

Usage: python3 record_demo.py [path/to/bridge.py]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", "bridge.py")


def main():
    p = subprocess.Popen([sys.executable, "-u", BRIDGE, "--demo", "--baud", "0"],
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

    send(b"\r#HELLO SE30 1\r")
    it = lines()
    for line in it:
        if line == b"#OK se30bridge 1":
            break
    for line in it:
        if line == b">":
            break
    send(b"hello\\\rthere\r")
    readies = 0
    for line in it:
        if line == b"W":
            send(b"!ACK\r")
        elif line == b"? 1":
            send(b"A 1 y\r")
        elif line == b">":
            readies += 1
            if readies == 2:      # first is the continuation, second ends the turn
                break
    send(b"/quit\r")
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
