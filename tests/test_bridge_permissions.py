"""Permission prompts in app mode when two tool calls ask at the same time.

The Agent SDK runs each can_use_tool request as its own task, so two tool
calls in one message can ask for permission concurrently. Run with:
    python3 -m unittest discover -s tests
Needs only the standard library: claude_agent_sdk is stubbed.
"""
import asyncio
import os
import sys
import types
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import bridge  # noqa: E402


def _stub_sdk():
    """Minimal stand-ins for the SDK types _can_use_tool returns."""
    if "claude_agent_sdk.types" in sys.modules:
        return
    sdk = types.ModuleType("claude_agent_sdk")
    t = types.ModuleType("claude_agent_sdk.types")

    class Allow:
        def __init__(self, updated_input=None):
            self.updated_input = updated_input

    class Deny:
        def __init__(self, message="", interrupt=False):
            self.message = message

    t.PermissionResultAllow = Allow
    t.PermissionResultDeny = Deny
    sdk.types = t
    sys.modules.setdefault("claude_agent_sdk", sdk)
    sys.modules["claude_agent_sdk.types"] = t


class FakeLink:
    def __init__(self):
        self.out = []

    async def send(self, data):
        self.out.append(data.decode("mac_roman"))

    def lines(self):
        return [l for l in "".join(self.out).split("\r\n") if l]


def make_session():
    _stub_sdk()
    args = types.SimpleNamespace(cols=80, rows=24, charset="macroman", window=0,
                                 cwd="/tmp/project", accept_edits=False, model=None,
                                 idle_timeout=0)
    link = FakeLink()
    term = bridge.Terminal(link, args)
    term.mode = "app"
    return bridge.ClaudeSession(term, args), term, link


class ConcurrentPermissionTest(unittest.TestCase):
    def test_second_request_waits_for_first_answer(self):
        async def run():
            sess, term, link = make_session()
            a = asyncio.ensure_future(sess._can_use_tool("Bash", {"command": "git status"}, None))
            b = asyncio.ensure_future(sess._can_use_tool("Bash", {"command": "git diff"}, None))
            await asyncio.sleep(0.05)
            # Only the first request is on the wire.
            self.assertEqual(link.lines(), ["Q 1 Bash git status", "? 1"])
            await term.lines.put("A 1 y")
            await asyncio.sleep(0.05)
            self.assertTrue(a.done())
            self.assertFalse(b.done())
            self.assertEqual(link.lines()[2:], ["Q 2 Bash git diff", "? 2"])
            await term.lines.put("A 2 n")
            await asyncio.wait_for(b, 1)
            self.assertIsInstance(a.result(), sys.modules["claude_agent_sdk.types"].PermissionResultAllow)
            self.assertIsInstance(b.result(), sys.modules["claude_agent_sdk.types"].PermissionResultDeny)
        asyncio.run(run())

    def test_always_covers_a_waiting_request(self):
        async def run():
            sess, term, link = make_session()
            a = asyncio.ensure_future(sess._can_use_tool("Write", {"file_path": "/tmp/project/a"}, None))
            b = asyncio.ensure_future(sess._can_use_tool("Write", {"file_path": "/tmp/project/b"}, None))
            await asyncio.sleep(0.05)
            await term.lines.put("A 1 a")
            await asyncio.wait_for(asyncio.gather(a, b), 1)
            # The second Write was allowed by "always" without a second prompt.
            self.assertEqual([l for l in link.lines() if l.startswith("Q")],
                             ["Q 1 Write a (0 lines)"])
        asyncio.run(run())


class ChannelPermissionTest(unittest.TestCase):
    """The same one-at-a-time rule for a protocol version 2 session."""

    def test_second_request_waits_in_a_channel(self):
        async def run():
            _stub_sdk()
            args = types.SimpleNamespace(cols=80, rows=24, charset="macroman", window=0,
                                         accept_edits=False, model=None, files="off",
                                         idle_timeout=0)
            link = FakeLink()
            term = bridge.Terminal(link, args)
            term.mode = "app2"
            ch = bridge.Channel(bridge.V2(term, args), 3, "project", "/tmp/project")
            sess = bridge.ClaudeSession(ch, ch.cfg)
            a = asyncio.ensure_future(sess._can_use_tool("Bash", {"command": "git status"}, None))
            b = asyncio.ensure_future(sess._can_use_tool("Bash", {"command": "git diff"}, None))
            await asyncio.sleep(0.05)
            self.assertEqual(link.lines(), ["3Q 1 Bash git status", "3? 1"])
            await ch.answers.put("1 y")
            await asyncio.sleep(0.05)
            self.assertTrue(a.done())
            self.assertEqual(link.lines()[2:], ["3Q 2 Bash git diff", "3? 2"])
            await ch.answers.put("2 n")
            await asyncio.wait_for(b, 1)
        asyncio.run(run())


if __name__ == "__main__":
    unittest.main()
