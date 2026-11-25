"""Protocol version 2 file service: the path guard, hidden files, reading
with CRCs, and saving. Runs bridge.py --demo over pipes against temporary
project folders. Standard library only (git is used when installed).
    python3 -m unittest discover -s tests
"""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from v2client import Client  # noqa: E402


def crc(data):
    return f"{zlib.crc32(data) & 0xFFFFFFFF:08x}"


HAVE_GIT = shutil.which("git") is not None


class FilesBase(unittest.TestCase):
    files_mode = "rw"

    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.proj = os.path.join(self.root, "proj")
        os.mkdir(self.proj)
        self.outside = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.addCleanup(shutil.rmtree, self.outside, True)
        self.x = 0

    def write(self, rel, data, mode=None):
        path = os.path.join(self.proj, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
        if mode is not None:
            os.chmod(path, mode)
        return path

    def read(self, rel):
        with open(os.path.join(self.proj, rel), "rb") as f:
            return f.read()

    def connect(self, *extra):
        c = Client("--demo", "--projects", self.root, "--files", self.files_mode, *extra)
        self.addCleanup(lambda: c.p.poll() is None and c.p.kill())
        c.hello()
        self.assertEqual(c.open(1, "proj"), "1O proj")
        c.wait_line("1>")
        return c

    def next_x(self):
        self.x += 1
        return str(self.x)

    def listing(self, c, path=""):
        start = len(c.lines)
        c.send(f"1L {path}")
        i = c.wait_for(lambda l: l.startswith("1L]") or l.startswith("1N - "), start=start)
        if c.text()[i].startswith("1N"):
            return c.text()[i][5:]
        j = c.wait_for(lambda l: l.startswith("1L["), start=start)
        return [l[3:] for l in c.text()[j + 1:i] if l.startswith("1L ")]

    def get(self, c, path, first=0, count=200, budget=4096):
        """(header fields, [raw line bytes], trailer fields) or the refusal."""
        x = self.next_x()
        start = len(c.lines)
        c.send(f"1G {x} {first} {count} {budget} {path}")
        i = c.wait_for(lambda l: l.startswith(f"1Z {x} ") or l.startswith(f"1N {x} "),
                       start=start)
        if c.text()[i].startswith("1N"):
            return c.text()[i].split(" ", 2)[2]
        h = c.wait_for(lambda l: l.startswith(f"1H {x} "), start=start)
        lines = []
        for raw in c.lines[h + 1:i]:
            if raw == b"W":
                continue
            if raw.startswith(b"1B"):
                lines.append(raw[3:])
            elif raw.startswith(b"1+"):
                lines[-1] += raw[3:]
        return c.text()[h].split()[2:], lines, c.text()[i].split()[2:]

    def save(self, c, path, token, text, block=2048, corrupt=(), commit_crc=None, force=False):
        """Sends a whole save; returns the final K/N line. corrupt lists block
        numbers whose first try gets a wrong CRC."""
        x = self.next_x()
        start = len(c.lines)
        c.send(f"1F {x} {token} {path}")
        i = c.wait_for(lambda l: l.startswith(f"1K {x} ") or l.startswith(f"1N {x} "),
                       start=start)
        if c.text()[i] != f"1K {x} 0":
            return c.text()[i]
        lines = text.split(b"\r")
        blocks, cur = [], []
        for ln in lines:
            cur.append(ln)
            if sum(len(b) + 1 for b in cur) >= block:
                blocks.append(cur)
                cur = []
        if cur or not blocks:
            blocks.append(cur)
        self.resends = 0
        for seq, blk in enumerate(blocks, 1):
            tries = 0
            while True:
                data = b"\r".join(blk)
                bad = seq in corrupt and tries == 0
                c.send(f"1S[ {x} {seq}")
                for ln in blk:
                    c.send(b"1S " + ln)
                c.send(f"1S] {x} {seq} {len(data)} {'00000000' if bad else crc(data)}")
                start = len(c.lines)
                j = c.wait_for(lambda l: l.startswith(f"1K {x} {seq}") or l.startswith(f"1N {x} {seq}"),
                               start=start)
                tries += 1
                if c.text()[j] == f"1K {x} {seq}":
                    break
                self.resends += 1
                self.assertLess(tries, 3)
        start = len(c.lines)
        c.send(f"1S. {x} {len(text)} {commit_crc or crc(text)}" + (" force" if force else ""))
        j = c.wait_for(lambda l: l.startswith(f"1K {x} saved") or l.startswith(f"1N {x} "),
                       start=start)
        self.last_x = x
        return c.text()[j]


class ListingTest(FilesBase):
    @unittest.skipUnless(HAVE_GIT, "needs git")
    def test_hidden_and_ignored_entries(self):
        self.write(".env", "SECRET=1\n")
        self.write(".env.example", "SECRET=\n")
        self.write(".gitignore", "build/\n")
        self.write("build/out.o", "x")
        self.write("README.md", "hi\n")
        self.write("src/a.c", "int a;\n")
        subprocess.run(["git", "init", "-q", self.proj], check=True)
        subprocess.run(["git", "-C", self.proj, "add", ".env.example"], check=True)
        c = self.connect()
        names = [e.split(" ", 2)[2] for e in self.listing(c)]
        self.assertEqual(names, ["src", ".gitignore", "README.md"])
        self.assertEqual(self.listing(c)[0], "d 0 src")
        self.assertIn("f 3 README.md", self.listing(c))
        self.assertEqual(self.get(c, ".env.example"), "hidden")
        self.assertEqual(self.get(c, "build/out.o"), "hidden")
        self.assertEqual(self.listing(c, "build"), "hidden")
        self.assertEqual(self.listing(c, ".git"), "hidden")
        c.close()

    def test_symlinks_and_escapes(self):
        os.symlink(self.outside, os.path.join(self.proj, "notes"))
        with open(os.path.join(self.outside, "secret.txt"), "w") as f:
            f.write("outside\n")
        self.write("docs/real.txt", "inside\n")
        os.symlink(os.path.join(self.proj, "docs", "real.txt"), os.path.join(self.proj, "link.txt"))
        c = self.connect()
        names = [e.split(" ", 2)[2] for e in self.listing(c)]
        self.assertNotIn("notes", names)
        self.assertIn("link.txt", names)
        h, lines, z = self.get(c, "link.txt")
        self.assertEqual(lines, [b"inside"])
        for bad in ("../x", "/etc/hosts", "docs/../../x", "notes/secret.txt", "a\0b", "./docs"):
            self.assertEqual(self.get(c, bad), "outside project", bad)
        self.assertEqual(self.get(c, "a\x01b"), "bad name")
        self.assertEqual(self.get(c, "nothere.txt"), "not found")
        self.assertEqual(self.get(c, "docs"), "not found")
        c.close()

    def test_hidden_names_ignore_letter_case(self):
        sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        import bridge
        for name in (".ENV", ".Env.local", ".GIT", "Server.PEM", "ID_RSA.pub", ".SSH"):
            self.assertTrue(bridge.hidden_name(name), name)
        self.assertFalse(bridge.hidden_name("environment.txt"))

    def test_symlink_cannot_alias_a_hidden_file(self):
        self.write(".env", "SECRET=1\n")
        os.symlink(os.path.join(self.proj, ".env"), os.path.join(self.proj, "config.txt"))
        c = self.connect()
        names = [e.split(" ", 2)[2] for e in self.listing(c)]
        self.assertNotIn("config.txt", names)
        self.assertEqual(self.get(c, "config.txt"), "hidden")
        self.assertEqual(self.save(c, "config.txt", "0", b"x").split(" ", 2)[2], "hidden")
        c.close()

    @unittest.skipUnless(HAVE_GIT, "needs git")
    def test_ignored_file_stays_hidden_beside_a_folder_link(self):
        # git refuses paths "beyond a symbolic link"; that must not show
        # the ignored file next to the link.
        self.write(".gitignore", "*.log\n")
        self.write("secret.log", "token\n")
        self.write("d/a.txt", "a\n")
        os.symlink(os.path.join(self.proj, "d"), os.path.join(self.proj, "lnk"))
        subprocess.run(["git", "init", "-q", self.proj], check=True)
        c = self.connect()
        names = [e.split(" ", 2)[2] for e in self.listing(c)]
        self.assertNotIn("secret.log", names)
        self.assertIn("lnk", names)
        self.assertEqual(self.get(c, "secret.log"), "hidden")
        h, lines, z = self.get(c, "lnk/a.txt")
        self.assertEqual(lines, [b"a"])
        c.close()

    def test_control_character_names(self):
        self.write("ok.txt", "x\n")
        self.write("bad\r\nname.txt", "x\n")
        c = self.connect()
        names = [e.split(" ", 2)[2] for e in self.listing(c)]
        self.assertEqual(names, ["ok.txt"])
        c.close()


class ReadTest(FilesBase):
    def test_small_lf_file(self):
        self.write("ten.txt", "".join(f"line {i}\n" for i in range(10)))
        c = self.connect()
        h, lines, z = self.get(c, "ten.txt")
        self.assertEqual(h[:3], ["0", "10", "10"])
        self.assertEqual(h[4], "e")
        self.assertEqual(len(lines), 10)
        joined = b"\r".join(f"line {i}".encode() for i in range(10))
        self.assertEqual(z, [str(len(joined)), crc(joined)])
        # A zero-line request returns the token only.
        h2, lines2, z2 = self.get(c, "ten.txt", count=0)
        self.assertEqual((h2[1], h2[3], lines2, z2), ("0", h[3], [], ["0", "00000000"]))
        c.close()

    def test_long_line_continues(self):
        self.write("wide.txt", "w" * 300 + "\nshort\n")
        c = self.connect()
        h, lines, z = self.get(c, "wide.txt")
        self.assertEqual(lines, [b"w" * 300, b"short"])
        b_lines = [l for l in c.text() if l.startswith("1B") or l.startswith("1+")]
        self.assertEqual(len(b_lines), 3)
        self.assertEqual(z[1], crc(b"w" * 300 + b"\rshort"))
        c.close()

    def test_mac_roman_round_trip(self):
        original = "a ≤ b ≠ c d\n\tindented\n".encode("utf-8")
        self.write("sym.txt", original)
        c = self.connect()
        h, lines, z = self.get(c, "sym.txt")
        self.assertEqual(h[4], "e")
        self.assertEqual(lines, ["a ≤ b ≠ c d".encode("mac_roman"), b"\tindented"])
        self.assertEqual(self.save(c, "sym.txt", h[3], b"\r".join(lines)).split()[2], "saved")
        self.assertEqual(self.read("sym.txt"), original)
        c.close()

    def test_latin1_control_characters_cannot_forge_lines(self):
        self.write("odd.txt", b"caf\xe9\r2T x\nesc \x1b here\n")
        c = self.connect()
        start = len(c.lines)
        h, lines, z = self.get(c, "odd.txt")
        self.assertEqual(h[4], "r:encoding")
        self.assertEqual(lines, [b"caf\x8e", b"2T x", b"esc ? here"])
        self.assertEqual(z[1], crc(b"\r".join(lines)))
        self.assertFalse([l for l in c.text()[start:] if l.startswith("2")])
        c.close()

    def test_budget(self):
        self.write("rows.txt", "".join("r" * 499 + "\n" for _ in range(20)))
        c = self.connect()
        h, lines, z = self.get(c, "rows.txt", count=200, budget=4096)
        self.assertEqual((h[1], len(lines)), ("8", 8))
        h, lines, z = self.get(c, "rows.txt", first=16)
        self.assertEqual((h[0], h[1]), ("16", "4"))
        c.close()

    def test_read_only_reasons(self):
        self.write("emoji.txt", "rocket \U0001F680\n")
        self.write("mixed.txt", b"a\r\nb\nc\r\n")
        self.write("big.txt", "x" * 100 + "\n" + ("y" * 99 + "\n") * 290)
        self.write("ctl.txt", "a\x0cb\n")
        self.write("image.png", b"\x89PNG\r\n\x1a\n\0\0\0\rIHDR")
        c = self.connect()
        self.assertEqual(self.get(c, "emoji.txt")[0][4], "r:chars")
        self.assertEqual(self.get(c, "mixed.txt")[0][4], "r:endings")
        self.assertEqual(self.get(c, "big.txt")[0][4], "r:big")
        self.assertEqual(self.get(c, "ctl.txt")[0][4], "r:control")
        self.assertEqual(self.get(c, "image.png"), "binary")
        # Read-only text is shown through the prose stand-ins.
        self.assertEqual(self.get(c, "emoji.txt")[1], [b"rocket "])
        c.close()

    def test_over_32000_lines(self):
        self.write("huge.txt", "".join(f"{i}\n" for i in range(33000)))
        c = self.connect()
        h, lines, z = self.get(c, "huge.txt", first=31990)
        self.assertEqual((h[1], h[2], h[4]), ("10", "32000", "r:long"))
        self.assertEqual(lines[-1], b"31999")
        c.close()


class SaveTest(FilesBase):
    def test_crlf_without_final_newline_and_mode(self):
        self.write("run.sh", b"#!/bin/sh\r\necho one\r\necho two", mode=0o755)
        c = self.connect()
        h, lines, z = self.get(c, "run.sh")
        self.assertEqual(h[4], "e")
        text = b"\r".join(lines).replace(b"one", b"uno")
        r = self.save(c, "run.sh", h[3], text)
        self.assertTrue(r.startswith(f"1K {self.last_x} saved "), r)
        self.assertEqual(self.read("run.sh"), b"#!/bin/sh\r\necho uno\r\necho two")
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(self.proj, "run.sh")).st_mode), 0o755)
        self.assertEqual(r.split()[3], self.get(c, "run.sh")[0][3])   # the new token
        # Claude hears about it at the start of the next turn, once.
        start = len(c.lines)
        c.turn(1, "hello", answer="n")
        said = " ".join(c.session(1, start))
        self.assertIn("edited these files from the SE/30", said)
        self.assertIn("run.sh", said)
        start = len(c.lines)
        c.turn(1, "again", answer="n")
        self.assertNotIn("edited these files", " ".join(c.session(1, start)))
        c.close()

    def test_conflict_then_force(self):
        self.write("a.txt", "one\ntwo\n")
        c = self.connect()
        h, lines, z = self.get(c, "a.txt")
        self.write("a.txt", "Claude wrote this\n")
        r = self.save(c, "a.txt", h[3], b"one\rTWO")
        self.assertEqual(r, f"1N {self.last_x} changed")
        self.assertEqual(self.read("a.txt"), b"Claude wrote this\n")
        c.send(f"1S. {self.last_x} 7 {crc(b'one' + bytes([13]) + b'TWO')} force")
        c.wait_for(lambda l: l.startswith(f"1K {self.last_x} saved"))
        self.assertEqual(self.read("a.txt"), b"one\nTWO\n")
        c.close()

    def test_damaged_blocks(self):
        body = "".join(f"line {i:04d} " + "z" * 40 + "\n" for i in range(150))
        self.write("long.txt", body)
        c = self.connect()
        h, lines, z = self.get(c, "long.txt")
        h2, lines2, z2 = self.get(c, "long.txt", first=int(h[1]))
        text = b"\r".join(lines + lines2).replace(b"line 0100", b"LINE 0100")
        r = self.save(c, "long.txt", h[3], text, corrupt=(2,))
        self.assertEqual(self.resends, 1)
        self.assertIn("saved", r)
        self.assertEqual(self.read("long.txt"), body.replace("line 0100", "LINE 0100").encode())
        before = self.read("long.txt")
        token = self.get(c, "long.txt", count=0)[0][3]
        r = self.save(c, "long.txt", token, text + b"!", commit_crc="12345678")
        self.assertEqual(r, f"1N {self.last_x} damaged")
        self.assertEqual(self.read("long.txt"), before)
        c.close()

    def test_refused_saves(self):
        self.write("emoji.txt", "rocket \U0001F680\n")
        self.write(".env", "S=1\n")
        c = self.connect()
        tok = self.get(c, "emoji.txt")[0][3]
        self.assertEqual(self.save(c, "emoji.txt", tok, b"x").split(" ", 2)[2], "read-only")
        self.assertEqual(self.save(c, ".env", "0", b"x").split(" ", 2)[2], "hidden")
        self.assertEqual(self.save(c, "nothere.txt", "0", b"x").split(" ", 2)[2], "not found")
        c.close()

    def test_control_characters_are_not_saved(self):
        self.write("a.txt", "one\n")
        c = self.connect()
        h, lines, z = self.get(c, "a.txt")
        self.assertEqual(self.save(c, "a.txt", h[3], b"one\x03two").split(" ", 2)[2], "control")
        self.assertEqual(self.read("a.txt"), b"one\n")
        self.assertIn("saved", self.save(c, "a.txt", h[3], b"one\ttwo"))
        c.close()

    def test_stale_temp_file_is_removed(self):
        self.write("a.txt", "one\n")
        stale = self.write(".a.txt.se30-abc123.tmp", "half written")
        c = self.connect()
        h, lines, z = self.get(c, "a.txt")
        self.assertIn("saved", self.save(c, "a.txt", h[3], b"two"))
        self.assertFalse(os.path.exists(stale))
        self.assertEqual(sorted(os.listdir(self.proj)), ["a.txt"])
        c.close()


class ReadOnlyModeTest(FilesBase):
    files_mode = "ro"

    def test_ro_refuses_saves(self):
        self.write("a.txt", "one\n")
        c = self.connect()
        self.assertIn("#OK se30bridge 2 files=ro resume ping models", c.text())
        h, lines, z = self.get(c, "a.txt")
        self.assertEqual(h[4], "r:ro")
        self.assertEqual(self.save(c, "a.txt", h[3], b"two").split(" ", 2)[2], "read-only")
        self.assertEqual(self.read("a.txt"), b"one\n")
        c.close()


class OffModeTest(FilesBase):
    files_mode = "off"

    def test_off_refuses_everything(self):
        self.write("a.txt", "one\n")
        c = self.connect()
        self.assertIn("#OK se30bridge 2 files=off resume ping models", c.text())
        self.assertEqual(self.listing(c), "files off")
        self.assertEqual(self.get(c, "a.txt"), "files off")
        c.close()


if __name__ == "__main__":
    unittest.main()
