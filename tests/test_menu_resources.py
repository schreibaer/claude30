"""Checks the menus in client/claude30.r against the item numbers in resources.h.

Two mistakes here have shipped before and are invisible on the host: an enable
mask written as if bit 0 were the menu's own flag (File > Quit stayed grey,
PR #7), and an item added without shifting the mask or the item constants
(PR #13 review). Rez's 31-bit MENU field has bit 0 = item 1; separators are
off and every other item is on, because adjust_menus() decides the rest.
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


if __name__ == "__main__":
    unittest.main()
