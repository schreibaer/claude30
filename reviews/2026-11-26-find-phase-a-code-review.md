# Code review: Find and keys (Phase A, U1-U3)

Scope: commits after 4992e2b on `claude/project-thread-kds1ny`. Plan: `docs/plans/2026-11-26-2215-feat-find-commands-rules-changes-plan.md`.

**How this review was done.** A single-reader review of the diff against the plan, by the same agent that wrote it. It did not run the multi-reviewer `ce-code-review` pipeline or a cross-model pass, so treat it as a first pass and expect a second look from the person or a later review.

## Verified

- 68k build is clean (no compiler warnings); host tests pass (15 of 15, including the new `test_find`); `tests/test_menu_resources.py` passes with the new Edit items and mask `0x7FFFF77D`.
- Cmd-Shift keys are caught before `MenuKey` (KTD1), so Cmd-F is Find and Cmd-Shift-F is Full Screen; Cmd-M reports why it is dimmed.
- Core search: case folding including Mac Roman accents, empty and too-long patterns, edges, and chunk-edge matches at every split point.

## Findings

1. **Minor, by design.** On a restart, Find searches upward from the top of the view, as KTD4 says, so a match that is visible on screen is skipped in favour of an older one. Revisit if testing on the SE/30 finds it surprising.
2. **Minor.** Status messages ("Not found.", "Not in window...") go through `window_set_status`, which later session updates may overwrite quickly. Check on hardware that they stay readable.
3. **Minor.** The Log window scrolls only with Up, Down, Page Up and Page Down (no scroll bar). The plan did not require one.
4. **Not covered.** The Mac-side code (Find dialog, fold match, file-window find, Log window, tab hide and close) compiles but was never run: there is no Mac or emulator in this environment. The plan's emulator and hardware rows (AE1, AE2, AE3, the file-window case, log speed on 200 KB) are still open.
5. **Not covered.** `U2` in the plan lists changes to `fold.c`; none were needed because `Fold.body` is already public.

No correctness bugs were found in the host-testable code.
