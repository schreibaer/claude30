"""Protocol version 2 against bridge.py --demo: sessions, project list, flow
control, closing, and v1 and tty compatibility. Run with:
    python3 -m unittest discover -s tests
Standard library only; the bridge runs as a subprocess over pipes.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from v2client import BRIDGE, Client  # noqa: E402


def children(pid):
    """Process ids whose parent is pid (Linux /proc, or ps elsewhere)."""
    out = subprocess.run(["ps", "-o", "pid=", "--ppid", str(pid)],
                         capture_output=True, text=True).stdout
    return [int(x) for x in out.split()]


class Projects:
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for name in ("alpha", "beta", "gamma", ".hidden"):
            os.mkdir(os.path.join(self.root, name))
        open(os.path.join(self.root, "notes.txt"), "w").close()

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def client(self, *args, **kw):
        c = Client("--demo", "--projects", self.root, *args, **kw)
        self.addCleanup(lambda: c.p.poll() is None and c.p.kill())
        return c


class HandshakeTest(Projects, unittest.TestCase):
    def test_hello_gets_ok_and_list(self):
        c = self.client()
        c.hello()
        self.assertIn("#OK se30bridge 2 files=rw resume ping models agents chat stream md", c.text())
        self.assertEqual(c.projects(), ["alpha", "beta", "gamma"])
        c.close()

    def test_only_projects_root_listed(self):
        # With only --projects, the bridge's own folder is not offered.
        c = self.client()
        c.hello()
        self.assertNotIn(os.path.basename(os.getcwd()), c.projects())
        c.close()

    def test_cwd_joins_the_list(self):
        extra = tempfile.mkdtemp(prefix="zeta")
        self.addCleanup(shutil.rmtree, extra, True)
        c = self.client("--cwd", extra)
        c.hello()
        self.assertIn(os.path.basename(extra), c.projects())
        c.close()

    def test_list_counts_toward_window(self):
        # The model list (five lines) comes first, so a few more projects
        # than before still fit in the second window.
        for i in range(2):
            os.mkdir(os.path.join(self.root, f"p{i}"))
        c = self.client("--window", "8", auto_ack=False)
        c.send(b"")
        c.send("#HELLO SE30 2")
        c.wait_line("W")
        time.sleep(0.3)
        self.assertNotIn("P]", c.text())         # stopped until !ACK
        c.send("!ACK")
        c.wait_line("P]")
        c.auto_ack = True
        c.close()

    def test_list_again(self):
        c = self.client()
        end = c.hello()
        os.mkdir(os.path.join(self.root, "delta"))
        c.send("!LIST")
        self.assertIn("delta", c.projects(start=end + 1))
        c.close()

    def test_slow_default_start_does_not_delay_v2(self):
        c = self.client(env={"SE30_DEMO_START_DELAY": "3"})
        t = time.monotonic()
        c.hello()
        self.assertLess(time.monotonic() - t, 2.5)
        time.sleep(0.3)
        self.assertEqual(children(c.p.pid), [])   # the default session's child is gone
        c.close()


class SessionsTest(Projects, unittest.TestCase):
    def test_two_sessions_route_their_lines(self):
        c = self.client()
        c.hello()
        self.assertEqual(c.open(1, "alpha"), "1O alpha")
        self.assertEqual(c.open(2, "beta"), "2O beta")
        c.wait_line("1>")
        c.wait_line("2>")
        start = len(c.lines)
        c.send("1P hello one")
        c.send("2P hello two")
        c.wait_line("1? 1", start=start, timeout=20)
        c.wait_line("2? 1", start=start, timeout=20)
        c.send("1A 1 y")
        c.send("2A 1 y")
        c.wait_line("1R success $0.0000", start=start, timeout=20)
        c.wait_line("2R success $0.0000", start=start, timeout=20)
        one = c.session(1, start)
        two = c.session(2, start)
        self.assertTrue(any("demo mode in alpha" in l for l in one))
        self.assertTrue(any("demo mode in beta" in l for l in two))
        self.assertFalse(any("beta" in l for l in one))
        # Every line after the list is link-level W or carries a session digit.
        for line in c.text()[start:]:
            self.assertTrue(line == "W" or line[:1] in "12", line)
        c.close()

    def test_permission_wait_does_not_block_other_session(self):
        c = self.client()
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        start = len(c.lines)
        c.send("2P go")
        q = c.wait_for(lambda l: l.startswith("2? "), 20, start)
        c.send("1P go")
        i = c.wait_for(lambda l: l.startswith("1? "), 20, q)
        c.send(f"1A {c.text()[i].split()[1]} y")
        c.wait_line("1>", 20, i)
        self.assertNotIn("2R success $0.0000", c.text()[start:])
        c.send(f"2A {c.text()[q].split()[1]} n")
        c.wait_line("2R success $0.0000", 20, q)
        c.close()

    def test_one_w_per_window_across_sessions(self):
        c = self.client("--window", "5", auto_ack=False)
        c.send(b"")
        c.send("#HELLO SE30 2")
        c.send("1O alpha")
        c.send("2O beta")
        c.send("1P x")
        c.send("2P y")
        prev = c.wait_for(lambda l: l.startswith("#OK"))
        seen_sessions = set()
        for _ in range(12):
            w = c.wait_for(lambda l: l == "W", 20, prev + 1)
            time.sleep(0.3)
            self.assertEqual(len(c.lines), w + 1)      # nothing more until !ACK
            between = c.text()[prev + 1:w]
            counted = [l for l in between if l not in ("1>", "2>")]
            self.assertEqual(len(counted), 5, between)
            seen_sessions |= {l[0] for l in counted if l[:1] in "12"}
            prev = w
            c.send("!ACK")
        self.assertEqual(seen_sessions, {"1", "2"})
        c.auto_ack = True
        c.send("!ACK")
        c.close()

    def test_refused_opens(self):
        c = self.client("--max-sessions", "2")
        c.hello()
        self.assertEqual(c.open(1, "../etc"), "1X not available")
        self.assertEqual(c.open(1, "nothere"), "1X not available")
        self.assertEqual(c.open(1, ".hidden"), "1X not available")
        self.assertEqual(c.open(1, "notes.txt"), "1X not available")
        self.assertEqual(c.open(1, "alpha"), "1O alpha")
        self.assertEqual(c.open(1, "beta"), "1X already open")
        self.assertEqual(c.open(2, "beta"), "2O beta")
        self.assertEqual(c.open(3, "gamma"), "3X too many tabs")
        c.close()

    def test_new_is_per_session(self):
        c = self.client()
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        start = len(c.lines)
        c.turn(1, "/new")
        self.assertIn("1I New conversation.", c.text()[start:])
        self.assertFalse(any(l.startswith("2I") for l in c.text()[start:]))
        c.close()

    def test_edits_is_per_session(self):
        c = self.client()
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        c.turn(1, "/edits on")
        c.turn(2, "/new")
        start = len(c.lines)
        c.turn(2, "hi", answer="n")
        c.turn(1, "hi", answer="n")
        two = " ".join(c.session(2, start))
        one = " ".join(c.session(1, start))
        self.assertIn("edits ask", two)
        self.assertIn("edits auto", one)
        c.close()

    def test_close_during_turn_and_permission(self):
        c = self.client()
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        c.open(3, "gamma")
        c.send("1P go")
        c.wait_for(lambda l: l.startswith("1T Pager test line 3"), 20)
        c.send("1X")
        c.wait_line("1X closed", 20)
        start = len(c.lines)
        c.send("2P go")
        c.wait_for(lambda l: l.startswith("2? "), 20)
        c.send("2X")
        c.wait_line("2X closed", 20)
        # Session 3 still works, and session 1's number is free again.
        c.turn(3, "hi")
        self.assertIn("3R success $0.0000", c.text()[start:])
        self.assertEqual(c.open(1, "alpha"), "1O alpha")
        c.close()

    def test_multiline_prompt(self):
        c = self.client()
        c.hello()
        c.open(4, "alpha")
        c.wait_line("4>")
        start = len(c.lines)
        c.send("4P first\\")
        time.sleep(0.2)
        self.assertNotIn("4>", c.text()[start:])   # no ready marker per continuation
        c.send("4P second")
        c.wait_for(lambda l: l.startswith("4T You said:"), 10, start)
        said = [l for l in c.text()[start:] if l.startswith("4T You said:")][0]
        self.assertIn("first", said)
        c.close()

    def test_stdin_close_leaves_no_children(self):
        c = self.client(env={"SE30_DEMO_START_DELAY": "0.2"})
        c.hello()
        c.open(1, "alpha")
        c.open(2, "beta")
        time.sleep(0.2)
        self.assertEqual(len(children(c.p.pid)), 2)
        pid = c.p.pid
        c.close()
        out = subprocess.run(["ps", "-eo", "pid=,ppid=,args="], capture_output=True, text=True).stdout
        self.assertFalse([l for l in out.splitlines() if l.split()[1] == str(pid)])


class CompatibilityTest(Projects, unittest.TestCase):
    def test_v1_exchange_unchanged(self):
        c = self.client("--window", "0")
        c.send(b"")
        c.send("#HELLO SE30 1")
        c.wait_line("#OK se30bridge 1")
        i = c.wait_line(">")
        c.send("hi")
        j = c.wait_for(lambda l: l.startswith("? "), 20, i + 1)
        lines = c.text()[i + 1:j + 1]
        self.assertTrue(lines[0].startswith("T You said:"))
        self.assertIn("U Read src/parser.py", lines)
        self.assertIn("C[", lines)
        self.assertEqual(lines[-1], "? 1")
        self.assertFalse(any(l[:1].isdigit() for l in lines))
        c.send("A 1 y")
        c.wait_line("R success $0.0000", 20, j)
        c.close()

    def test_tty_echo_and_pager(self):
        c = self.client("--rows", "10")
        c.send("hi")
        end = time.monotonic() + 20
        while b"--More--" not in bytes(c.raw) and time.monotonic() < end:
            time.sleep(0.05)
        self.assertIn(b"--More--", bytes(c.raw))
        self.assertIn(b"> hi\r\n", bytes(c.raw))     # echoed as typed
        c.close()


if __name__ == "__main__":
    unittest.main()
