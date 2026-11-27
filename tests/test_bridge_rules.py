"""Permanent allow rules (PROTOCOL.md, "Rules (rules)"): which rules may be
saved, how an answer is bound to its request, the Q+ line and r answers on
the wire, and /allow. Run with:
    python3 -m unittest discover -s tests
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bridge  # noqa: E402
from test_bridge_resume import Base  # noqa: E402

ALLOWED = ["Read", "Bash(git status:*)", "Edit(src/**)", "WebFetch(domain:example.com)",
           "mcp__github__get_issue", "Glob", "Bash(npm test:*)", "Bash(npm test -- --watch=false)",
           "Write(docs/*.md)", "Bash(echo (x):*)"]
REFUSED = ["Bash", "Bash(*)", "Bash(:*)", "Bash(sh:*)", "Bash(/bin/sh:*)", "Bash(FOO=1 sh:*)",
           "Bash(npm test; rm -rf /:*)", "Bash(a | b:*)", "Bash(a `b`:*)", "Bash(a $(b):*)",
           "Bash(env X:*)", "Bash(python3:*)", "Edit(/etc/**)", "Edit(../x)", "Edit(.env)",
           "Edit(.claude/**)", "Edit(package.json)", "Edit(.github/workflows/x.yml)",
           "Edit(~/x)", "Edit", "Write", "WebFetch", "Agent", "ExitPlanMode",
           "ExitPlanMode(x)", "mcp__github", "Edit(*)", "Edit(**)", "Edit(/**)",
           "Bash(" + "a" * 300 + ")", "bad rule", "Bash(x", "Bash()", "Edit(src/é)"]


class ValidateTests(unittest.TestCase):
    def test_allowed_rules_pass(self):
        for r in ALLOWED:
            self.assertIsNone(bridge.validate_rule(r), r)

    def test_each_refusal_class_has_a_reason(self):
        for r in REFUSED:
            self.assertIsInstance(bridge.validate_rule(r), str, r)

    def test_parentheses_in_the_content_round_trip(self):
        self.assertEqual(bridge.parse_rule("Bash(echo (x):*)"), ("Bash", "echo (x):*"))
        self.assertEqual(bridge.format_rule(*bridge.parse_rule("Bash(echo (x):*)")),
                         "Bash(echo (x):*)")
        self.assertEqual(bridge.parse_rule("mcp__github__get_issue"),
                         ("mcp__github__get_issue", None))


class BindingTests(unittest.TestCase):
    def judge(self, rule, tool, inp, suggestion):
        return bridge.judge_rule("r " + rule, tool, inp, suggestion, "/proj")[1]

    def test_a_rule_must_cover_the_request(self):
        self.assertIsNone(self.judge("Bash(npm test:*)", "Bash",
                                     {"command": "npm test -- --watch=false"}, "Bash(npm test:*)"))
        self.assertIsNotNone(self.judge("Bash(git:*)", "Bash", {"command": "npm test"},
                                        "Bash(npm test)"))
        self.assertIsNotNone(self.judge("Bash(npm test:*)", "Bash",
                                        {"command": "npm testing"}, "Bash(npm test:*)"))

    def test_a_rule_may_not_be_broader_than_the_suggestion(self):
        inp = {"command": "npm test"}
        self.assertIsNotNone(self.judge("Bash(git:*)", "Bash", inp, "Bash(npm test)"))
        self.assertIsNotNone(self.judge("Bash(npm test:*)", "Bash", inp, "Bash(npm test)"))
        self.assertIsNone(self.judge("Bash(npm test)", "Bash", inp, "Bash(npm test)"))
        self.assertIsNone(self.judge("Bash(npm test:*)", "Bash", inp, "Bash(npm:*)"))

    def test_a_rule_for_another_tool_is_refused(self):
        why = self.judge("Edit(src/**)", "Bash", {"command": "npm test"}, "Bash(npm test)")
        self.assertIn("another tool", why)

    def test_file_rules_match_a_path_inside_the_project(self):
        inp = {"file_path": "/proj/src/parser.py"}
        self.assertIsNone(self.judge("Edit(src/**)", "Edit", inp, "Edit(src/**)"))
        self.assertIsNotNone(self.judge("Edit(docs/**)", "Edit", inp, "Edit(docs/**)"))
        self.assertIsNotNone(self.judge("Edit(src/**)", "Edit", inp, "Edit(src/parser.py)"))

    def test_the_whole_tool_is_wider_than_anything(self):
        self.assertTrue(bridge.rule_within("Bash(git status:*)", "Bash"))
        self.assertFalse(bridge.rule_within("Bash", "Bash(git status:*)"))


class SettingsTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d, True)
        os.makedirs(os.path.join(self.d, ".claude"))
        self.path = bridge.settings_local_path(self.d)

    def write(self, text):
        with open(self.path, "w") as f:
            f.write(text)

    def test_missing_file_lists_nothing(self):
        self.assertEqual(bridge.allow_lines(self.d), ["No local allow rules."])

    def test_malformed_json_is_reported_and_not_rewritten(self):
        self.write("{not json")
        self.assertIn("Could not read", bridge.allow_lines(self.d)[0])
        removed, err = bridge.remove_allow_rule(self.d, 1)
        self.assertIsNone(removed)
        self.assertIn("Could not read", err)
        with open(self.path) as f:
            self.assertEqual(f.read(), "{not json")

    def test_remove_deletes_exactly_one_entry_and_keeps_the_rest(self):
        self.write(json.dumps({"model": "x", "permissions": {
            "allow": ["Read", "Bash(npm test:*)", "Glob"], "deny": ["Bash(rm:*)"]}}))
        self.assertEqual(bridge.allow_lines(self.d)[:3], ["1. Read", "2. Bash(npm test:*)", "3. Glob"])
        removed, err = bridge.remove_allow_rule(self.d, 2)
        self.assertEqual((removed, err), ("Bash(npm test:*)", None))
        with open(self.path) as f:
            data = json.load(f)
        self.assertEqual(data, {"model": "x", "permissions": {
            "allow": ["Read", "Glob"], "deny": ["Bash(rm:*)"]}})

    def test_out_of_range_gives_a_notice(self):
        self.write(json.dumps({"permissions": {"allow": ["Read"]}}))
        self.assertEqual(bridge.remove_allow_rule(self.d, 9), (None, "There is no rule 9."))


class FakeTerm:
    cols = 80
    rules_enabled = True

    def __init__(self, answer):
        self.answer = answer
        self.suggestion = "unset"
        self.notices = []
        self.agents = None

    async def ask_permission(self, pid, tool, summary, detail, suggestion=None):
        self.suggestion = suggestion
        return self.answer

    async def notice(self, text):
        self.notices.append(text)


class Ctx:
    def __init__(self, rules):
        from claude_agent_sdk.types import PermissionRuleValue, PermissionUpdate
        self.suggestions = [PermissionUpdate(type="addRules", behavior="allow",
                                             destination="localSettings",
                                             rules=[PermissionRuleValue(t, c) for t, c in rules])] if rules else []
        self.agent_id = None


class SessionTests(unittest.IsolatedAsyncioTestCase):
    def make(self, answer, plan=False):
        cfg = bridge.SessionConfig(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, cfg.cwd, True)
        term = FakeTerm(answer)
        sess = bridge.ClaudeSession(term, cfg)
        sess.agents = type("A", (), {"label_for": lambda self, i: None})()
        return sess, term, cfg

    async def test_a_valid_rule_allows_and_passes_a_localsettings_update(self):
        sess, term, _ = self.make("r Bash(npm test:*)")
        res = await sess._can_use_tool("Bash", {"command": "npm test -- x"},
                                       Ctx([("Bash", "npm test:*")]))
        self.assertEqual(term.suggestion, "Bash(npm test:*)")
        self.assertEqual(type(res).__name__, "PermissionResultAllow")
        u = res.updated_permissions[0]
        self.assertEqual((u.type, u.behavior, u.destination), ("addRules", "allow", "localSettings"))
        self.assertEqual((u.rules[0].tool_name, u.rules[0].rule_content), ("Bash", "npm test:*"))

    async def test_a_refused_rule_denies_with_a_notice(self):
        for rule in ("Bash", "Bash(sh:*)", "Bash(git:*)", "Edit(src/**)"):
            sess, term, _ = self.make("r " + rule)
            res = await sess._can_use_tool("Bash", {"command": "npm test"},
                                           Ctx([("Bash", "npm test")]))
            self.assertEqual(type(res).__name__, "PermissionResultDeny", rule)
            self.assertTrue(term.notices[0].startswith("Rule not saved:"), rule)

    async def test_no_suggestion_falls_back_to_the_exact_command(self):
        sess, term, _ = self.make("y")
        await sess._can_use_tool("Bash", {"command": "make all"}, Ctx([]))
        self.assertEqual(term.suggestion, "Bash(make all)")
        sess, term, _ = self.make("y")
        await sess._can_use_tool("Edit", {"file_path": "/x"}, Ctx([]))
        self.assertEqual(term.suggestion, "Edit")

    async def test_exit_plan_mode_never_offers_a_rule(self):
        sess, term, cfg = self.make("y")
        cfg.plan = True
        await sess._can_use_tool("ExitPlanMode", {"plan": "x"}, Ctx([("ExitPlanMode", None)]))
        self.assertIsNone(term.suggestion)

    async def test_a_rule_answer_without_an_offer_is_not_a_rule(self):
        sess, term, _ = self.make("r Bash(npm test:*)")
        term.rules_enabled = False
        res = await sess._can_use_tool("Bash", {"command": "npm test"}, Ctx([]))
        self.assertEqual(type(res).__name__, "PermissionResultDeny")


class WireTests(Base):
    def rules_client(self, flag=True, optin=True):
        c = self.client(*(["--allow-rules"] if flag else []))
        if optin:
            c.send("!RULES")
        return c

    def ask(self, c, sid, prompt):
        """Sends a prompt and returns (index of the ? line, its id)."""
        start = len(c.lines)
        c.send(f"{sid}P {prompt}")
        i = c.wait_for(lambda l: l.startswith(f"{sid}? "), 20, start)
        return start, i, c.text()[i].split()[1]

    def test_the_word_comes_only_with_the_flag(self):
        self.assertTrue([l for l in self.rules_client().text() if l.startswith("#OK")][0].endswith(" rules"))
        c = self.rules_client(flag=False)
        self.assertNotIn("rules", [l for l in c.text() if l.startswith("#OK")][0].split())

    def test_q_plus_precedes_the_question_and_an_r_answer_saves(self):
        c = self.rules_client()
        self.open_new(c, 1, "alpha")
        start, i, pid = self.ask(c, 1, "fix it")
        lines = c.text()
        self.assertEqual(lines[i - 1 - 0] if lines[i - 1].startswith("1Q+") else
                         [l for l in lines[start:i] if l.startswith("1Q+")][0],
                         f"1Q+ {pid} Edit(src/**)")
        c.send(f"1A {pid} r Edit(src/**)")
        c.wait_line("1>", start=i)
        c.wait_for(lambda l: l.startswith("1I Rule saved: Edit(src/**)"), start=i)
        path = os.path.join(self.root, "alpha", ".claude", "settings.local.json")
        with open(path) as f:
            self.assertEqual(json.load(f)["permissions"]["allow"], ["Edit(src/**)"])
        c.close()

    def test_a_refused_rule_denies_the_request(self):
        c = self.rules_client()
        self.open_new(c, 1, "alpha")
        _, i, pid = self.ask(c, 1, "fix it")
        c.send(f"1A {pid} r Edit")
        c.wait_for(lambda l: l.startswith("1I Rule not saved:"), start=i)
        c.wait_for(lambda l: "(denied)" in l, start=i)
        c.wait_line("1>", start=i)
        c.close()

    def test_without_the_flag_or_the_optin_there_is_no_q_plus_and_r_means_no(self):
        for flag, optin in ((False, True), (True, False)):
            c = self.rules_client(flag=flag, optin=optin)
            self.open_new(c, 1, "alpha")
            _, i, pid = self.ask(c, 1, "fix it")
            self.assertFalse(any(l.startswith("1Q+") for l in c.text()))
            c.send(f"1A {pid} r Edit(src/**)")
            c.wait_for(lambda l: "(denied)" in l, start=i)
            c.wait_line("1>", start=i)
            c.close()

    def test_a_chat_never_gets_q_plus(self):
        c = self.rules_client()
        start = len(c.lines)
        c.send("2O *")
        c.wait_line("2>", start=start)
        c.send("2P search the web")
        c.wait_for(lambda l: l.startswith("2? "), 20, start)
        self.assertFalse(any(l.startswith("2Q+") for l in c.text()))
        c.close()

    def test_allow_lists_and_removes_with_a_restart(self):
        c = self.rules_client()
        self.open_new(c, 1, "alpha")
        d = os.path.join(self.root, "alpha", ".claude")
        os.makedirs(d)
        with open(os.path.join(d, "settings.local.json"), "w") as f:
            json.dump({"permissions": {"allow": ["Read", "Glob"]}}, f)
        start = len(c.lines)
        c.send("1P /allow")
        c.wait_for(lambda l: l == "1I 1. Read", start=start)
        c.wait_for(lambda l: l == "1I 2. Glob", start=start)
        c.send("1P /allow remove 1")
        c.wait_for(lambda l: l == "1I Removed: Read", start=start)
        c.wait_for(lambda l: l == "1P " or True, start=start)
        c.send("1P /allow")
        c.wait_for(lambda l: l == "1I 1. Glob", start=start)
        c.send("1P /allow remove 9")
        c.wait_for(lambda l: l == "1I There is no rule 9.", start=start)
        c.close()

    def test_remove_is_refused_without_the_flag(self):
        c = self.rules_client(flag=False)
        self.open_new(c, 1, "alpha")
        start = len(c.lines)
        c.send("1P /allow remove 1")
        c.wait_for(lambda l: "needs a bridge started with --allow-rules" in l, start=start)
        c.close()


if __name__ == "__main__":
    unittest.main()
