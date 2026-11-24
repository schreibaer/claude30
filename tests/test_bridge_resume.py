"""Resuming conversations in a new call: resume tokens, reopening a session
with its token, the cut-off note, and what ends a token. Runs bridge.py
--demo over pipes; bridges in one test share a state folder, as calls on
one host do. Run with:
    python3 -m unittest discover -s tests
"""
import json
import os
import re
import shutil
import signal
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from v2client import Client  # noqa: E402

TOKEN = re.compile(r"^[0-9a-f]{16}$")
CUT_NOTE = "The link to the SE/30 dropped"


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for name in ("alpha", "beta", "two words"):
            os.mkdir(os.path.join(self.root, name))
        self.state = tempfile.mkdtemp(prefix="se30state")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(shutil.rmtree, self.state, True)

    def client(self, *args, **kw):
        c = Client("--demo", "--projects", self.root, *args, state_dir=self.state,
                   env={"SE30_EVICT_WAIT": "3"}, **kw)
        self.addCleanup(c.reap)
        c.hello()
        return c

    def token_line(self, c, sid, start=0):
        """The payload of the next <sid>Y line: (token, how)."""
        i = c.wait_for(lambda l: l.startswith(f"{sid}Y "), 10, start)
        _, tok, how = c.text()[i].split(" ")
        return tok, how, i

    def open_new(self, c, sid, name):
        start = len(c.lines)
        self.assertEqual(c.open(sid, name), f"{sid}O {name}")
        tok, how, _ = self.token_line(c, sid, start)
        c.wait_line(f"{sid}>", start=start)
        self.assertRegex(tok, TOKEN)
        self.assertEqual(how, "new")
        return tok

    def resume(self, c, sid, tok, name):
        """Sends <sid>Y and returns (token, how) once the session is ready."""
        start = len(c.lines)
        c.send(f"{sid}Y {tok} {name}")
        c.wait_line(f"{sid}O {name}", start=start)
        tok2, how, i = self.token_line(c, sid, start)
        c.wait_line(f"{sid}>", start=i)
        return tok2, how

    def said(self, c, sid, start):
        """The demo's echo of the prompt, which may span several paragraphs,
        up to the turn's first tool line."""
        i = c.wait_for(lambda l: l.startswith(f"{sid}T You said:"), 10, start)
        j = c.wait_for(lambda l: l.startswith(f"{sid}U "), 10, i)
        return " ".join(l[3:] for l in c.text()[i:j] if l[:2] in (f"{sid}T", f"{sid}+"))

    def records(self):
        with open(os.path.join(self.state, "sessions.json")) as f:
            return json.load(f)


class TokenTest(Base):
    def test_open_sends_token_before_ready(self):
        c = self.client()
        ok = [l for l in c.text() if l.startswith("#OK")][0]
        self.assertIn(" resume", ok)
        self.assertIn(" ping", ok)
        start = len(c.lines)
        c.open(1, "alpha")
        c.wait_line("1>", start=start)
        tags = [l for l in c.text()[start:] if l[:1] == "1"]
        self.assertEqual([t[:2] for t in tags], ["1O", "1Y", "1>"])
        c.close()

    def test_resume_continues_turn_count_in_new_call(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        start = len(c.lines)
        c.turn(1, "two")
        self.assertIn("turn 2", self.said(c, 1, start))
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "resumed"))
        start = len(c.lines)
        c.turn(1, "three")
        said = self.said(c, 1, start)
        self.assertIn("turn 3", said)
        self.assertNotIn(CUT_NOTE, said)
        c.close()

    def test_project_name_with_spaces(self):
        c = self.client()
        tok = self.open_new(c, 2, "two words")
        c.turn(2, "hi")
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 2, tok, "two words"), (tok, "resumed"))
        c.close()

    def test_unopened_conversation_resumes_quietly(self):
        # No turn ran, so there is nothing to lose and nothing to resume.
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "resumed"))
        c.close()

    def test_bad_tokens_open_fresh_conversations(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "hi")
        c.close()
        c = self.client()
        for sid, t, name in ((2, tok, "beta"),               # another project's token
                             (3, "0123456789abcdef", "alpha"),  # unknown
                             (4, "NOT-A-TOKEN", "alpha")):     # malformed
            new, how = self.resume(c, sid, t, name)
            self.assertEqual(how, "new", (sid, t))
            self.assertRegex(new, TOKEN)
            self.assertNotEqual(new, tok)
            start = len(c.lines)
            c.turn(sid, "x")
            self.assertIn("turn 1", self.said(c, sid, start))
        # The original is still there for its own project.
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "resumed"))
        c.close()

    def test_resume_refused_like_open(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.send(f"1Y {tok} alpha")
        c.wait_line("1X already open", start=start)
        c.send(f"2Y {tok} nowhere")
        c.wait_line("2X not available", start=start)
        c.close()

    def test_new_and_edits_change_the_token(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "hi")
        start = len(c.lines)
        c.send("1P /new")
        tok2, how, _ = self.token_line(c, 1, start)
        self.assertEqual(how, "new")
        self.assertNotEqual(tok2, tok)
        c.wait_line("1>", start=start)
        start = len(c.lines)
        c.send("1P /edits on")
        tok3, how, _ = self.token_line(c, 1, start)
        self.assertEqual(how, "new")
        self.assertNotIn(tok, self.records())
        self.assertNotIn(tok2, self.records())
        c.wait_line("1>", start=start)
        c.turn(1, "x")
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha")[1], "new")
        start = len(c.lines)
        self.assertEqual(self.resume(c, 2, tok3, "alpha"), (tok3, "resumed"))
        c.turn(2, "y")
        self.assertIn("edits auto", self.said(c, 2, start))
        c.close()

    def test_close_and_quit_forget_the_token(self):
        c = self.client()
        tok1 = self.open_new(c, 1, "alpha")
        tok2 = self.open_new(c, 2, "beta")
        start = len(c.lines)
        c.send("1X")
        c.wait_line("1X closed", start=start)
        c.send("2P /quit")
        c.wait_line("2X closed", start=start)
        self.assertEqual(self.records(), {})
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok1, "alpha")[1], "new")
        self.assertEqual(self.resume(c, 2, tok2, "beta")[1], "new")
        c.close()

    def test_state_file_is_private_and_prunes_old_records(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.close()
        path = os.path.join(self.state, "sessions.json")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        recs = self.records()
        recs[tok]["used"] -= 31 * 86400
        with open(path, "w") as f:
            json.dump(recs, f)
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha")[1], "new")
        c.close()


class CutOffTest(Base):
    def cut(self, how_to_end):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "first")
        if how_to_end == "evict":
            # Stuck mid-turn on an unanswered W, as after a Wi-Fi drop.
            c.auto_ack = False
            start = len(c.lines)
            c.send("1P second")
            c.wait_line("W", start=start)
            c2 = self.client()
            self.assertEqual(c.p.wait(8), 0)
            return tok, c2
        # Waiting on a permission answer when the line closes.
        c.send("1P second")
        c.wait_for(lambda l: l.startswith("1? "), 20)
        c.close()
        return tok, self.client()

    def check_cut(self, tok, c):
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "cut"))
        start = len(c.lines)
        c.turn(1, "third")
        said = self.said(c, 1, start)
        self.assertIn(CUT_NOTE, said)
        self.assertIn("turn 3", said)
        # Only once: the turn after that is ordinary.
        start = len(c.lines)
        c.turn(1, "fourth")
        self.assertNotIn(CUT_NOTE, self.said(c, 1, start))
        c.close()
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "resumed"))
        c.close()

    def test_cut_after_stdin_closes_mid_turn(self):
        self.check_cut(*self.cut("close"))

    def test_cut_after_eviction_mid_turn(self):
        self.check_cut(*self.cut("evict"))

    def test_cut_after_sigterm_mid_turn(self):
        c = self.client()
        tok = self.open_new(c, 1, "alpha")
        c.send("1P first")
        c.wait_for(lambda l: l.startswith("1? "), 20)
        c.p.send_signal(signal.SIGTERM)
        self.assertEqual(c.p.wait(8), 0)
        c = self.client()
        self.assertEqual(self.resume(c, 1, tok, "alpha"), (tok, "cut"))
        c.close()


class AlwaysTest(Base):
    def test_always_survives_resume(self):
        c = self.client("--window", "0")
        tok = self.open_new(c, 1, "alpha")
        c.turn(1, "one", answer="a")
        c.close()
        c = self.client("--window", "0")
        self.resume(c, 1, tok, "alpha")
        start = len(c.lines)
        c.turn(1, "two", answer="n")
        self.assertFalse([l for l in c.text()[start:] if l.startswith("1? ")])
        c.close()


if __name__ == "__main__":
    unittest.main()
