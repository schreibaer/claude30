"""Streamed answers: the chunker that turns one text block's deltas into the
T / + / C lines the app already draws, the session logic that decides per
block whether to stream, and the wire behavior against bridge.py --demo.
Run with:
    python3 -m unittest discover -s tests
"""
import asyncio
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge  # noqa: E402
from claude_agent_sdk import AssistantMessage, StreamEvent, TextBlock, ToolUseBlock  # noqa: E402
from test_bridge_agents import StubClient, done, sub_use  # noqa: E402
from v2client import Client  # noqa: E402


class Fence:
    _in_fence = False


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def render(pairs):
    """What the app draws for these lines: T starts a paragraph, + joins with
    one space (after C: with nothing), C lines and fences stand alone."""
    lines, last = [], None
    for t, p in pairs:
        if t == "+":
            lines[-1] += ("" if last == "C" else " ") + p
        elif t == "T":
            lines.append("T:" + p)
            last = "T"
        elif t == "C":
            lines.append("C:" + p)
            last = "C"
        else:
            lines.append(t)
            last = None
    return lines


SAMPLE = (
    "Intro line with **bold** words that goes on for a while so that the line is long "
    "enough to need several chunks when it is streamed word by word and then some more.\n\n"
    "- first item uses `code` inline\n  - nested item\n"
    "```python\ndef parse(s):\n    return s.split()\n```\n"
    "`parse()` reads tokens one at a time and keeps a small stack of open brackets, so it "
    "can report the line of an unmatched bracket in the text.\n"
    "  ```\nindented fence\n  ```\nlast line")


def chunk_all(text, size, clock=None):
    fence = Fence()
    ch = bridge.StreamChunker(fence, clock or Clock())
    out = []
    for i in range(0, len(text), size):
        out += ch.feed(text[i:i + size])
    out += ch.finish()
    return out


class ChunkerTests(unittest.TestCase):
    def test_pieces_render_like_the_whole_text_at_every_split(self):
        want = render(bridge.app_text_lines(SAMPLE, Fence()))
        for size in list(range(1, 9)) + [37, 120]:
            self.assertEqual(render(chunk_all(SAMPLE, size)), want, f"deltas of {size}")
        for i in range(len(SAMPLE) + 1):
            ch = bridge.StreamChunker(Fence(), Clock())
            out = ch.feed(SAMPLE[:i]) + ch.feed(SAMPLE[i:]) + ch.finish()
            self.assertEqual(render(out), want, f"split at {i}")

    def test_long_prose_goes_out_in_pieces_of_about_a_hundred_bytes(self):
        text = " ".join(f"word{i:03d}" for i in range(60))     # about 480 bytes
        out = chunk_all(text, 5)
        self.assertEqual(out[0][0], "T")
        self.assertTrue(all(t == "+" for t, _ in out[1:]))
        for _, p in out[:-1]:
            self.assertTrue(100 <= len(p) <= 200, len(p))
            self.assertFalse(p.startswith(" "))
            self.assertTrue(p.split()[-1].startswith("word") and len(p.split()[-1]) == 7)
        self.assertEqual(render(out), ["T:" + text])

    def test_inline_backticks_in_a_started_line_are_not_a_fence(self):
        ch = bridge.StreamChunker(Fence(), Clock())
        text = "some words here and there use ```code``` in the middle of a long prose line " * 3
        out = ch.feed(text.strip(), True) + ch.finish()
        self.assertTrue(all(t in ("T", "+") for t, _ in out), out)
        self.assertEqual(render(out), ["T:" + text.strip()])

    def test_a_pause_releases_whole_words(self):
        clock = Clock()
        ch = bridge.StreamChunker(Fence(), clock)
        self.assertEqual(ch.feed("alpha "), [])
        clock.t += 0.6
        self.assertEqual(ch.feed("be"), [("T", "alpha")])
        clock.t += 0.6
        self.assertEqual(ch.feed("t"), [])        # "bet" has no complete word yet
        self.assertEqual(ch.feed(" gamma"), [("+", "bet")])
        self.assertEqual(ch.finish(), [("+", "gamma")])

    def test_force_sends_every_complete_word_at_once(self):
        ch = bridge.StreamChunker(Fence(), Clock())
        self.assertEqual(ch.feed("one two thr", force=True), [("T", "one two")])

    def test_a_word_longer_than_a_line_is_cut_at_200(self):
        out = chunk_all("x" * 250 + " tail", 50)
        self.assertEqual([len(p) for _, p in out], [200, 50, 4])

    def test_fences_wait_for_their_newline(self):
        fence = Fence()
        ch = bridge.StreamChunker(fence, Clock())
        self.assertEqual(ch.feed("``"), [])
        self.assertEqual(ch.feed("`py"), [])
        self.assertEqual(ch.feed("\nx = 1 and a lot more text that stays until the end of the line, "
                                 "which is long enough to pass the hundred byte mark"),
                         [("C[", "")])
        self.assertTrue(fence._in_fence)
        self.assertEqual(ch.feed("\n``"), [("C", "x = 1 and a lot more text that stays until the end of "
                                                 "the line, which is long enough to pass the hundred byte mark")])
        self.assertEqual(ch.feed("`\n"), [("C]", "")])
        self.assertFalse(fence._in_fence)

    def test_a_paragraph_that_opens_with_inline_code_streams(self):
        text = ("`parse()` reads tokens one at a time and keeps a stack of open brackets so it "
                "can report where the matching bracket is missing in the input ")
        ch = bridge.StreamChunker(Fence(), Clock())
        out = ch.feed(text)
        self.assertEqual(out[0][0], "T")
        self.assertTrue(out[0][1].startswith("`parse()` reads"))

    def test_blank_lines_and_a_trailing_newline_match_the_whole_text(self):
        for text in ("a b\n\nc", "a b\n", "\n", "one"):
            self.assertEqual(render(chunk_all(text, 1)),
                             render(bridge.app_text_lines(text, Fence())), repr(text))


# ---- the session: which blocks stream, and never twice ----------------------

def se(event, parent=None):
    return StreamEvent(uuid="u", session_id="s", event=event, parent_tool_use_id=parent)


def block_events(text, idx=0, step=7, mid=None):
    """message_start, a text block's start, its deltas and its stop."""
    ev = [se({"type": "message_start", "message": {"id": "m1"}}),
          se({"type": "content_block_start", "index": idx, "content_block": {"type": "text", "text": ""}})]
    for i in range(0, len(text), step):
        ev.append(se({"type": "content_block_delta", "index": idx,
                      "delta": {"type": "text_delta", "text": text[i:i + step]}}))
    return ev


def stop(idx=0):
    return se({"type": "content_block_stop", "index": idx})


def asst(text):
    return AssistantMessage(content=[TextBlock(text=text)], model="m", message_id="m1")


class StreamTerm:
    cols = 80

    def __init__(self, stream):
        self.out = []
        self.on_interrupt = None
        self.stream = stream
        self._in_fence = False

    async def text(self, md):
        self.out.append(("text", md))

    async def stream_pieces(self, pairs):
        self.out.extend(("piece", t, p) for t, p in pairs)

    async def tool(self, name, summary):
        self.out.append(("tool", name, summary))

    async def tool_error(self, msg):
        self.out.append(("error", msg))

    async def result(self, status, cost, meter=None):
        self.out.append(("result", status))

    async def usage(self, meter):
        pass

    async def agent_start(self, *a):
        pass

    async def agent_step(self, *a):
        pass

    async def agent_end(self, *a):
        pass

    def pieces(self):
        return [(o[1], o[2]) for o in self.out if o[0] == "piece"]


def run(msgs, stream, actions=None):
    """Runs one turn over scripted messages. A callable in the script runs
    when reached; a coroutine it returns is awaited."""
    term = StreamTerm(stream)
    sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))

    async def go():
        sess.client = StubClient(msgs)
        sess._msgs = asyncio.Queue()
        sess._in_turn = False
        sess._pump = asyncio.ensure_future(sess._pump_messages())
        handle = sess._handle

        async def scripted(msg):
            if callable(msg) and not isinstance(msg, StreamEvent):
                r = msg(term, sess)
                if asyncio.iscoroutine(r):
                    await r
                return False
            return await handle(msg)
        sess._handle = scripted
        try:
            await sess.turn("hi")
        finally:
            await sess.stop()
    asyncio.run(go())
    return term


TEXT = ("The parser reads tokens one at a time and keeps a small stack of open brackets, so it can "
        "report the line of an unmatched bracket.\n\nA second paragraph follows here, a bit shorter.")


class SessionTests(unittest.TestCase):
    def test_streaming_on_sends_the_block_once_as_pieces(self):
        term = run(block_events(TEXT) + [asst(TEXT), stop(), done()], True)
        self.assertEqual([o[0] for o in term.out if o[0] != "piece"], ["result"])
        self.assertEqual(render(term.pieces()), render(bridge.app_text_lines(TEXT, Fence())))
        self.assertGreater(len([o for o in term.out if o[0] == "piece"]), 2)

    def test_turning_it_on_after_the_block_printed_does_not_repeat_it(self):
        def switch(term, sess):
            term.stream = True
            return sess.stream_changed()
        term = run([asst(TEXT), switch, stop(), done()], False)
        self.assertEqual([o for o in term.out if o[0] == "piece"], [])
        self.assertEqual([o for o in term.out if o[0] == "text"], [("text", TEXT)])

    def test_streaming_off_prints_the_block_whole_as_today(self):
        term = run(block_events(TEXT) + [asst(TEXT), stop(), done()], False)
        self.assertEqual(term.out, [("text", TEXT), ("result", "success")])

    def test_turning_it_on_midway_sends_the_text_so_far_at_once(self):
        seen = []

        def switch(term, sess):
            term.stream = True
            return sess.stream_changed()

        def snapshot(term, sess):
            seen.append(render(term.pieces()))
        half = block_events(TEXT)
        k = len(half) // 2
        term = run(half[:k] + [switch, snapshot] + half[k:] + [asst(TEXT), stop(), done()], False)
        self.assertTrue(seen[0], "text so far went out when the switch came")
        self.assertEqual(render(term.pieces()), render(bridge.app_text_lines(TEXT, Fence())))
        self.assertEqual([o for o in term.out if o[0] == "text"], [])

    def test_turning_it_off_midway_holds_the_rest_until_the_block_ends(self):
        seen = []

        def off(term, sess):
            term.stream = False
            seen.append(len(term.out))

        def before_end(term, sess):
            seen.append(len(term.out))
        long_text = " ".join(f"word{i:03d}" for i in range(80))
        ev = block_events(long_text, step=11)
        k = len(ev) // 2
        term = run(ev[:k] + [off] + ev[k:] + [before_end, asst(long_text), stop(), done()], True)
        self.assertGreater(seen[0], 0)
        self.assertEqual(seen[0], seen[1], "nothing went out between the switch and the block end")
        self.assertEqual(render(term.pieces()), ["T:" + long_text])
        self.assertEqual([o for o in term.out if o[0] == "text"], [])

    def test_the_next_block_after_switching_off_arrives_whole(self):
        second = "Second block, written after the switch went off."
        ev = block_events(TEXT) + [asst(TEXT), stop(), se({"type": "message_start", "message": {"id": "m2"}})]
        ev += block_events(second, idx=1)[1:] + [AssistantMessage(content=[TextBlock(text=second)],
                                                                   model="m", message_id="m2"), stop(1)]

        def off(term, sess):
            term.stream = False
        k = len(block_events(TEXT)) + 2
        term = run(ev[:k] + [off] + ev[k:] + [done()], True)
        self.assertEqual([o for o in term.out if o[0] == "text"], [("text", second)])

    def test_subagent_stream_events_send_nothing(self):
        ev = [se(e.event, parent="toolu_1") for e in block_events("I am thinking out loud for a while")]
        term = run(ev + [done()], True)
        self.assertEqual(term.out, [("result", "success")])

    def test_an_interrupt_flushes_the_streamed_text_before_the_result(self):
        text = "Half an answer that was cut off by the stop key"
        term = run(block_events(text) + [done(reason="aborted_streaming")], True)
        self.assertEqual(render(term.pieces()), ["T:" + text])
        self.assertEqual(term.out[-1], ("result", "interrupted"))

    def test_a_block_without_stream_events_prints_whole_even_when_on(self):
        term = run([asst("No events came for this one."), done()], True)
        self.assertEqual(term.out, [("text", "No events came for this one."), ("result", "success")])

    def test_a_turn_claude_starts_by_itself_streams_inside_one_turn(self):
        term = StreamTerm(True)
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))

        async def go():
            sess.client = StubClient(block_events(TEXT) + [asst(TEXT), stop(), done()])
            sess._msgs = asyncio.Queue()
            sess._in_turn = False
            sess._pump = asyncio.ensure_future(sess._pump_messages())
            await asyncio.sleep(0.05)
            turned = await sess.follow()
            await sess.stop()
            return turned
        self.assertTrue(asyncio.run(go()))
        self.assertEqual(render(term.pieces()), render(bridge.app_text_lines(TEXT, Fence())))
        self.assertEqual(term.out[-1], ("result", "success"))

    def test_tool_lines_and_text_keep_their_order(self):
        tool = AssistantMessage(content=[ToolUseBlock(id="t1", name="Read", input={"file_path": "/tmp/a.py"})],
                                model="m", message_id="m1")
        term = run(block_events("Looking now.") + [asst("Looking now."), stop(), tool, done()], True)
        kinds = [o[0] for o in term.out]
        self.assertEqual(kinds[-2:], ["tool", "result"])
        self.assertEqual(render(term.pieces()), ["T:Looking now."])


# ---- the channel: a + piece only continues its own paragraph ----------------

class FakeLink:
    def __init__(self):
        self.lines = []

    async def send_line(self, data, counted=True):
        self.lines.append(data.decode("latin-1"))


def bare_channel():
    ch = object.__new__(bridge.Channel)
    ch.sid = 1
    ch.term = FakeLink()
    ch.group = asyncio.Lock()
    ch._para_open = False
    return ch


class ChannelTests(unittest.TestCase):
    def test_a_line_between_two_pieces_starts_the_next_one_as_a_paragraph(self):
        async def go():
            ch = bare_channel()
            await ch.stream_pieces([("T", "first words"), ("+", "more words")])
            await ch.tag("J~", "1 3 Grep def parse")
            await ch.stream_pieces([("+", "after the line")])
            await ch.send_split("B", b"file line")
            await ch.stream_pieces([("+", "after a file line")])
            await ch.stream_pieces([("+", "same paragraph")])
            return ch.term.lines
        self.assertEqual(asyncio.run(go()), [
            "1T first words", "1+ more words", "1J~ 1 3 Grep def parse", "1T after the line",
            "1B file line", "1T after a file line", "1+ same paragraph"])

    def test_a_long_code_line_keeps_its_continuations_as_code(self):
        async def go():
            ch = bare_channel()
            await ch.stream_pieces([("C", "x" * 200), ("+", "y" * 50)])
            return ch.term.lines
        self.assertEqual(asyncio.run(go()), ["1C " + "x" * 200, "1+ " + "y" * 50])


# ---- over the wire, against the demo bridge ---------------------------------

class WireTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.mkdir(os.path.join(self.root, "alpha"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def client(self, *extra):
        c = Client("--demo", "--projects", self.root)
        self.addCleanup(c.reap)
        c.hello()
        return c

    def long_turn(self, c, sid, prompt="show a streaming answer"):
        start = len(c.lines)
        end = c.turn(sid, prompt)
        return c.session(sid, start)[: end - start]

    def rendered(self, lines):
        pairs = []
        for l in lines:
            tag, _, pay = l.partition(" ")
            if tag in ("T", "+", "C", "C[", "C]"):
                pairs.append((tag, pay))
        return render(pairs)

    def test_ok_advertises_stream(self):
        c = self.client()
        self.assertIn("#OK se30bridge 2 files=rw resume ping models agents chat stream md usage cmds", c.text())

    def test_streamed_text_is_chunked_and_equals_the_unstreamed_text(self):
        c = self.client()
        c.send("1V on")
        c.open(1, "alpha")
        c.wait_line("1>")
        on = self.long_turn(c, 1)
        c.send("2O alpha")
        c.wait_line("2>")
        off = self.long_turn(c, 2)
        plus_on = [l for l in on if l.startswith("+ ")]
        plus_off = [l for l in off if l.startswith("+ ")]
        self.assertGreater(len(plus_on), len(plus_off))
        self.assertTrue(all(len(l) - 2 <= 200 for l in plus_on))
        self.assertEqual(self.rendered(on), self.rendered(off))

    def test_two_tabs_stream_independently(self):
        c = self.client()
        c.open(1, "alpha")
        c.wait_line("1>")
        c.send("2O alpha")
        c.wait_line("2>")
        c.send("1V on")
        start = len(c.lines)
        c.send("1P show a streaming answer")
        c.send("2P show a streaming answer")
        c.wait_for(lambda l: l == "1>", start=start)
        c.wait_for(lambda l: l == "2>", start=start)
        one = [l for l in c.session(1, start) if l.startswith("+ ")]
        two = [l for l in c.session(2, start) if l.startswith("+ ")]
        self.assertGreater(len(one), len(two))

    def test_a_line_in_the_middle_reopens_the_paragraph(self):
        c = self.client()
        c.send("1V on")
        c.open(1, "alpha")
        c.wait_line("1>")
        got = self.long_turn(c, 1, "show a streaming answer with a tool")
        i = got.index("U Read src/parser.py")
        self.assertTrue(got[i - 1].startswith(("T ", "+ ")))
        self.assertTrue(got[i + 1].startswith("T "), got[i + 1])

    def test_unknown_values_and_unopened_sessions_do_nothing(self):
        c = self.client()
        c.send("1V maybe")
        c.send("3V on")
        c.open(1, "alpha")
        c.wait_line("1>")
        got = self.long_turn(c, 1)
        today = [(t + " " + p).rstrip() for t, p in bridge.app_text_lines(bridge.LONG_DEMO, Fence())]
        self.assertEqual([l for l in got if l.split(" ")[0] in ("T", "+", "C", "C[", "C]")], today,
                         "nothing streamed: exactly the lines of the whole block")
        c.open(3, "alpha")
        c.wait_line("3>")
        three = self.long_turn(c, 3)
        self.assertEqual(self.rendered(three), self.rendered(got))

    def test_the_setting_is_per_session_and_changes_between_turns(self):
        c = self.client()
        c.open(1, "alpha")
        c.wait_line("1>")
        before = self.long_turn(c, 1)
        c.send("1V on")
        after = self.long_turn(c, 1)
        self.assertGreater(len([l for l in after if l.startswith("+ ")]),
                           len([l for l in before if l.startswith("+ ")]))


if __name__ == "__main__":
    unittest.main()
