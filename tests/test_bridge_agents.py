"""Subagents: what the bridge forwards while Claude runs one, and what the
SE/30 is told. The tracker tests feed ClaudeSession.turn a scripted SDK
message stream through a stub client; the wire tests run bridge.py --demo.
Run with:
    python3 -m unittest discover -s tests
"""
import asyncio
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge  # noqa: E402
from claude_agent_sdk import (AssistantMessage, ResultMessage, SystemMessage,  # noqa: E402
                              TextBlock, ToolResultBlock, ToolUseBlock, UserMessage)


class FakeTerm:
    """Records the semantic output calls in order."""
    cols = 80

    def __init__(self):
        self.out = []
        self.on_interrupt = None

    async def text(self, md):
        self.out.append(("text", md))

    async def tool(self, name, summary):
        self.out.append(("tool", name, summary))

    async def tool_error(self, msg):
        self.out.append(("error", msg))

    async def result(self, status, cost):
        self.out.append(("result", status))

    async def agent_start(self, k, typ, task):
        self.out.append(("a+", k, typ, task))

    async def agent_step(self, k, steps, tool, summary):
        self.out.append(("a~", k, steps, tool, summary))

    async def agent_end(self, k, typ, status, steps, summary):
        self.out.append(("a-", k, typ, status, steps, summary))


class StubClient:
    """Yields a scripted stream, then stays open like the CLI does. A
    callable in the script runs when the session handles it, so a test can
    act at a given point (advance the clock, raise)."""
    def __init__(self, msgs):
        self.msgs = list(msgs)
        self.more = asyncio.Queue()    # messages to send later, between turns
        self.queries = []

    async def query(self, prompt):
        self.queries.append(prompt)

    async def disconnect(self):
        pass

    async def receive_messages(self):
        for m in self.msgs:
            yield m
        while True:
            yield await self.more.get()


def attach(sess, client):
    """What ClaudeSession.start does, with the stub instead of the CLI.
    Call inside the event loop."""
    sess.client = client
    sess._msgs = asyncio.Queue()
    sess._in_turn = False
    sess._pump = asyncio.ensure_future(sess._pump_messages())
    handle = sess._handle

    async def scripted(msg):
        if callable(msg):
            msg()
            return False
        return await handle(msg)
    sess._handle = scripted


def agent_use(tid, typ="Explore", desc="Find parser tests"):
    return AssistantMessage(content=[ToolUseBlock(id=tid, name="Agent",
                            input={"subagent_type": typ, "description": desc, "prompt": "x"})],
                            model="m")


def sub_use(parent, name="Grep", inp=None):
    return AssistantMessage(content=[ToolUseBlock(id="t" + name, name=name,
                            input=inp or {"pattern": "parse"})], model="m", parent_tool_use_id=parent)


def sub_text(parent):
    return AssistantMessage(content=[TextBlock(text="I am thinking out loud")], model="m",
                            parent_tool_use_id=parent)


def sub_result(parent, err=True):
    return UserMessage(content=[ToolResultBlock(tool_use_id="tGrep", content="boom", is_error=err)],
                       parent_tool_use_id=parent)


def top_result(tid, text="Tests are in tests/test_parser.py\nand more", err=False):
    return UserMessage(content=[ToolResultBlock(tool_use_id=tid,
                       content=[{"type": "text", "text": text}], is_error=err)])


def sysmsg(subtype, **data):
    return SystemMessage(subtype=subtype, data=data)


def done(sub="success", reason=None):
    return ResultMessage(subtype=sub, duration_ms=1, duration_api_ms=1, is_error=False,
                         num_turns=1, session_id="s", total_cost_usd=0.0,
                         terminal_reason=reason)


def run_turn(msgs):
    term = FakeTerm()
    cfg = bridge.SessionConfig("/tmp")
    sess = bridge.ClaudeSession(term, cfg)
    sess.agents.clock = Clock()

    async def go():
        attach(sess, StubClient(msgs))
        try:
            await sess.turn("hi")
        finally:
            await sess.stop()
    asyncio.run(go())
    return term.out, sess


class Clock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class TrackerTests(unittest.TestCase):
    def test_one_subagent_gives_one_start_and_one_end(self):
        out, _ = run_turn([
            agent_use("a1"),
            sysmsg("task_started", task_id="T1", tool_use_id="a1", description="d"),
            sub_use("a1", "Grep"), sub_text("a1"), sub_use("a1", "Read", {"file_path": "/tmp/x.py"}),
            sub_result("a1", err=True),
            top_result("a1"),
            AssistantMessage(content=[TextBlock(text="The tests are in test_parser.py")], model="m"),
            done(),
        ])
        self.assertEqual(out[0], ("a+", 1, "Explore", "Find parser tests"))
        ends = [o for o in out if o[0] == "a-"]
        self.assertEqual(ends, [("a-", 1, "Explore", "done", 2, "Tests are in tests/test_parser.py")])
        self.assertFalse([o for o in out if o[0] in ("error", "tool")])
        self.assertNotIn("thinking out loud", repr(out))
        self.assertEqual(out[-2], ("text", "The tests are in test_parser.py"))
        self.assertEqual(out[-1], ("result", "success"))

    def test_error_result_fails_the_line(self):
        out, _ = run_turn([agent_use("a1"), top_result("a1", "Agent crashed\nstack", err=True), done()])
        self.assertIn(("a-", 1, "Explore", "failed", 0, "Agent crashed"), out)

    def test_error_result_after_interrupt_is_stopped(self):
        # The interrupt arrives mid-stream, after the turn started.
        term = FakeTerm()
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))

        async def flow():
            attach(sess, StubClient([
                agent_use("a1"),
                lambda: setattr(sess.agents, "interrupted", True),   # what interrupt() sets
                top_result("a1", "The user doesn't want to proceed", err=True),
                done("success", "aborted")]))
            await sess.turn("hi")
            await sess.stop()
        asyncio.run(flow())
        ends = [o for o in term.out if o[0] == "a-"]
        self.assertEqual(ends[0][3], "stopped")
        self.assertIn(("result", "interrupted"), term.out)

    def test_notification_ends_the_line_and_the_later_result_is_hidden(self):
        out, _ = run_turn([
            agent_use("a1"),
            sysmsg("task_notification", task_id="T1", tool_use_id="a1", status="completed",
                   summary="Found it in tests/test_parser.py"),
            top_result("a1", "[Subagent hand-back] long text", err=True),
            done(),
        ])
        ends = [o for o in out if o[0] == "a-"]
        self.assertEqual(ends, [("a-", 1, "Explore", "done", 0, "Found it in tests/test_parser.py")])
        self.assertFalse([o for o in out if o[0] == "error"])

    def test_two_subagents_end_in_reverse_order(self):
        out, _ = run_turn([
            agent_use("a1", "Explore", "first"), agent_use("a2", "Plan", "second"),
            sub_use("a1"), sub_use("a2"),
            top_result("a2", "second done"), top_result("a1", "first done"), done(),
        ])
        self.assertEqual([o for o in out if o[0] == "a+"],
                         [("a+", 1, "Explore", "first"), ("a+", 2, "Plan", "second")])
        self.assertEqual([o[:4] + o[5:] for o in out if o[0] == "a-"],
                         [("a-", 2, "Plan", "done", "second done"),
                          ("a-", 1, "Explore", "done", "first done")])

    def test_exception_mid_subagent_ends_it_stopped(self):
        def boom():
            raise RuntimeError("link gone")
        term = FakeTerm()
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))

        async def go():
            attach(sess, StubClient([agent_use("a1"), boom]))
            try:
                await sess.turn("hi")
            finally:
                await sess.stop()
        with self.assertRaises(RuntimeError):
            asyncio.run(go())
        self.assertIn(("a-", 1, "Explore", "stopped", 0, "Find parser tests"), term.out)

    def test_interrupted_result_message_closes_open_subagents(self):
        out, _ = run_turn([agent_use("a1"), done("success", "aborted")])
        self.assertIn(("a-", 1, "Explore", "stopped", 0, "Find parser tests"), out)
        self.assertEqual(out[-1], ("result", "interrupted"))

    def test_messages_for_unknown_parent_are_hidden(self):
        out, _ = run_turn([sub_text("zzz"), sub_use("zzz"), sub_result("zzz"), done()])
        self.assertEqual(out, [("result", "success")])

    def test_main_conversation_is_unchanged(self):
        out, _ = run_turn([
            AssistantMessage(content=[TextBlock(text="Looking."),
                                      ToolUseBlock(id="r1", name="Read",
                                                   input={"file_path": "/tmp/a.py"})], model="m"),
            UserMessage(content=[ToolResultBlock(tool_use_id="r1", content="nope\nmore",
                                                 is_error=True)]),
            done(),
        ])
        self.assertEqual(out, [("text", "Looking."), ("tool", "Read", "a.py"),
                               ("error", "nope"), ("result", "success")])

    def test_progress_is_throttled_and_only_on_change(self):
        term = FakeTerm()
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))
        clock = sess.agents.clock = Clock()

        async def flow():
            await sess.agents.start("a1", {"subagent_type": "Explore", "description": "d"})
            for i in range(12):                # one step every 0.5 s for 6 s
                clock.t += 0.5
                await sess.agents.step("a1", "Grep", f"p{i}")
            clock.t += 5
            await sess.agents.step("a1", steps=10)   # the CLI's own count, nothing new: no line
            await sess.agents.step("a1", steps=12)   # a higher count after 5 s: a line
            clock.t += 5
            await sess.agents.step("a1", "Read", "x.py")
            await sess.agents.step("a1", "Read", "y.py")   # same tool, 0 s later: held back
        asyncio.run(flow())
        steps = [o for o in term.out if o[0] == "a~"]
        self.assertEqual([(o[2], o[3]) for o in steps], [(10, "Grep"), (12, "Grep"), (13, "Read")])

    def test_slots_are_reused(self):
        term = FakeTerm()
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))

        async def flow():
            await sess.agents.start("a1", {"description": "one"})
            await sess.agents.end("a1", "done", "ok")
            await sess.agents.start("a2", {"description": "two"})
        asyncio.run(flow())
        self.assertEqual([o[1] for o in term.out if o[0] == "a+"], [1, 1])

    def test_label_for(self):
        term = FakeTerm()
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))
        ag = sess.agents

        async def flow():
            self.assertEqual(ag.label_for(None), "")
            await ag.start("a1", {"subagent_type": "Explore", "description": "d"})
            self.assertEqual(ag.label_for(None), "")
            self.assertEqual(ag.label_for("X"), "Explore")
            ag.bind("a1", "T1")
            self.assertEqual(ag.label_for("T1"), "Explore")
            await ag.start("a2", {"subagent_type": "Plan", "description": "d"})
            self.assertEqual(ag.label_for("T1"), "Explore")
            self.assertEqual(ag.label_for("unmapped"), "subagent")
        asyncio.run(flow())

    def test_init_message_records_agent_names(self):
        _, sess = run_turn([sysmsg("init", agents=["Explore", "Plan"]), done()])
        self.assertEqual(sess.init_agents, ["Explore", "Plan"])

    def test_long_task_and_summary_are_clipped_for_plain_lines(self):
        class T(FakeTerm):
            pass
        term = bridge.Channel.__new__(bridge.Channel)
        sent = []

        async def tool(name, summary):
            sent.append((name, summary))
        term.tool = tool
        term.v2 = type("V", (), {"agents_optin": False})()
        asyncio.run(bridge.Channel.agent_start(term, 1, "Explore", "x" * 300))
        self.assertEqual(len(sent[0][1]), len("Explore: ") + 120)


# ---- over the wire, against the demo bridge ---------------------------------

import re  # noqa: E402
import shutil  # noqa: E402
import tempfile  # noqa: E402

from v2client import Client  # noqa: E402


class BackgroundTests(unittest.TestCase):
    """An Agent call with run_in_background: its tool result only confirms
    the launch, the turn ends, and the subagent's end arrives between turns,
    followed by a turn Claude starts by itself."""

    def bg_use(self, tid="b1"):
        return AssistantMessage(content=[ToolUseBlock(id=tid, name="Agent", input={
            "subagent_type": "Explore", "description": "Survey tests", "prompt": "x",
            "run_in_background": True})], model="m")

    def later(self):
        return [sub_use("b1"), sub_use("b1", "Read", {"file_path": "/tmp/a.py"}),
                sysmsg("task_notification", task_id="T1", tool_use_id="b1",
                       status="completed", summary="Found 12 test files"),
                AssistantMessage(content=[TextBlock(text="The survey is done.")], model="m"),
                done()]

    def start(self, term):
        sess = bridge.ClaudeSession(term, bridge.SessionConfig("/tmp"))
        stub = StubClient([self.bg_use(), top_result("b1", "Async agent launched successfully."),
                           AssistantMessage(content=[TextBlock(text="Started.")], model="m"),
                           done()])
        attach(sess, stub)
        return sess, stub

    def test_launch_closes_the_line_and_the_end_comes_with_a_new_one(self):
        term = FakeTerm()
        woken = []
        term.wake = lambda: woken.append(1)

        async def flow():
            sess, stub = self.start(term)
            await sess.turn("go")
            first = list(term.out)
            for m in self.later():
                stub.more.put_nowait(m)
            for _ in range(20):
                await asyncio.sleep(0)
            self.assertTrue(woken)
            self.assertTrue(await sess.follow())
            self.assertFalse(await sess.follow())    # nothing left
            await sess.stop()
            return first, term.out[len(first):]
        first, after = asyncio.run(flow())
        self.assertEqual(first, [
            ("a+", 1, "Explore", "Survey tests"),
            ("a-", 1, "Explore", "background", 0, "runs on, its result follows"),
            ("text", "Started."), ("result", "success")])
        self.assertEqual(after, [
            ("a+", 1, "Explore", "Survey tests"),
            ("a-", 1, "Explore", "done", 2, "Found 12 test files"),
            ("text", "The survey is done."), ("result", "success")])

    def test_progress_alone_between_turns_is_not_a_turn(self):
        term = FakeTerm()

        async def flow():
            sess, stub = self.start(term)
            await sess.turn("go")
            n = len(term.out)
            stub.more.put_nowait(sub_use("b1"))
            for _ in range(20):
                await asyncio.sleep(0)
            turned = await sess.follow()
            await sess.stop()
            return turned, term.out[n:]
        turned, after = asyncio.run(flow())
        self.assertFalse(turned)
        self.assertEqual(after, [])

    def test_unread_output_is_shown_before_the_next_turn(self):
        # tty and v1 have no wake: the next prompt shows it first, and that
        # prompt's turn still ends at its own result.
        term = FakeTerm()

        async def flow():
            sess, stub = self.start(term)
            await sess.turn("go")
            n = len(term.out)
            for m in self.later():
                stub.more.put_nowait(m)
            for _ in range(20):
                await asyncio.sleep(0)
            stub.more.put_nowait(AssistantMessage(content=[TextBlock(text="Reply.")], model="m"))
            stub.more.put_nowait(done())
            await sess.turn("next")
            await sess.stop()
            return term.out[n:]
        after = asyncio.run(flow())
        self.assertEqual([o[0] for o in after], ["a+", "a-", "text", "result", "text", "result"])
        self.assertEqual(after[4], ("text", "Reply."))

    def test_turn_end_leaves_a_background_subagent_running(self):
        out, sess = run_turn([self.bg_use(), top_result("b1", "launched"), done()])
        self.assertEqual([o for o in out if o[0] == "a-"],
                         [("a-", 1, "Explore", "background", 0, "runs on, its result follows")])
        self.assertIn("b1", sess.agents.background)


class WireTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        os.mkdir(os.path.join(self.root, "alpha"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def client(self, agents):
        c = Client("--demo", "--projects", self.root)
        self.addCleanup(c.reap)
        c.hello(agents=agents)
        c.open(1, "alpha")
        c.wait_line("1>")
        return c

    def agents_turn(self, c, answer="y"):
        start = len(c.lines)
        end = c.turn(1, "run the agents demo", answer)
        return c.session(1, start)[: end - start], start

    def test_ok_advertises_agents(self):
        c = self.client(False)
        self.assertIn("#OK se30bridge 2 files=rw resume ping models agents chat", c.text())

    def test_background_result_arrives_after_the_turn(self):
        c = self.client(True)
        end = c.turn(1, "run in the background please")
        c.wait_for(lambda l: l == "1>", timeout=10, start=end + 1)
        got = c.session(1, end + 1)
        self.assertEqual([l for l in got if l[:1] in "JTR>"][:5], [
            "J+ 1 Explore Survey the tests", "J- 1 done 3 Found 12 test files",
            "T The background subagent is done: there are 12 test files.",
            "R success $0.0000", ">"])

    def test_background_result_without_optin_is_plain_lines(self):
        c = self.client(False)
        end = c.turn(1, "run in the background please")
        before = c.session(1, 0)
        self.assertIn("U Agent Explore background: runs on, its result follows", before)
        c.wait_for(lambda l: l == "1>", timeout=10, start=end + 1)
        got = c.session(1, end + 1)
        self.assertEqual(got[:2], ["U Agent Explore: Survey the tests",
                                   "U Agent Explore done: Found 12 test files"])

    def test_without_optin_only_plain_lines(self):
        c = self.client(False)
        lines, _ = self.agents_turn(c)
        self.assertFalse([l for l in lines if l.startswith("J")])
        tools = [l for l in lines if l.startswith("U Agent")]
        self.assertEqual(tools[0], "U Agent Explore: Find where the parser is tested")
        self.assertEqual(tools[1], "U Agent general-purpose: Check the docs for the parser")
        self.assertIn("U Agent general-purpose done: The parser is documented in docs/parser.md", tools)
        self.assertIn("U Agent Explore done: Tests are in tests/test_parser.py", tools)

    def test_with_optin_j_lines_and_labelled_permission(self):
        c = self.client(True)
        lines, _ = self.agents_turn(c)
        j = [l for l in lines if l.startswith("J")]
        self.assertEqual(j[0], "J+ 1 Explore Find where the parser is tested")
        self.assertEqual(j[1], "J+ 2 general-purpose Check the docs for the parser")
        self.assertTrue(any(l.startswith("J~ 1 ") for l in j))
        self.assertTrue(any(l.startswith("J~ 2 ") for l in j))
        self.assertIn("J- 2 done 2 The parser is documented in docs/parser.md", j)
        self.assertIn("J- 1 done 3 Tests are in tests/test_parser.py", j)
        self.assertFalse([l for l in lines if l.startswith("U Agent")])
        q = [l for l in lines if l.startswith("Q ")]
        self.assertEqual(q, ["Q 1 Bash (general-purpose) git log -5"])
        # the ends come before the turn's R line and the ready line
        self.assertLess(max(i for i, l in enumerate(lines) if l.startswith("J-")),
                        next(i for i, l in enumerate(lines) if l.startswith("R ")))

    def test_slot_numbers_are_reused(self):
        c = self.client(True)
        self.agents_turn(c)
        lines, _ = self.agents_turn(c)
        self.assertEqual([l for l in lines if l.startswith("J+")][0][:4], "J+ 1")

    def test_stop_ends_open_subagents_as_stopped(self):
        c = self.client(True)
        start = len(c.lines)
        c.send("1P run the agents demo")
        c.wait_for(lambda l: l.startswith("1J~ "), start=start)
        c.send("1!STOP")
        c.wait_line("1>", start=start)
        lines = c.session(1, start)
        ends = [l for l in lines if l.startswith("J-")]
        self.assertEqual(len(ends), 2)
        self.assertTrue(all(" stopped " in l for l in ends))
        self.assertTrue([l for l in lines if l.startswith("R interrupted")])

    def test_throttle_limits_progress_lines(self):
        c = self.client(True)
        lines, _ = self.agents_turn(c)
        steps = [l for l in lines if l.startswith("J~")]
        self.assertLessEqual(len(steps), 5)
        for l in steps:
            self.assertRegex(l, r"^J~ [12] \d+ \S+ ")

    def test_tty_mode_prints_agent_lines(self):
        c = Client("--demo", "--projects", self.root)
        self.addCleanup(c.reap)
        # tty mode has no HELLO: type a prompt at the demo bridge
        c.send(b"")
        time.sleep(0.5)
        c.send("agents")
        c.wait_for(lambda l: "[Agent] Explore: Find where the parser is tested" in l, timeout=15)
        c.wait_for(lambda l: "(general-purpose) git log -5" in l, timeout=15)
        c.send("y")
        c.wait_for(lambda l: "[Agent] Explore done: Tests are in tests/test_parser.py" in l,
                   timeout=25)


class AgentsCommandTests(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.proj = os.path.join(self.root, "alpha")
        self.adir = os.path.join(self.proj, ".claude", "agents")
        os.makedirs(self.adir)
        self.addCleanup(shutil.rmtree, self.root, True)
        home = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, home, True)
        old = os.environ.get("HOME")
        os.environ["HOME"] = home
        self.addCleanup(lambda: os.environ.__setitem__("HOME", old) if old else None)

    def write(self, name, text, d=None):
        with open(os.path.join(d or self.adir, name), "w") as f:
            f.write(text)

    def test_project_agent_is_listed_with_its_description(self):
        self.write("reviewer.md", "---\nname: reviewer\ndescription: Reviews diffs for bugs\n---\nbody\n")
        out = bridge.agents_lines(self.proj, ["Explore", "Plan"])
        self.assertIn("reviewer (project): Reviews diffs for bugs", out)
        self.assertIn("Explore (built in)", out)
        self.assertEqual(out[-1], "New or changed agent files apply after /new.")

    def test_missing_name_and_description(self):
        self.write("helper.md", "---\ndescription: Helps\n---\n")
        self.write("quiet.md", "---\nname: quiet\n---\n")
        out = bridge.agents_lines(self.proj, ["x"])
        self.assertIn("helper (project): Helps", out)
        self.assertIn("quiet (project): (no description)", out)

    def test_broken_frontmatter_does_not_stop_the_list(self):
        self.write("broken.md", "no frontmatter at all\n")
        self.write("ok.md", "---\nname: ok\ndescription: fine\n---\n")
        out = bridge.agents_lines(self.proj, ["x"])
        self.assertIn("broken (project): (unreadable)", out)
        self.assertIn("ok (project): fine", out)

    def test_symlink_out_of_the_folder_is_skipped(self):
        outside = os.path.join(self.root, "secret.md")
        with open(outside, "w") as f:
            f.write("---\nname: secret\ndescription: leaked\n---\n")
        os.symlink(outside, os.path.join(self.adir, "secret.md"))
        out = " ".join(bridge.agents_lines(self.proj, ["x"]))
        self.assertNotIn("secret", out)

    def test_init_names_defined_by_a_file_appear_once(self):
        self.write("reviewer.md", "---\nname: reviewer\ndescription: d\n---\n")
        out = bridge.agents_lines(self.proj, ["reviewer", "Explore"])
        self.assertEqual(len([l for l in out if l.startswith("reviewer")]), 1)
        self.assertIn("reviewer (project): d", out)

    def test_before_any_reply_it_says_built_ins_come_later(self):
        out = bridge.agents_lines(self.proj, [])
        self.assertIn("Built-in agents are listed after the first reply.", out)

    def test_user_agents_are_listed(self):
        udir = os.path.join(os.environ["HOME"], ".claude", "agents")
        os.makedirs(udir)
        self.write("mine.md", "---\nname: mine\ndescription: personal\n---\n", udir)
        self.assertIn("mine (user): personal", bridge.agents_lines(self.proj, ["x"]))

    def test_command_over_v2_and_tty(self):
        self.write("reviewer.md", "---\nname: reviewer\ndescription: Reviews diffs for bugs\n---\n")
        c = Client("--demo", "--projects", self.root)
        self.addCleanup(c.reap)
        c.hello()
        c.open(1, "alpha")
        c.wait_line("1>")
        start = len(c.lines)
        c.send("1P /agents")
        c.wait_for(lambda l: l.startswith("1I New or changed"), start=start)
        got = c.session(1, start)
        self.assertIn("I reviewer (project): Reviews diffs for bugs", got)
        self.assertIn("I Explore (built in)", got)
        self.assertTrue(all(l.startswith("I ") for l in got if l[:1] != ">"))
        # the same command in tty mode
        t = Client("--demo", "--projects", self.root)
        self.addCleanup(t.reap)
        t.send(b"")
        time.sleep(0.5)
        t.send("/agents")
        t.wait_for(lambda l: "New or changed agent files apply after /new." in l)


import time  # noqa: E402

if __name__ == "__main__":
    unittest.main()
