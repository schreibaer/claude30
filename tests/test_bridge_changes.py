"""Changes: git status and diffs as read-only virtual files (:changes and
:diff/<path>) over the file lines. Runs bridge.py --demo over pipes against
temporary git repositories. Run with:
    python3 -m unittest discover -s tests
"""
import os
import shutil
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
from test_bridge_files import FilesBase, HAVE_GIT  # noqa: E402


def git(cwd, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")
    subprocess.run(["git", *args], cwd=cwd, env=env, check=True, capture_output=True)


@unittest.skipUnless(HAVE_GIT, "git is not installed")
class ChangesTests(FilesBase):
    def repo(self):
        git(self.proj, "init", "-q", "-b", "main")
        self.write("src/a.c", "int a;\nint b;\n")
        self.write("old.txt", "keep me\nline2\nline3\n")
        self.write("gone.txt", "bye\n")
        self.write("staged.txt", "s1\n")
        self.write("moved.txt", "one\ntwo\nthree\nfour\nfive\nsix\n")
        self.write(".env", "SECRET=1\n")
        self.write(".gitignore", "ignored.log\n.env\n")
        git(self.proj, "add", "-f", ".")
        git(self.proj, "commit", "-q", "-m", "first")

    def text(self, c, path, **kw):
        r = self.get(c, path, **kw)
        if isinstance(r, str):
            return r
        return b"\n".join(r[1]).decode("mac_roman"), r[0]

    def change_things(self):
        self.write("src/a.c", "int a;\nint b2;\nint c;\n")             # modified
        self.write("new.txt", "n1\nn2\n")                                # untracked
        os.unlink(os.path.join(self.proj, "gone.txt"))                   # deleted
        git(self.proj, "mv", "moved.txt", "renamed.txt")                 # renamed
        self.write("staged.txt", "s1\ns2\n")
        git(self.proj, "add", "staged.txt")                              # staged
        self.write(".env", "SECRET=2\n")                                 # hidden, changed
        self.write("ignored.log", "x\n")                                 # ignored

    def test_changes_lists_the_visible_files_with_codes_and_counts(self):
        self.repo()
        self.change_things()
        c = self.connect()
        self.assertIn("changes", [l for l in c.text() if l.startswith("#OK")][0].split())
        out, hdr = self.text(c, ":changes")
        self.assertEqual(hdr[-1], "r:changes")
        lines = out.split("\n")
        self.assertEqual(lines[0], "Branch: main")
        body = "\n".join(lines[1:])
        self.assertRegex(body, r" M src/a.c \+2 -1")
        self.assertRegex(body, r"\?\? new.txt \+2 -0")
        self.assertRegex(body, r" D gone.txt \+0 -1")
        self.assertRegex(body, r"R  renamed.txt \(from moved.txt\) \+0 -0")
        self.assertRegex(body, r"M  staged.txt \+1 -0")
        self.assertNotIn(".env", body)
        self.assertNotIn("ignored.log", body)
        self.assertEqual(len([l for l in lines[1:] if l]), 5)
        c.p.stdin.close()

    def test_diff_of_a_file_and_the_refusals(self):
        self.repo()
        self.change_things()
        c = self.connect()
        out, hdr = self.text(c, ":diff/src/a.c")
        self.assertEqual(hdr[-1], "r:diff")
        self.assertIn("-int b;", out)
        self.assertIn("+int b2;", out)
        self.assertEqual(self.get(c, ":diff/.env"), "hidden")
        self.assertEqual(self.get(c, ":diff/../x"), "outside project")
        self.assertEqual(self.get(c, ":diff/nope"), "not found")
        for p in (":diff/*", ":diff/.en[v]", ":diff/:(glob)**/.env"):
            self.assertIn(self.get(c, p), ("not found", "hidden", "bad name"), p)
        # a deleted file's diff has its - lines, a renamed file opens under its new path
        self.assertIn("-bye", self.text(c, ":diff/gone.txt")[0])
        self.assertIn("renamed.txt", self.text(c, ":diff/renamed.txt")[0])
        # an untracked file's diff is every line with +
        out = self.text(c, ":diff/new.txt")[0]
        self.assertIn("+n1", out)
        self.assertIn("+n2", out)
        c.p.stdin.close()

    def test_a_repo_with_no_commits_lists_files_as_added(self):
        git(self.proj, "init", "-q", "-b", "main")
        self.write("a.txt", "x\n")
        git(self.proj, "add", "a.txt")
        c = self.connect()
        out, _ = self.text(c, ":changes")
        self.assertRegex(out, r"A  a.txt \+1 -0")
        self.assertIn("+x", self.text(c, ":diff/a.txt")[0])
        c.p.stdin.close()

    def test_a_clean_tree_and_a_non_repo(self):
        self.repo()
        c = self.connect()
        self.assertIn("No changes since the last commit.", self.text(c, ":changes")[0])
        c.p.stdin.close()
        shutil.rmtree(os.path.join(self.proj, ".git"))
        c = self.connect()
        self.assertEqual(self.get(c, ":changes"), "not git")
        c.p.stdin.close()

    def test_a_binary_diff_is_one_line(self):
        self.repo()
        self.write("pic.bin", b"\0\1\2\3" * 10)
        git(self.proj, "add", "pic.bin")
        git(self.proj, "commit", "-q", "-m", "bin")
        self.write("pic.bin", b"\0\9\9\9" * 10)
        c = self.connect()
        out, _ = self.text(c, ":diff/pic.bin")
        self.assertIn("Binary files", out)
        self.assertEqual(len([l for l in out.split("\n") if "Binary" in l]), 1)
        self.assertIn("binary", self.text(c, ":changes")[0])
        c.p.stdin.close()

    def test_a_huge_diff_is_cut(self):
        self.repo()
        self.write("big.txt", "".join(f"line {i}\n" for i in range(50000)))
        c = self.connect()
        r = self.get(c, ":diff/big.txt", count=200)
        self.assertEqual(r[0][-1], "r:diff")
        self.assertLessEqual(int(r[0][2]), 32000)
        c.p.stdin.close()

    def test_git_config_cannot_run_programs(self):
        self.repo()
        marker = os.path.join(self.outside, "ran")
        script = os.path.join(self.outside, "x.sh")
        with open(script, "w") as f:
            f.write(f"#!/bin/sh\ntouch {marker}\ncat\n")
        os.chmod(script, 0o755)
        git(self.proj, "config", "diff.external", script)
        git(self.proj, "config", "core.fsmonitor", script)
        git(self.proj, "config", "filter.x.clean", script)
        git(self.proj, "config", "diff.t.textconv", script)
        self.write(".gitattributes", "*.c filter=x diff=t\n")
        self.write("src/a.c", "int a;\nchanged;\n")
        c = self.connect()
        self.text(c, ":changes")
        self.text(c, ":diff/src/a.c")
        self.assertFalse(os.path.exists(marker))
        c.p.stdin.close()

    def test_an_untracked_symlink_shows_one_line_not_the_target(self):
        self.repo()
        with open(os.path.join(self.outside, "secret.txt"), "w") as f:
            f.write("TOPSECRET\n")
        os.symlink(os.path.join(self.outside, "secret.txt"), os.path.join(self.proj, "lnk"))
        c = self.connect()
        self.assertIn("lnk", self.text(c, ":changes")[0])
        out = self.text(c, ":diff/lnk")[0]
        self.assertIn("symbolic link", out)
        self.assertNotIn("TOPSECRET", out)
        c.p.stdin.close()

    def test_a_host_file_named_like_a_virtual_path_is_not_listed_and_is_refused(self):
        self.repo()
        self.write(":changes", "fake\n")
        c = self.connect()
        self.assertNotIn(":changes", self.listing(c))
        self.assertEqual(self.get(c, "./:changes"), "outside project")
        self.assertEqual(self.get(c, "sub/:changes"), "bad name")
        out, hdr = self.text(c, ":changes")
        self.assertEqual(hdr[-1], "r:changes")      # the virtual one, not the host file
        c.p.stdin.close()

    def test_pieces_of_one_read_come_from_one_snapshot(self):
        self.repo()
        self.write("src/a.c", "".join(f"v{i}\n" for i in range(300)))
        c = self.connect()
        h1, lines1, _ = self.get(c, ":diff/src/a.c", first=0, count=50)
        self.write("src/a.c", "totally different\n")
        h2, lines2, _ = self.get(c, ":diff/src/a.c", first=50, count=50)
        self.assertEqual(h1[3], h2[3])              # the same token
        self.assertEqual(h1[2], h2[2])              # the same total
        h3, _, _ = self.get(c, ":diff/src/a.c", first=0, count=50)
        self.assertNotEqual(h1[3], h3[3])           # a read from 0 takes a fresh one
        c.p.stdin.close()


class FilesOffTests(FilesBase):
    files_mode = "off"

    def test_no_word_and_a_refusal_with_files_off(self):
        c = self.connect()
        self.assertNotIn("changes", [l for l in c.text() if l.startswith("#OK")][0].split())
        self.assertEqual(self.get(c, ":changes"), "files off")
        c.p.stdin.close()


if __name__ == "__main__":
    unittest.main()
