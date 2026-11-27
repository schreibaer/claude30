"""Markdown marks in prose: the md word, the app's !MD opt-in, and what
changes in the T, + and C lines after it. Run with:
    python3 -m unittest discover -s tests
"""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge  # noqa: E402
from test_bridge_stream import Clock, render  # noqa: E402
from v2client import Client  # noqa: E402


class Fence:
    _in_fence = False
    keep_marks = False


class KeepFence(Fence):
    keep_marks = True


def prose_lines(text, keep):
    return [(t + " " + p).rstrip() for t, p in
            bridge.app_text_lines(text, KeepFence() if keep else Fence())]


class OfferTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.mkdir(os.path.join(self.root, "alpha"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def client(self, *extra, md=False):
        c = Client("--demo", "--projects", self.root, *extra)
        self.addCleanup(c.reap)
        c.hello(md=md)
        return c

    def answer(self, c, prompt="show the styles"):
        c.open(1, "alpha")
        c.wait_line("1>")
        start = len(c.lines)
        end = c.turn(1, prompt)
        got = c.session(1, start)[: end - start]
        return [l for l in got if l.split(" ")[0] in ("T", "+", "C", "C[", "C]")]

    def test_ok_carries_md(self):
        c = self.client()
        self.assertIn("#OK se30bridge 2 files=rw resume ping models agents chat stream md usage cmds changes", c.text())

    def test_without_md_the_lines_are_todays(self):
        c = self.client()
        self.assertEqual(self.answer(c), prose_lines(bridge.STYLES_DEMO, False))

    def test_after_md_marks_stay_in_prose_and_code(self):
        c = self.client(md=True)
        got = self.answer(c)
        self.assertEqual(got, prose_lines(bridge.STYLES_DEMO, True))
        self.assertTrue(any("**bold**" in l for l in got))
        self.assertIn("C def __init__(self, **kwargs):", got)

    def test_md_changes_only_the_marks(self):
        kept = prose_lines(bridge.STYLES_DEMO, True)
        today = prose_lines(bridge.STYLES_DEMO, False)
        self.assertEqual(len(kept), len(today))
        for k, t in zip(kept, today):
            if k.startswith(("C ", "C[", "C]")):
                continue
            self.assertEqual(k.replace("**", "").replace("__", ""), t)

    def test_code_is_stripped_without_md(self):
        c = self.client()
        self.assertIn("C def init(self, kwargs):", self.answer(c))

    def test_md_twice_does_no_harm(self):
        c = self.client(md=True)
        c.send("!MD")
        self.assertEqual(self.answer(c), prose_lines(bridge.STYLES_DEMO, True))

    def test_streaming_with_md_renders_like_the_whole_answer(self):
        c = self.client(md=True)
        c.send("1V on")
        got = self.answer(c)
        pairs = [tuple(l.partition(" ")[::2]) for l in got]
        self.assertTrue(any(t == "+" for t, _ in pairs), "it streamed")
        self.assertEqual(render(pairs), render(bridge.app_text_lines(bridge.STYLES_DEMO, KeepFence())))

    def test_no_md_option_leaves_the_word_out_and_ignores_the_line(self):
        c = self.client("--no-md", md=True)
        self.assertIn("#OK se30bridge 2 files=rw resume ping models agents chat stream usage cmds changes", c.text())
        self.assertNotIn(" md", [l[-3:] for l in c.text() if l.startswith("#OK")][0])
        self.assertEqual(self.answer(c), prose_lines(bridge.STYLES_DEMO, False))


class PromptTests(unittest.TestCase):
    class Term:
        cols = 80

    class MdTerm(Term):
        keep_marks = True

    def test_plain_prompts_without_md(self):
        for p in (bridge.SYSTEM_APPEND, bridge.CHAT_PROMPT):
            text = p.format(cols=80, style=bridge.style_sentence(self.Term()))
            self.assertIn("Avoid tables", text)
            self.assertNotIn("**bold**", text)

    def test_md_prompts_name_the_styles(self):
        for p in (bridge.SYSTEM_APPEND, bridge.CHAT_PROMPT):
            text = p.format(cols=80, style=bridge.style_sentence(self.MdTerm()))
            self.assertIn("**bold**", text)
            self.assertIn("No strikethrough", text)
            self.assertNotIn("Avoid tables", text)
            self.assertTrue(text.isascii())


class ChunkerTests(unittest.TestCase):
    def test_marks_survive_every_split_of_the_deltas(self):
        text = "A **bold phrase** and __this__ with `code` and a line\n```\nx = __y__\n```\ndone"
        want = render(bridge.app_text_lines(text, KeepFence()))
        for i in range(len(text) + 1):
            ch = bridge.StreamChunker(KeepFence(), Clock())
            out = ch.feed(text[:i]) + ch.feed(text[i:]) + ch.finish()
            self.assertEqual(render(out), want, i)
        self.assertIn("T:A **bold phrase** and __this__ with `code` and a line", want)
        self.assertIn("C:x = __y__", want)


if __name__ == "__main__":
    unittest.main()
