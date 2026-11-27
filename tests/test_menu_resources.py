"""Checks the menus in client/claude30.r against the item numbers in resources.h.

Two mistakes here have shipped before and are invisible on the host: an enable
mask written as if bit 0 were the menu's own flag (File > Quit stayed grey,
PR #7), and an item added without shifting the mask or the item constants
(PR #13 review). Rez's 31-bit MENU field has bit 0 = item 1; separators are
off and every other item is on, because adjust_menus() decides the rest.

It also checks every dialog item list: a StaticText, EditText or UserItem whose
rect overlaps a button erases that button when it is redrawn (SetDialogItemText
or an update), so the button shows as a stray line. The permission dialog's key
hint covered its Rule... button that way (PR #22).
"""
import os
import re
import unittest

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
RSRC = os.path.join(ROOT, "client", "claude30.r")
HDR = os.path.join(ROOT, "client", "src", "mac", "resources.h")

# item constant -> (menu, start of the item's text in claude30.r)
ITEMS = {
    "kAboutItem": ("kAppleMenu", "About Claude30"),
    "kOpenFileItem": ("kFileMenu", "Open Project File"),
    "kCloseItem": ("kFileMenu", "Close"),
    "kSaveItem": ("kFileMenu", "Save"),
    "kRevertItem": ("kFileMenu", "Revert to Saved"),
    "kFullScreenItem": ("kFileMenu", "Full Screen"),
    "kCompactItem": ("kFileMenu", "Compact Input"),
    "kQuitItem": ("kFileMenu", "Quit"),
    "kUndoItem": ("kEditMenu", "Undo"),
    "kCutItem": ("kEditMenu", "Cut"),
    "kCopyItem": ("kEditMenu", "Copy"),
    "kPasteItem": ("kEditMenu", "Paste"),
    "kClearItem": ("kEditMenu", "Clear"),
    "kSelectAllItem": ("kEditMenu", "Select All"),
    "kFindItem": ("kEditMenu", "Find"),
    "kFindAgainItem": ("kEditMenu", "Find Again"),
    "kFindNewerItem": ("kEditMenu", "Find Newer"),
    "kSettingsItem": ("kEditMenu", "Settings"),
    "kConnectItem": ("kSessionMenu", "Connect"),
    "kDisconnectItem": ("kSessionMenu", "Disconnect"),
    "kStopItem": ("kSessionMenu", "Stop"),
    "kPrevPromptItem": ("kSessionMenu", "Previous Prompt"),
    "kNextPromptItem": ("kSessionMenu", "Next Prompt"),
    "kFoldItem": ("kSessionMenu", "Open or Close Fold"),
    "kNewTabItem": ("kSessionMenu", "New Tab"),
    "kNewChatItem": ("kSessionMenu", "New Chat"),
    "kCloseTabItem": ("kSessionMenu", "Close Tab"),
    "kModelItem": ("kSessionMenu", "Model and Effort"),
    "kStreamItem": ("kSessionMenu", "Stream Answers"),
    "kPlanItem": ("kSessionMenu", "Plan Mode"),
    "kChangesItem": ("kSessionMenu", "Show Changes"),
    "kLinkTestItem": ("kSessionMenu", "Link Test"),
}


def menus():
    """{menu id name: (mask or None for allEnabled, [item texts])}"""
    src = open(RSRC, encoding="mac_roman").read()
    out = {}
    for m in re.finditer(r"resource 'MENU' \((\w+)\)\s*\{\s*\w+,\s*\w+,\s*(\w+),"
                         r"[^{]*\{(.*?)\}\s*\};", src, re.S):
        name, mask, body = m.groups()
        items = re.findall(r'^\s*"((?:[^"\\]|\\.)*)"\s*,', body, re.M)
        out[name] = (None if mask == "allEnabled" else int(mask, 0), items)
    return out


def dialog_items(src=None):
    """{DITL id name: [(kind, (top, left, bottom, right))]}"""
    if src is None:
        src = open(RSRC, encoding="mac_roman").read()
    out = {}
    for m in re.finditer(r"resource 'DITL' \((\w+)\)\s*\{\s*\{(.*?)\n\t\}\s*\};", src, re.S):
        out[m.group(1)] = [
            (kind, tuple(int(n) for n in rect))
            for *rect, kind in re.findall(
                r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\},\s*(\w+)",
                m.group(2))]
    return out


def overlaps(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def button_overlaps(items):
    """Pairs (text item, button) whose rects intersect."""
    buttons = [r for k, r in items if k == "Button"]
    return [(k, r, b) for k, r in items
            if k in ("StaticText", "EditText", "UserItem")
            for b in buttons if overlaps(r, b)]


def settings_items(src=None):
    """[(kind, rect, text)] for the Settings DITL, in item order (item 1 first)."""
    if src is None:
        src = open(RSRC, encoding="mac_roman").read()
    m = re.search(r"resource 'DITL' \(kSettingsDialog\)\s*\{\s*\{(.*?)\n\t\}\s*\};", src, re.S)
    return [(kind, tuple(int(n) for n in rect), text)
            for *rect, kind, text in re.findall(
                r"\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\},\s*(\w+)\s*\{\s*\w+,\s*\"([^\"]*)\"",
                m.group(1))]


class MenuResourceTests(unittest.TestCase):
    def test_masks_have_separators_off_and_items_on(self):
        found = menus()
        self.assertIn("kSessionMenu", found)
        for name, (mask, items) in found.items():
            if mask is None:
                continue
            self.assertEqual(mask >> 31, 0, f"{name}: the mask is 31 bits")
            for i, text in enumerate(items):
                want = 0 if text == "-" else 1
                self.assertEqual(
                    (mask >> i) & 1, want,
                    f"{name} item {i + 1} ({text!r}): bit {i} should be {want}. "
                    f"Bit 0 is item 1; separators off, every other item on.")
            extra = mask >> len(items)
            self.assertEqual(extra, (1 << (31 - len(items))) - 1,
                             f"{name}: bits past the last item should stay on")

    def test_item_constants_name_the_right_items(self):
        found = menus()
        defs = dict(re.findall(r"#define\s+(k\w+Item)\s+(\d+)", open(HDR).read()))
        for const, (menu, text) in ITEMS.items():
            self.assertIn(const, defs, f"{const} missing from resources.h")
            items = found[menu][1]
            n = int(defs[const])
            self.assertTrue(
                1 <= n <= len(items) and items[n - 1].startswith(text),
                f"{const} = {n} but {text!r} is item "
                f"{next((i + 1 for i, t in enumerate(items) if t.startswith(text)), '?')}"
                f" of {menu}: shift the constants after inserting a menu item")


    def test_no_dialog_item_covers_a_button(self):
        found = dialog_items()
        self.assertIn("kPermDialog", found)
        self.assertGreaterEqual(len(found["kPermDialog"]), 8)
        for name, items in found.items():
            self.assertEqual(
                button_overlaps(items), [],
                f"{name}: an item overlaps a button and will erase it when redrawn")

    def test_settings_dialog_offers_four_speeds_and_fits_the_screen(self):
        items = settings_items()
        defs = dict(re.findall(r"#define\s+(kSet\w+)\s+(\d+)", open(HDR).read()))
        speeds = {"kSet115200": "115,200 baud", "kSet57600": "57,600 baud",
                  "kSet38400": "38,400 baud", "kSet19200": "19,200 baud"}
        for const, text in speeds.items():
            self.assertIn(const, defs, f"{const} missing from resources.h")
            kind, _, label = items[int(defs[const]) - 1]
            self.assertEqual((kind, label), ("RadioButton", text), const)
        # Radios in one column, fastest on top, and nothing falls off the dialog.
        left = [items[int(defs[c]) - 1][1] for c in speeds]
        self.assertEqual(sorted(left), left)
        src = open(RSRC, encoding="mac_roman").read()
        top, lft, bottom, right = map(int, re.search(
            r"resource 'DLOG' \(kSettingsDialog\) \{\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)", src).groups())
        self.assertLessEqual(bottom, 342 - 8, "the dialog must fit a 512 x 342 screen")
        for kind, r, text in items:
            self.assertLessEqual(r[2], bottom - top, f"{text or kind!r} falls below the dialog")
            self.assertLessEqual(r[3], right - lft, f"{text or kind!r} falls off the right edge")

    def test_overlap_check_catches_the_old_permission_hint(self):
        src = open(RSRC, encoding="mac_roman").read()
        old = src.replace("{ 280, 10, 298, 200 }, StaticText", "{ 280, 10, 298, 274 }, StaticText")
        self.assertNotEqual(old, src)
        self.assertTrue(button_overlaps(dialog_items(old)["kPermDialog"]))


if __name__ == "__main__":
    unittest.main()
