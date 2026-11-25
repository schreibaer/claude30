"""Model and effort per session: the model list in the handshake, presets
before a tab opens, changes between turns, typed /model and /effort, and
the fallbacks. Runs bridge.py --demo over pipes (the demo names a model or
effort in its reply only when it differs from the default), so these catch
protocol and lifecycle bugs but not whether the real SDK honours an effort
on resume. Run with:
    python3 -m unittest discover -s tests
"""
import json
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from v2client import Client  # noqa: E402


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for name in ("alpha", "beta"):
            os.mkdir(os.path.join(self.root, name))
        self.state = tempfile.mkdtemp(prefix="se30state")
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(shutil.rmtree, self.state, True)

    def client(self, *args, **kw):
        c = Client("--demo", "--projects", self.root, *args, state_dir=self.state, **kw)
        self.addCleanup(c.reap)
        c.hello()
        return c

    def said(self, c, sid, start):
        """The demo's echo of the prompt, up to the turn's first tool line."""
        i = c.wait_for(lambda l: l.startswith(f"{sid}T You said:"), 10, start)
        j = c.wait_for(lambda l: l.startswith(f"{sid}U "), 10, i)
        return " ".join(l[3:] for l in c.text()[i:j] if l[:2] in (f"{sid}T", f"{sid}+"))

    def ready(self, c, sid, name="alpha"):
        start = len(c.lines)
        c.open(sid, name)
        c.wait_line(f"{sid}>", start=start)
        return start

    def ask(self, c, sid, prompt="hi"):
        start = len(c.lines)
        c.turn(sid, prompt)
        return self.said(c, sid, start)


class HandshakeTest(Base):
    def test_models_in_handshake(self):
        c = self.client()
        text = c.text()
        self.assertIn("models", [l for l in text if l.startswith("#OK")][0].split())
        i = text.index("M[")
        self.assertEqual([l for l in text[i:text.index("P[")] if l != "W"],
                         ["M[", "M opus", "M sonnet", "M haiku", "M] - -"])
        c.close()

    def test_models_flag_and_bridge_default(self):
        c = self.client("--models", "a,b", "c", "--model", "sonnet")
        text = c.text()
        i = text.index("M[")
        self.assertEqual([l for l in text[i:text.index("P[")] if l != "W"],
                         ["M[", "M a", "M b", "M c", "M] sonnet -"])
        c.close()


class PresetTest(Base):
    def test_preset_applies_at_open(self):
        c = self.client()
        c.send("2M sonnet high")
        start = self.ready(c, 2)
        tags = [l[:2] for l in c.text()[start:] if l[:1] == "2"]
        self.assertEqual(tags, ["2O", "2Y", "2M", "2>"])
        self.assertIn("2M sonnet high", c.text())
        said = self.ask(c, 2)
        self.assertIn("model sonnet", said)
        self.assertIn("effort high", said)
        c.close()

    def test_no_preset_names_nothing(self):
        c = self.client()
        start = self.ready(c, 1)
        self.assertIn("1M - -", c.text()[start:])
        said = self.ask(c, 1)
        self.assertNotIn("model", said)
        self.assertNotIn("effort", said)
        c.close()

    def test_preset_is_for_its_own_number_only(self):
        c = self.client()
        c.send("3M haiku low")
        self.ready(c, 4)
        self.assertNotIn("model haiku", self.ask(c, 4))
        c.close()

    def test_unknown_preset_falls_back_with_a_note(self):
        c = self.client()
        c.send("1M gpt9 turbo")
        start = self.ready(c, 1)
        mine = c.session(1, start)
        self.assertTrue(any(l.startswith("E Unknown model gpt9") for l in mine), mine)
        self.assertTrue(any(l.startswith("E Unknown effort turbo") for l in mine), mine)
        self.assertIn("M - -", mine)
        c.close()

    def test_preset_dropped_when_the_connection_ends(self):
        c = self.client()
        c.send("1M sonnet high")
        c.close()
        c2 = self.client()
        self.ready(c2, 1)
        self.assertNotIn("model sonnet", self.ask(c2, 1))
        c2.close()


class ChangeTest(Base):
    def test_model_change_while_ready(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1M haiku -")
        c.wait_line("1M haiku -", start=start)
        c.wait_line("1>", start=start + 1)
        self.assertIn("model haiku", self.ask(c, 1))
        c.close()

    def test_effort_change_restarts_and_resumes(self):
        c = self.client()
        self.ready(c, 1)
        c.turn(1, "one")
        with open(os.path.join(self.state, "sessions.json")) as f:
            before = json.load(f)
        start = len(c.lines)
        c.send("1M - max")
        c.wait_line("1M - max", start=start)
        c.wait_line("1>", start=start + 1)
        self.assertFalse([l for l in c.text()[start:] if l.startswith("1Y ")])
        said = self.ask(c, 1, "two")
        self.assertIn("effort max", said)
        self.assertIn("turn 2", said)
        with open(os.path.join(self.state, "sessions.json")) as f:
            self.assertEqual(json.load(f).keys(), before.keys())
        c.close()

    def test_change_during_a_turn_waits(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1P long task")
        c.wait_for(lambda l: l.startswith("1? "), 20, start)
        c.send("1M - high")
        i = c.wait_line("1M - high", start=start)
        note = c.text()[i - 1]
        self.assertTrue(note.startswith("1I "), note)
        # The running turn finishes normally; no second M follows.
        c.send(f"1A {c.text()[c.wait_for(lambda l: l.startswith('1? '), 5, start)].split()[1]} y")
        c.wait_line("1>", 20, i)
        self.assertEqual([l for l in c.text()[start:] if l.startswith("1M ")], ["1M - high"])
        self.assertIn("effort high", self.ask(c, 1))
        c.close()

    def test_unknown_model_change_falls_back(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1M gpt9 -")
        c.wait_line("1M - -", start=start)
        self.assertTrue([l for l in c.text()[start:] if l.startswith("1E Unknown model gpt9")])
        c.wait_line("1>", start=start + 1)
        self.assertNotIn("model", self.ask(c, 1))
        c.close()

    def test_invalid_effort_changes_effort_only(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1M sonnet turbo")
        c.wait_line("1M sonnet -", start=start)
        self.assertTrue([l for l in c.text()[start:] if l.startswith("1E Unknown effort")])
        c.wait_line("1>", start=start + 1)
        said = self.ask(c, 1)
        self.assertIn("model sonnet", said)
        self.assertNotIn("effort", said)
        c.close()

    def test_two_sessions_stay_independent(self):
        c = self.client()
        c.send("1M haiku -")
        self.ready(c, 1)
        self.ready(c, 2, "beta")
        self.assertIn("model haiku", self.ask(c, 1))
        self.assertNotIn("model", self.ask(c, 2))
        c.send("2M sonnet low")
        c.wait_line("2M sonnet low")
        time.sleep(0.3)
        self.assertIn("effort low", self.ask(c, 2))
        self.assertNotIn("effort", self.ask(c, 1))
        c.close()


class TypedCommandTest(Base):
    def test_model_alone_lists_choices(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1P /model")
        c.wait_for(lambda l: l.startswith("1I Choices: opus sonnet haiku"), 10, start)
        self.assertIn("1I Model: default (the CLI default).", c.text())
        c.close()

    def test_typed_model_sends_m_and_applies(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1P /model sonnet")
        c.wait_line("1M sonnet -", start=start)
        c.wait_line("1>", start=start + 1)
        self.assertIn("model sonnet", self.ask(c, 1))
        c.send("1P /effort high")
        c.wait_line("1M sonnet high")
        c.wait_for(lambda l: l == "1I Effort set.")
        said = self.ask(c, 1)
        self.assertIn("model sonnet", said)
        self.assertIn("effort high", said)
        c.close()

    def test_typed_unknown_model_changes_nothing(self):
        c = self.client()
        self.ready(c, 1)
        start = len(c.lines)
        c.send("1P /model gpt9")
        c.wait_for(lambda l: l.startswith("1I Unknown model gpt9."), 10, start)
        self.assertFalse([l for l in c.text()[start:] if l.startswith("1M ")])
        c.close()

    def test_tty_mode_commands(self):
        c = Client("--demo", "--rows", "1000", state_dir=self.state)
        self.addCleanup(c.reap)
        c.wait_for(lambda l: "Ready." in l or "Type /help" in l)
        c.send("hello")
        end = time.monotonic() + 30
        while b"Allow?" not in c.raw and time.monotonic() < end:
            time.sleep(0.1)    # the prompt has no line end, so wait on raw bytes
        self.assertIn(b"Allow?", c.raw)
        c.send("a")
        c.wait_for(lambda l: l.startswith("(success"), 20)
        c.send("/effort high")
        c.wait_for(lambda l: l == "Effort set.", 10)
        c.send("again")
        c.wait_for(lambda l: "effort high" in l and "turn 2" in l, 20)
        c.close()


if __name__ == "__main__":
    unittest.main()
