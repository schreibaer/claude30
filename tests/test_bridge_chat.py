"""Chats: tabs outside any project. Opening one with the reserved name `*`,
what Claude is given in it, how it resumes in a new call, and what deletes
it. Runs bridge.py --demo over pipes; bridges in one test share a state
folder (and a stand-in for Claude's own folder), as calls on one host do.
Run with:
    python3 -m unittest discover -s tests
"""
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import time
import unittest
import uuid

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from v2client import Client  # noqa: E402
import bridge  # noqa: E402

TOKEN = re.compile(r"^[0-9a-f]{16}$")


class Base(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        for name in ("alpha", "*"):          # a folder called * is never a project
            os.mkdir(os.path.join(self.root, name))
        self.state = tempfile.mkdtemp(prefix="se30state")
        self.claude = tempfile.mkdtemp(prefix="claudecfg")
        for d in (self.root, self.state, self.claude):
            self.addCleanup(shutil.rmtree, d, True)

    def client(self, *args, **kw):
        c = Client("--demo", "--projects", self.root, *args, state_dir=self.state,
                   env={"SE30_EVICT_WAIT": "3", "CLAUDE_CONFIG_DIR": self.claude}, **kw)
        self.addCleanup(c.reap)
        c.hello()
        return c

    def open_chat(self, c, sid, tok=None):
        """Opens (or resumes) a chat; returns (token, how)."""
        start = len(c.lines)
        if tok:
            c.send(f"{sid}Y {tok} *")
        else:
            c.send(f"{sid}O *")
        c.wait_line(f"{sid}O *", start=start)
        i = c.wait_for(lambda l: l.startswith(f"{sid}Y "), 10, start)
        _, token, how = c.text()[i].split(" ")
        c.wait_line(f"{sid}>", start=i)
        return token, how

    def records(self):
        with open(os.path.join(self.state, "sessions.json")) as f:
            return json.load(f)

    def chat_dirs(self):
        root = os.path.join(self.state, "chats")
        return sorted(os.listdir(root)) if os.path.isdir(root) else []

    def transcript(self, claude_id, child="-some-chat-folder"):
        """A stand-in for the transcript Claude Code saves for a conversation."""
        d = os.path.join(self.claude, "projects", child)
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, claude_id + ".jsonl")
        with open(path, "w") as f:
            f.write("{}\n")
        return path

    def said(self, c, sid, start):
        i = c.wait_for(lambda l: l.startswith(f"{sid}T You said:"), 10, start)
        return c.text()[i]


class OpenTest(Base):
    def test_capability_and_open_lines(self):
        c = self.client()
        ok = [l for l in c.text() if l.startswith("#OK")][0]
        self.assertTrue(ok.endswith(" chat stream md usage cmds changes"), ok)
        start = len(c.lines)
        self.assertEqual(c.open(1, "*"), "1O *")
        c.wait_line("1>", start=start)
        tags = [l for l in c.text()[start:] if l[:1] == "1"]
        self.assertEqual([t[:2] for t in tags], ["1O", "1Y", "1M", "1>"])
        self.assertRegex(tags[1].split(" ")[1], TOKEN)
        self.assertEqual(tags[1].split(" ")[2], "new")
        c.close()

    def test_each_chat_has_its_own_private_folder(self):
        c = self.client()
        t1, _ = self.open_chat(c, 1)
        t2, _ = self.open_chat(c, 2)
        recs = self.records()
        f1, f2 = recs[t1]["cwd"], recs[t2]["cwd"]
        self.assertNotEqual(f1, f2)
        for f in (f1, f2):
            self.assertEqual(os.path.dirname(f), os.path.join(self.state, "chats"))
            self.assertTrue(os.path.isdir(f))
            self.assertEqual(stat.S_IMODE(os.stat(f).st_mode), 0o700)
        self.assertEqual(recs[t1]["project"], "*")
        c.close()

    def test_a_folder_called_star_is_not_a_project(self):
        c = self.client()
        self.assertEqual(c.projects(), ["alpha"])
        self.assertEqual(c.open(1, "*"), "1O *")
        c.close()

    def test_chat_and_project_tabs_side_by_side(self):
        c = self.client()
        self.assertEqual(c.open(1, "alpha"), "1O alpha")
        self.open_chat(c, 2)
        start = len(c.lines)
        c.turn(1, "hello")
        self.assertIn("alpha", self.said(c, 1, start))
        c.close()

    def test_chat_files_are_refused(self):
        c = self.client()
        self.open_chat(c, 1)
        start = len(c.lines)
        c.send("1L .")
        i = c.wait_for(lambda l: l.startswith("1N - "), start=start)
        self.assertEqual(c.text()[i][5:], "files off")
        c.close()


class CommandsTest(Base):
    def test_edits_and_agents_do_not_apply(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        for cmd in ("/edits on", "/agents"):
            start = len(c.lines)
            c.send(f"1P {cmd}")
            c.wait_line("1>", start=start)
            infos = [l for l in c.text()[start:] if l.startswith("1I ")]
            self.assertEqual(infos, [f"1I {cmd.split()[0]} does not apply in a chat."])
        self.assertEqual(list(self.records()), [tok])    # no restart happened
        c.close()

    def test_help_is_the_chat_help(self):
        c = self.client()
        self.open_chat(c, 1)
        start = len(c.lines)
        c.send("1P /help")
        c.wait_line("1>", start=start)
        text = "\n".join(c.text()[start:])
        self.assertIn("This is a chat", text)
        self.assertNotIn("/edits", text)
        c.close()

    def test_new_keeps_the_folder_and_changes_the_token(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        folder = self.records()[tok]["cwd"]
        old_id = self.records()[tok]["claude_id"]
        old_file = self.transcript(old_id)
        start = len(c.lines)
        c.send("1P /new")
        i = c.wait_for(lambda l: l.startswith("1Y "), 10, start)
        new = c.text()[i].split(" ")
        c.wait_line("1>", start=i)
        self.assertEqual(new[2], "new")
        self.assertNotEqual(new[1], tok)
        self.assertEqual(self.records()[new[1]]["cwd"], folder)
        self.assertNotIn(tok, self.records())
        self.assertFalse(os.path.exists(old_file))      # nothing can resume it now
        c.close()

    def test_a_web_search_asks_and_always_covers_the_next(self):
        c = self.client()
        self.open_chat(c, 1)
        start = len(c.lines)
        c.send("1P search the news")
        i = c.wait_for(lambda l: l.startswith("1Q "), 10, start)
        self.assertIn("WebSearch", c.text()[i])
        j = c.wait_for(lambda l: l.startswith("1? "), 10, i)
        c.send(f"1A {c.text()[j].split()[1]} a")
        c.wait_line("1>", start=j)
        start = len(c.lines)
        c.send("1P search again")
        c.wait_line("1>", start=start)
        self.assertFalse([l for l in c.text()[start:] if l.startswith("1? ")])
        c.close()


class ResumeTest(Base):
    def test_resume_in_a_new_call_keeps_folder_and_count(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        c.turn(1, "one")
        folder = self.records()[tok]["cwd"]
        c.close()
        c = self.client()
        self.assertEqual(self.open_chat(c, 1, tok), (tok, "resumed"))
        start = len(c.lines)
        c.turn(1, "two")
        self.assertIn("turn 2", self.said(c, 1, start))
        self.assertEqual(self.records()[tok]["cwd"], folder)
        c.close()

    def test_a_folder_deleted_meanwhile_is_made_again(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        c.turn(1, "one")
        folder = self.records()[tok]["cwd"]
        c.close()
        shutil.rmtree(folder)
        c = self.client()
        self.assertEqual(self.open_chat(c, 1, tok)[0], tok)
        self.assertTrue(os.path.isdir(folder))
        c.close()

    def test_a_record_pointing_elsewhere_starts_a_new_chat(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        c.turn(1, "one")
        c.close()
        recs = self.records()
        recs[tok]["cwd"] = self.root                      # not a chat folder
        with open(os.path.join(self.state, "sessions.json"), "w") as f:
            json.dump(recs, f)
        c = self.client()
        new, how = self.open_chat(c, 1, tok)
        self.assertEqual(how, "new")
        self.assertNotEqual(new, tok)
        self.assertTrue(os.path.isdir(self.root))         # and was not touched
        c.close()

    def test_chat_and_project_tokens_do_not_cross(self):
        c = self.client()
        chat, _ = self.open_chat(c, 1)
        c.turn(1, "hi")
        c.open(2, "alpha")
        c.turn(2, "hi")
        proj = [t for t, r in self.records().items() if r["project"] == "alpha"][0]
        c.close()
        c = self.client()
        new, how = self.open_chat(c, 1, proj)             # a project's token, as a chat
        self.assertEqual(how, "new")
        start = len(c.lines)
        c.send(f"2Y {chat} alpha")                         # a chat's token, as a project
        c.wait_line("2O alpha", start=start)
        i = c.wait_for(lambda l: l.startswith("2Y "), 10, start)
        self.assertEqual(c.text()[i].split(" ")[2], "new")
        c.close()


class DeleteTest(Base):
    def test_closing_a_chat_deletes_everything_of_it(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        rec = self.records()[tok]
        file = self.transcript(rec["claude_id"])
        start = len(c.lines)
        c.send("1X")
        c.wait_line("1X closed", start=start)
        self.assertNotIn(tok, self.records())
        self.assertFalse(os.path.exists(rec["cwd"]))
        self.assertFalse(os.path.exists(file))
        self.assertFalse(os.path.exists(os.path.dirname(file)))   # its emptied folder
        c.close()

    def test_closing_a_project_tab_leaves_transcripts_alone(self):
        c = self.client()
        c.open(1, "alpha")
        c.turn(1, "hi")
        tok = list(self.records())[0]
        file = self.transcript(self.records()[tok]["claude_id"])
        start = len(c.lines)
        c.send("1X")
        c.wait_line("1X closed", start=start)
        self.assertTrue(os.path.exists(file))
        c.close()

    def test_a_dropped_call_keeps_the_chat(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        c.turn(1, "hi")
        folder = self.records()[tok]["cwd"]
        c.close()                                          # the link drops
        self.assertIn(tok, self.records())
        self.assertTrue(os.path.isdir(folder))

    def test_quit_keeps_the_chat(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        folder = self.records()[tok]["cwd"]
        c.send("!QUIT")
        c.p.wait(10)
        self.assertIn(tok, self.records())
        self.assertTrue(os.path.isdir(folder))

    def test_close_then_quit_still_deletes(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        rec = self.records()[tok]
        file = self.transcript(rec["claude_id"])
        c.send("1X")
        c.send("!QUIT")
        c.close()                                           # and the link is gone
        self.assertNotIn(tok, self.records())
        self.assertFalse(os.path.exists(rec["cwd"]))
        self.assertFalse(os.path.exists(file))

    def age(self, token, days):
        recs = self.records()
        recs[token]["used"] = time.time() - days * 86400
        with open(os.path.join(self.state, "sessions.json"), "w") as f:
            json.dump(recs, f)

    def test_the_sweep_deletes_a_chat_unused_for_over_a_week(self):
        c = self.client()
        old, _ = self.open_chat(c, 1)
        fresh, _ = self.open_chat(c, 2)
        c.open(3, "alpha")
        c.turn(3, "hi")
        proj = [t for t, r in self.records().items() if r["project"] == "alpha"][0]
        recs = self.records()
        old_file = self.transcript(recs[old]["claude_id"])
        fresh_file = self.transcript(recs[fresh]["claude_id"], "-other")
        old_dir, fresh_dir = recs[old]["cwd"], recs[fresh]["cwd"]
        stray = os.path.join(self.state, "chats", "0123456789abcdef")
        os.mkdir(stray)
        c.close()
        self.age(old, 8)
        self.age(fresh, 6)
        self.age(proj, 20)
        c = self.client()
        recs = self.records()
        self.assertNotIn(old, recs)
        self.assertFalse(os.path.exists(old_dir))
        self.assertFalse(os.path.exists(old_file))
        self.assertFalse(os.path.exists(stray))
        self.assertIn(fresh, recs)
        self.assertTrue(os.path.isdir(fresh_dir))
        self.assertTrue(os.path.exists(fresh_file))
        self.assertIn(proj, recs)                          # projects are not swept at a week
        self.assertEqual(self.open_chat(c, 1, fresh), (fresh, "resumed"))
        c.close()

    def test_a_chat_record_outlives_the_project_limit_until_swept(self):
        # The 30-day limit for project records must not orphan a chat's folder.
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        folder = self.records()[tok]["cwd"]
        c.close()
        self.age(tok, 40)
        c = self.client()
        self.assertFalse(os.path.exists(folder))
        self.assertNotIn(tok, self.records())
        c.close()

    def test_symlinks_are_never_followed(self):
        c = self.client()
        tok, _ = self.open_chat(c, 1)
        cid = self.records()[tok]["claude_id"]
        outside = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, outside, True)
        keep = os.path.join(outside, "keep.txt")
        open(keep, "w").write("x")
        projects = os.path.join(self.claude, "projects")
        os.makedirs(projects)
        os.symlink(outside, os.path.join(projects, "linked"))       # a child that is a link
        os.makedirs(os.path.join(projects, "real"))
        os.symlink(keep, os.path.join(projects, "real", cid + ".jsonl"))  # a link as the file
        start = len(c.lines)
        c.send("1X")
        c.wait_line("1X closed", start=start)
        self.assertTrue(os.path.exists(keep))
        self.assertTrue(os.path.islink(os.path.join(projects, "real", cid + ".jsonl")))
        self.assertIn("not a plain file", c.stderr_now().decode())
        c.close()


class OptionsTest(unittest.TestCase):
    """What the SDK is started with. The wire tests above use the demo
    session; these check the real session's options without starting Claude."""

    class Term:
        cols = 80

    def session(self, chat):
        cfg = bridge.SessionConfig("/tmp/x", False, None)
        cfg.chat = chat
        return bridge.ClaudeSession(self.Term(), cfg)

    def test_a_chat_gets_web_tools_and_a_short_prompt(self):
        o = self.session(True).options()
        self.assertEqual(o.tools, ["WebSearch", "WebFetch"])
        self.assertEqual(o.allowed_tools, [])          # each web call asks
        self.assertEqual(o.setting_sources, [])
        self.assertTrue(o.strict_mcp_config)
        self.assertIsInstance(o.system_prompt, str)
        self.assertIn("SE/30", o.system_prompt)
        self.assertIn("80 columns", o.system_prompt)

    def test_a_project_session_is_unchanged(self):
        o = self.session(False).options()
        self.assertIsNone(o.tools)
        self.assertEqual(o.allowed_tools, ["Read", "Glob", "Grep", "TodoWrite"])
        self.assertEqual(o.system_prompt["preset"], "claude_code")
        self.assertEqual(o.setting_sources, ["user", "project", "local"])
        self.assertFalse(o.strict_mcp_config)

    def test_resume_and_new_ids(self):
        s = self.session(True)
        s.cfg.claude_id = str(uuid.uuid4())
        self.assertEqual(s.options().session_id, s.cfg.claude_id)
        s.cfg.resume = True
        o = s.options()
        self.assertEqual(o.resume, s.cfg.claude_id)


if __name__ == "__main__":
    unittest.main()
