"""describe_tool marks an Edit that replaces every match, so the client's
summary does not claim a single place."""
import bridge


def test_replace_all_marks_the_header():
    head, body = bridge.describe_tool(
        "Edit", {"file_path": "/p/a.py", "old_string": "a", "new_string": "b", "replace_all": True}, "/p")
    assert head == "a.py (all occurrences)"
    assert body == ["@@ -1 +1 @@", "-a", "+b"]


def test_plain_edit_is_unchanged():
    head, _ = bridge.describe_tool(
        "Edit", {"file_path": "/p/a.py", "old_string": "a", "new_string": "b"}, "/p")
    assert head == "a.py"
