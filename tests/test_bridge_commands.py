"""Claude Code commands and plan mode (PROTOCOL.md, "Commands and plan mode"):
which typed lines pass through, how the allowlist is built, and the wire
behavior against bridge.py --demo. Run with:
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
from test_bridge_resume import Base  # noqa: E402

ALLOWED = {"compact", "hello"}
KNOWN = ["clear", "compact", "config", "hello", "permissions", "mcp"]


class Holder:
    pass


class RouteTests(unittest.TestCase):
    def route(self, text):
        return bridge.route_prompt(text, ALLOWED, KNOWN)

    def test_allowed_commands_go_as_they_are(self):
        self.assertEqual(self.route("/compact"), ("command", "/compact"))
        self.assertEqual(self.route("/compact keep the tests"), ("command", "/compact keep the tests"))
        self.assertEqual(self.route("/hello"), ("command", "/hello"))

    def test_text_that_starts_with_a_slash_is_a_prompt_with_a_leading_space(self):
        self.assertEqual(self.route("/usr/bin is missing"), ("send", " /usr/bin is missing"))
        self.assertEqual(self.route("hello /compact"), ("send", "hello /compact"))
        self.assertEqual(self.route("/"), ("send", "/"))

    def test_built_ins_that_change_the_host_are_refused(self):
        for name in ("permissions", "add-dir", "hooks", "mcp", "memory", "config", "login", "resume"):
            kind, text = self.route(f"/{name}")
            self.assertEqual(kind, "refuse", name)
            self.assertIn(f"/{name} is not available", text)

    def test_a_listed_command_that_is_not_allowed_is_refused(self):
        self.assertEqual(self.route("/config")[0], "refuse")

    def test_bridge_words(self):
        self.assertEqual(self.route("/clear"), ("clear", ""))
        self.assertEqual(self.route("/commands"), ("commands", ""))
        self.assertEqual(self.route("/plan ON"), ("plan", "on"))
        self.assertEqual(self.route("/plan"), ("plan", ""))


class AllowlistTests(unittest.TestCase):
    def test_the_list_is_compact_command_files_skills_and_plugin_commands(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        os.makedirs(os.path.join(d, ".claude", "commands", "sub"))
        for rel in ("hello.md", os.path.join("sub", "deep.md")):
            open(os.path.join(d, ".claude", "commands", rel), "w").close()
        s = Holder()
        names = ["compact", "clear", "config", "hello", "sub:deep", "debug", "plug:thing", "mcp"]
        bridge.set_session_commands(s, names, ["debug"], d, terminal=["config"])
        self.assertEqual(s.allowed_commands,
                         {"compact", "hello", "sub:deep", "debug", "plug:thing"})
        self.assertIn("mcp", s.known_commands)

    def test_a_command_file_the_cli_does_not_list_is_not_allowed(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        os.makedirs(os.path.join(d, ".claude", "commands"))
        open(os.path.join(d, ".claude", "commands", "ghost.md"), "w").close()
        s = Holder()
        bridge.set_session_commands(s, ["compact"], None, d)
        self.assertNotIn("ghost", s.allowed_commands)


class WireTests(Base):
    def cmds_client(self):
        c = self.client()
        c.send("!CMDS")
        c.send("!USAGE")
        return c

    def test_ok_has_the_word_and_no_at_lines_without_the_opt_in(self):
        c = self.client()
        self.assertTrue([l for l in c.text() if l.startswith("#OK")][0].endswith(" cmds"))
        self.open_new(c, 1, "alpha")
        c.send("1P /plan on")
        c.wait_for(lambda l: l == "1I Plan mode on.")
        c.wait_line("1>", start=len(c.lines) - 3)
        c.close()
        self.assertFalse(any(l.startswith("1@") for l in c.text()))

    def test_plan_mode_switch_confirmation_and_exit(self):
        c = self.cmds_client()
        self.open_new(c, 1, "alpha")
        self.assertIn("1@ default", c.text())
        start = len(c.lines)
        c.send("1@ plan")
        c.wait_line("1@ plan", start=start)
        i = c.turn(1, "add a test")       # approves the plan
        lines = c.text()[start:i + 1]
        self.assertTrue(any(l.startswith("1Q ") and "ExitPlanMode" in l for l in lines))
        self.assertIn("1@ default", lines)
        c.close()

    def test_plan_left_with_n_stays_in_plan_mode(self):
        c = self.cmds_client()
        self.open_new(c, 1, "alpha")
        c.send("1@ plan")
        c.wait_line("1@ plan")
        start = len(c.lines)
        c.turn(1, "add a test", answer="n")
        self.assertNotIn("1@ default", c.text()[start:])
        c.close()

    def test_new_and_clear_leave_plan_mode(self):
        for word in ("/new", "/clear"):
            c = self.cmds_client()
            self.open_new(c, 1, "alpha")
            c.send("1@ plan")
            c.wait_line("1@ plan")
            start = len(c.lines)
            c.send(f"1P {word}")
            c.wait_line("1@ default", start=start)
            c.close()

    def test_plan_survives_a_resume(self):
        c = self.cmds_client()
        tok = self.open_new(c, 1, "alpha")
        c.send("1@ plan")
        c.wait_line("1@ plan")
        c.close()
        c = self.cmds_client()
        start = len(c.lines)
        self.resume(c, 1, tok, "alpha")
        self.assertIn("1@ plan", c.text()[start:])
        c.close()

    def test_a_chat_refuses_plan_mode(self):
        c = self.cmds_client()
        c.send("1O *")
        c.wait_line("1>")
        c.send("1@ plan")
        c.wait_for(lambda l: l == "1I Plan mode does not apply in a chat.")
        c.close()

    def test_compact_reports_and_lowers_the_context(self):
        c = self.cmds_client()
        self.open_new(c, 1, "alpha")
        c.turn(1, "one")
        c.turn(1, "two")
        start = len(c.lines)
        i = c.turn(1, "/compact")
        lines = c.text()[start:i + 1]
        self.assertIn("1I Conversation compacted.", lines)
        self.assertTrue(any(l.startswith("1$ ") for l in lines))
        c.close()

    def test_a_slash_path_is_a_prompt_and_a_builtin_is_refused(self):
        c = self.cmds_client()
        self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.turn(1, "/usr/bin is missing")
        self.assertIn("You said: “ /usr/bin is missing”", " ".join(c.text()[start:]))
        start = len(c.lines)
        c.send("1P /permissions")
        c.wait_for(lambda l: l.startswith("1I /permissions is not available"), start=start)
        c.close()

    def test_commands_lists_the_bridge_and_claude_code_commands(self):
        c = self.cmds_client()
        self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.send("1P /commands")
        c.wait_for(lambda l: l.startswith("1I Bridge: "), start=start)
        c.wait_for(lambda l: l.startswith("1I Claude Code:") and "/compact" in l, start=start)
        c.close()


if __name__ == "__main__":
    unittest.main()
