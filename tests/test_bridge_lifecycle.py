"""How a bridge ends: a new call ends the previous bridge, signals shut a
bridge down cleanly, and the heartbeat ends a bridge whose app went silent.
Run with:
    python3 -m unittest discover -s tests
Standard library only; the bridge runs as a subprocess over pipes.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from v2client import Client  # noqa: E402


def children(pid):
    out = subprocess.run(["ps", "-o", "pid=", "--ppid", str(pid)],
                         capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    # A zombie still answers kill(0); it has exited all the same.
    with open(f"/proc/{pid}/stat") as f:
        return f.read().split(")")[-1].split()[0] != "Z"


def wait_gone(pid, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if not alive(pid):
            return True
        time.sleep(0.05)
    return False


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for name in ("alpha", "beta"):
            os.mkdir(os.path.join(self.root, name))
        self.state = tempfile.mkdtemp(prefix="se30state")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(shutil.rmtree, self.state, True)

    def client(self, *args, env=None, **kw):
        e = {"SE30_EVICT_WAIT": "3"}
        e.update(env or {})
        c = Client("--demo", "--projects", self.root, *args, state_dir=self.state, env=e, **kw)
        self.addCleanup(c.reap)
        return c


class EvictionTest(Base):
    def test_new_call_ends_bridge_stuck_on_ack(self):
        old = self.client("--window", "12", "--models", "opus", auto_ack=False)
        old.hello()
        old.open(1, "alpha")
        old.wait_line("1>")
        old.send("1P hello")
        old.wait_line("W", start=0)
        n = old.ws
        time.sleep(0.5)
        self.assertEqual(old.ws, n)          # stuck: no !ACK, no more output
        new = self.client()
        new.hello()
        self.assertEqual(old.p.wait(8), 0)
        self.assertIn(b"ending the previous call", new.stderr_now())
        new.close()

    def test_bridge_that_ignores_sigterm_is_killed(self):
        holder = subprocess.Popen([sys.executable, "-c", (
            "import fcntl, os, signal, sys, time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o600)\n"
            "fcntl.flock(fd, fcntl.LOCK_EX)\n"
            "os.write(fd, str(os.getpid()).encode())\n"
            "print('locked', flush=True)\n"
            "time.sleep(60)\n"), os.path.join(self.state, "call.lock")],
            stdout=subprocess.PIPE)
        self.addCleanup(holder.stdout.close)
        self.addCleanup(lambda: holder.poll() is None and holder.kill())
        self.assertEqual(holder.stdout.readline(), b"locked\n")
        start = time.monotonic()
        c = self.client()
        c.hello()
        self.assertEqual(holder.wait(1), -signal.SIGKILL)
        self.assertLess(time.monotonic() - start, 8)
        c.close()

    def test_stale_lock_file_is_ignored(self):
        # A pid that is not running, in a lock file nobody holds.
        with open(os.path.join(self.state, "call.lock"), "w") as f:
            f.write("999999")
        start = time.monotonic()
        c = self.client()
        c.hello()
        self.assertLess(time.monotonic() - start, 2.5)
        self.assertNotIn(b"ending the previous call", c.stderr_now())
        c.close()

    def test_separate_state_dirs_run_side_by_side(self):
        a = self.client()
        other = tempfile.mkdtemp(prefix="se30state")
        self.addCleanup(shutil.rmtree, other, True)
        b = Client("--demo", "--projects", self.root, state_dir=other)
        self.addCleanup(b.reap)
        a.hello()
        b.hello()
        self.assertIsNone(a.p.poll())
        self.assertIsNone(b.p.poll())
        a.close()
        b.close()


class SignalTest(Base):
    def test_sigterm_during_permission_request_stops_cleanly(self):
        c = self.client(env={"SE30_DEMO_START_DELAY": "0.2"})
        c.hello()
        c.open(1, "alpha")
        c.wait_line("1>")
        c.send("1P hello")
        c.wait_for(lambda l: l.startswith("1? "), 20)
        kids = children(c.p.pid)
        self.assertEqual(len(kids), 1)
        c.p.send_signal(signal.SIGTERM)
        self.assertEqual(c.p.wait(8), 0)
        self.assertTrue(wait_gone(kids[0], 3))

    def test_sighup_ends_tty_call(self):
        c = self.client()
        c.wait_for(lambda l: "Type /help" in l or "Ready." in l)
        c.p.send_signal(signal.SIGHUP)
        self.assertEqual(c.p.wait(8), 0)

    def test_sigkill_leaves_no_claude_process(self):
        c = self.client(env={"SE30_DEMO_START_DELAY": "0.2"})
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        c.wait_line("2>")
        kids = children(c.p.pid)
        self.assertEqual(len(kids), 2)
        c.p.kill()
        c.p.wait()
        for k in kids:
            self.assertTrue(wait_gone(k, 5), f"child {k} survived")


class HeartbeatTest(Base):
    def test_ping_gets_uncounted_pong(self):
        c = self.client()
        start = c.hello() + 1
        ws = c.ws
        for _ in range(10):
            c.send("!PING")
        c.wait_for(lambda l: c.text()[start:].count("#PONG") == 10, 5, start)
        self.assertEqual(c.ws, ws)
        c.close()

    def test_pong_while_waiting_for_ack(self):
        c = self.client("--window", "12", "--models", "opus", auto_ack=False)
        c.hello()
        c.open(1, "alpha")
        c.wait_line("1>")
        c.send("1P hello")
        w = c.wait_line("W")
        time.sleep(0.3)
        c.send("!PING")
        c.wait_line("#PONG", 3, w)
        c.send("!ACK")
        c.close()

    def test_silent_app_is_dropped_after_idle_timeout(self):
        c = self.client("--idle-timeout", "2", env={"SE30_DEMO_START_DELAY": "0.2"})
        c.hello()
        c.open(1, "alpha")
        c.wait_line("1>")
        kids = children(c.p.pid)
        c.send("!PING")
        c.wait_line("#PONG")
        start = time.monotonic()
        self.assertEqual(c.p.wait(6), 0)       # pipes still open: only the timeout ends it
        self.assertGreater(time.monotonic() - start, 1.5)
        self.assertTrue(wait_gone(kids[0], 3))

    def test_app_that_never_pings_is_not_timed_out(self):
        c = self.client("--idle-timeout", "1")
        c.hello()
        time.sleep(2.5)
        self.assertEqual(c.open(1, "alpha"), "1O alpha")
        c.close()

    def test_any_input_keeps_the_call_alive(self):
        c = self.client("--idle-timeout", "2")
        c.hello()
        c.send("!PING")
        for _ in range(5):
            time.sleep(0.8)
            c.send("!LIST")
        self.assertIsNone(c.p.poll())
        self.assertEqual(c.p.wait(6), 0)


if __name__ == "__main__":
    unittest.main()
