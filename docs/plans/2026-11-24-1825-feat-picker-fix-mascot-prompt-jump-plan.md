---
title: Picker Refresh, Claude Mascot and Keyboard Scrolling - Plan
type: feat
date: 2026-11-24
topic: claude30-polish
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Picker Refresh, Claude Mascot and Keyboard Scrolling - Plan

## Goal Capsule

- **Objective:** On the SE/30, the Open Project File list always shows the folder the person is in, a tab where Claude is working is obvious at a glance, and the person can move anywhere in a long transcript, including between their own prompts, from the keyboard without touching the scroll bar.
- **Means:** Erase list areas before the List Manager redraws them (KTD1), a 1-bit Claude mascot sprite animated from the main event loop in the input area (KTD2 to KTD5), a per-tab list of prompt positions that Cmd-Up and Cmd-Down scroll to (KTD6, KTD7), and a set of modified arrow and navigation keys that scroll the transcript (KTD8).
- **Product authority:** This Product Contract. PROTOCOL.md v2 and the bridge are not touched.
- **Execution profile:** One pull request to `schreibaer/claude30`, client only. Build order U1, U2, U3, U4, U5. U1 is independent and can land first if the others slip.
- **Stop conditions:** Stop and ask if the animation measurably slows serial draining at 57,600 baud (line-noise markers appear in the emulator burst case that did not appear before), or if the mascot cannot be read at 1 bit and 16 pixels tall.
- **Who finishes:** An implementing agent builds the app with the shared Retro68 toolchain cache, runs the host tests and the emulator rows below, and refreshes the release files. The person checks the hardware rows on the real SE/30.
- **Open blockers:** None.

---

## Product Contract

### Summary

Three client changes to Claude30.
The Open Project File list and the New Tab project list are erased before every redraw, so names from a previous folder never linger.
While a turn runs, a small pixel Claude mascot walks in the input box next to a changing word, and it stays visible but smaller once the person types ahead.
Cmd-Up and Cmd-Down scroll the transcript to the previous or next prompt the person sent, and further keys scroll by a few lines, by a page, and to the top or bottom, so the scroll bar is never needed.

### Problem Frame

The person browses project folders from the SE/30 and reads long transcripts on a 512 by 342 screen.
Opening a folder in Open Project File leaves the previous folder's names on screen below the new entries: only as many rows as the new folder has are redrawn, so the list looks wrong whenever a folder has fewer entries than the last one.
While Claude works, the only sign is a bullet on the tab label and a status line at the bottom, which is easy to miss.
Going back to an earlier prompt means dragging the scroll bar thumb or holding a scroll arrow, which is slow with a one-button mouse on a long transcript.

### Key Decisions

- **The animation is the Claude mascot.** The person asked for the mascot instead of the starburst first proposed. Governs R4.
- **Typing ahead stays allowed while Claude works.** The input box stays editable during a turn, so the animation must never cover typed text. Governs R5.
- **Every scroll the bar offers has a key.** The person wants to stop clicking the scroll bar. Whether the bar itself stays is an open product choice (see Open Questions); the key set is the same either way. Governs R12, R13.
- **Cmd-Up and Cmd-Down are the prompt shortcut.** They work on any keyboard layout, unlike Cmd-[ and Cmd-] on a German layout, and Cmd-Up already means "go up" in Open Project File. Governs R8.

### Requirements

**File and project lists**

- R1. After opening a folder or going up in Open Project File, the list shows exactly the new folder's entries, with no rows left over from the previous folder, and is scrolled to the top.
- R2. The New Tab project list has the same guarantee when it refills after a fresh project list arrives.

**Claude is working**

- R3. While the front tab's turn runs, the input area shows a Claude animation. It stops as soon as the turn ends or is stopped.
- R4. The animation is a 1-bit pixel version of the Claude mascot with a few frames of movement, beside a short word that changes every few seconds. While a tool runs, the word names the tool (for example "Running Bash"). While the tab waits for a permission answer, the mascot stands still with its arms up and the word reads "Waiting for you".
- R5. When the input box is empty, the mascot and word sit inside the input box. When the person has typed text, the animation moves to a small spot beside the Send button and never draws over the text.
- R6. The animation belongs to the front tab. Switching to an idle tab hides it; switching to a busy tab shows it.
- R7. The animation never delays receiving serial data. It pauses while a modal dialog is open.

**Jumping between prompts**

- R8. Cmd-Up scrolls the transcript so the nearest prompt above the current top line sits at the top of the transcript area. Cmd-Down does the same for the nearest prompt below it.
- R9. Cmd-Down after the last prompt scrolls to the end of the transcript. Cmd-Up with no earlier prompt in the window beeps.
- R10. The Session menu lists Previous Prompt and Next Prompt, with the arrow keys named in the item text, enabled when the front tab has a transcript.
- R11. Prompts that were trimmed from the window into the session log are not reachable, and jumping never selects or changes text.
- R12. The transcript scrolls from the keyboard whichever area has the focus: Control-Up and Control-Down by three lines, Option-Up and Option-Down by one page, Cmd-Option-Up and Cmd-Option-Down to the top and the bottom. On keyboards that have them, Page Up, Page Down, Home and End do the same as the page and top/bottom keys.
- R13. Plain arrow keys keep moving the caret in the input box, and none of the scroll keys type a character or change the input text.

### Acceptance Examples

- AE1. Covers R1. A project's top folder has 20 entries and `docs/` has 3. Opening `docs/` shows 3 rows and blank space below them. Going Up shows all 20 again from the top.
- AE2. Covers R5. Claude is working and the input box is empty: the mascot walks inside the input box beside "Pondering…". The person types "next, fix the tests": the mascot jumps to the spot by the Send button and the typed text is untouched. The person deletes the text: the mascot returns to the box.
- AE3. Covers R8, R9. The transcript holds prompts A, B and C and the view sits between B and C. Cmd-Up puts B at the top. Cmd-Up again puts A at the top. Cmd-Up again beeps. Cmd-Down three times shows B, then C, then the end of the transcript.

### Open Questions

- **Does the scroll bar stay?** Asked of the person, recommended answer: keep it. If it goes, the transcript view widens by 16 pixels and `gScroll` is removed; if a thin position strip replaces it, it is drawn, not clickable. Only `window.c` layout and U4 change; no other unit depends on the answer.

### Scope Boundaries

- No bridge or PROTOCOL.md changes.
- No animation in file windows, dialogs or the tab strip; the tab strip bullet stays as it is.
- No colour. The SE/30 screen is 1-bit, so the mascot is drawn in black and white only.

#### Deferred to Follow-Up Work

- Jumping to prompts that were trimmed into the log.
- A setting to turn the animation off. Add one if the person finds it distracting.

### Sources / Research

- `client/src/mac/opendlg.c:105-124` (`list_folder`) clears the list with drawing off, then only invalidates; `draw_list` (`opendlg.c:211-218`) calls `LUpdate` without erasing. `InvalRect` never erases, so pixels outside the new rows survive. `client/src/mac/picker.c:88-131` has the same pattern.
- `client/src/mac/window.c` already owns layout (`gInputView`, `gSend`), idle work (`window_idle`), styled inserts (`insert_run`) and trimming (`trim`), which the mascot and prompt jumps extend.
- `client/src/core/batch.c:169-174` (`batch_prompt`) is the only producer of `RS_PROMPT` runs.
- Toolchain: build with the shared Retro68 cache from the "Cache the Retro68 toolchain between threads" work (`tools/setup-retro68.sh`, archive under `/mnt/project-files/toolchain/`), not a fresh toolchain build.

---

## Planning Contract

### Key Technical Decisions

- KTD1. **Lists erase their cell area before redrawing.** `draw_list` in both dialogs erases the list's view rectangle (inside the frame, excluding the scroll bar) and then calls `LUpdate`. Clearing a folder also scrolls the list back to row 0. Erasing in the draw procedure, not only when the list is cleared, covers every path that refills the list, including the project list arriving late. The extra erase per update is cheap for 15 visible rows.
- KTD2. **The animation logic is portable core code.** A new `client/src/core/mascot.c` decides, from the tick count and the tab state, which frame to show, which word to show and where in the box the mascot is. It holds the frame bitmaps as `const` arrays of 16-bit rows. This follows the repo's split of host-tested `core/` logic and thin `mac/` drawing, and lets the frame and word rules be unit tested.
- KTD3. **Frames are 16 by 16, 1-bit, drawn with `CopyBits` from in-memory `BitMap`s.** No resources, no offscreen `GrafPort`, no color QuickDraw. Each frame redraw touches only the mascot rectangle and the word rectangle, so the cost per frame is two small blits and one `DrawText`. Toolbox: `CopyBits`, `EraseRect`, `DrawText`, `TextWidth`, all in System 6 and later.
- KTD4. **The animation runs from `window_idle`, paced by `TickCount`.** A new frame every 8 ticks (about 7.5 frames a second), a new word every 180 ticks. The main loop already calls `window_idle` after `session_pump` on every pass and uses a 1-tick `WaitNextEvent` sleep while online, so the animation adds no new timing path and serial draining keeps priority. During `ModalDialog` the main loop does not run, which pauses the animation as R7 allows.
- KTD5. **The input-box spot is drawn outside TextEdit's text.** When the input is empty, the mascot and word are drawn into the input view after `TEUpdate` and erased before the first keystroke reaches `TEKey`. When text exists, the animation uses a fixed 16 by 16 spot below the Send button, which is inside the input band and never overlaps `gInputView`. Layout gives the spot a rectangle in `layout()` so full screen and normal size both work.
- KTD6. **Each tab keeps a list of prompt start offsets.** `window_append` records the transcript offset of every `RS_PROMPT` run it inserts. `trim` subtracts the cut from every offset and drops offsets that fall below zero. A capped ring of 256 offsets per tab is plenty, since a trimmed transcript holds at most about 28 KB. The bookkeeping (record, shift on trim, find previous and next) is a small portable helper in `core/` with host tests. Scanning TextEdit style runs instead was rejected: the prompt style (Geneva bold) is shared with tool errors and line-noise markers.
- KTD7. **A jump scrolls by whole lines through the existing scroll helper.** The target line is the TextEdit line containing the prompt offset (`lineStarts`), its pixel top is `TEGetHeight(line, 1, te)` as `trim` already computes, and the scroll goes through `scroll_to_px`, which clamps and keeps the scroll bar in sync. "Current position" is the line at the top of the view, so a prompt already at the top counts as current and Cmd-Up moves past it.

- KTD8. **Scroll keys are caught in `do_key` before the input box sees them.** Arrow and navigation keys arrive as character codes (0x1C to 0x1F arrows, 0x0B Page Up, 0x0C Page Down, 0x01 Home, 0x04 End); `do_key` checks the modifiers and routes scroll keys to a new `window_scroll_key` when the main window is in front, else lets them through. Control-arrows are used for line steps because plain arrows belong to the input caret, and Option-arrows for pages because System 7 TextEdit gives them no meaning. All scrolling goes through `scroll_to_px` with the existing line height and page size from `scroll_action`, so the keys and the bar always agree.

### High-Level Technical Design

The mascot, as directional pixel art (the implementer refines it on screen; `#` is black):

```
frame A (stand)      frame B (step)       wait (?)
................     ................     ..........##....
..############..     ..............       .........#..#...
..############..     ..############..     ..######....#...
..##.####.###...     ..############..     ..######...#....
..############..     ..##.####.###...     ..##.##.##.#....
##############..     ..############..     ..######........
##############..     ##############..     ########...#....
..############..     ##############..     ########........
..############..     ..############..     ..######........
..#.#....#.#....     ..############..     ..######........
..#.#....#.#....     ...#.#..#.#.....     ..#.#..#.#......
................     ..#.#....#.#....     ..#.#..#.#......
```

A wide body with two eye holes, stubby arms out the sides and four short legs. Frames: stand, step (legs and body shift one pixel), blink (eyes closed), and wait (still, with a "?"). In the empty input box the word sits at the left and the mascot walks back and forth to its right. In the small spot it only steps in place.

Words cycle through a short list in the Claude Code spirit, for example "Pondering", "Noodling", "Tinkering", "Scheming", "Musing", "Percolating", each followed by "…" (Mac Roman 0xC9). When the tab's `last_tool` is set, the word is "Running " plus the tool name, truncated to fit.

Frame and state selection, as a sketch:

```
mascot_step(state, ticks, busy, waiting, has_text, tool, box_width):
  if not busy and not waiting: hidden
  place  = has_text ? SPOT : BOX
  frame  = waiting ? WAIT : cycle(STAND, STEP, STAND, STEP, ..., BLINK) by ticks / 8
  word   = waiting ? "Waiting for you" : tool ? "Running <tool>" : WORDS[(ticks / 180) % n]
  x      = place == BOX and not waiting ? ping-pong(ticks / 8, box_width - word_width - 16) : 0
```

### Risks & Dependencies

- **TextEdit caret and the mascot share the input box.** `TEIdle` blinks the caret at its own pace. The mascot is drawn only when the box is empty, where the caret sits at the left edge; the walk path starts to the right of the caret's position. Verify in the emulator that the caret still blinks and is never erased.
- **Drawing while the window is behind a file window.** `window_idle` runs whatever window is in front. Drawing must clip to the main window's visible region, which QuickDraw does through the window's port. Verify with a file window partly covering the input band.
- **Build toolchain.** The build waits for the shared Retro68 archive. If it is missing, follow the setup script's instructions rather than building Retro68 from source.

---

## Implementation Units

### U1. Lists that erase before they redraw

- **Goal:** Fix the stale rows in Open Project File and the New Tab picker.
- **Requirements:** R1, R2.
- **Dependencies:** None.
- **Files:** `client/src/mac/opendlg.c`, `client/src/mac/picker.c`.
- **Approach:** Apply KTD1 in both `draw_list` procedures. In `list_folder`, scroll the list to the top after clearing it (`LScroll` by minus the current `visible.top`). Keep the existing `LSetDrawingMode` batching. Check during implementation whether `LDelRow(0, 0, ...)` already resets `visible`; if it does, the scroll call is harmless.
- **Test scenarios:** No host test reaches the List Manager. Emulator rows E1 and E2 cover it: a folder with many entries, then a folder with few, then Up; a scrolled list, then a folder change; and the New Tab picker refilling after Connect.
- **Verification:** Emulator rows E1 and E2 pass.

### U2. Mascot core: frames, words and placement

- **Goal:** Portable logic that decides what the animation shows, with host tests.
- **Requirements:** R3, R4, R5 (decisions only).
- **Dependencies:** None.
- **Files:** `client/src/core/mascot.h`, `client/src/core/mascot.c`, `client/tests/test_mascot.c`, `client/tests/CMakeLists.txt`, `client/CMakeLists.txt`.
- **Approach:** KTD2. Expose one call that takes ticks, busy, waiting, has-text, tool name and box width and fills a small result struct: visible, place, frame index, word and x offset. Frame bitmaps live in this file as `const unsigned short[16]` per frame. No Toolbox headers.
- **Test scenarios:**
  - Not busy and not waiting gives hidden.
  - Busy with an empty input gives the box place; with text gives the spot place.
  - Waiting gives the wait frame, "Waiting for you" and x of 0, whatever the ticks.
  - A tool name gives "Running <tool>", and a tool name too long for the box is cut so the word plus mascot fit.
  - The frame changes every 8 ticks and the word every 180 ticks; the same ticks always give the same result.
  - The walk x stays within 0 and box width minus word width minus 16, turns around at both ends, and is 0 when the box is too narrow.
- **Verification:** `test_mascot` passes with the rest of the host suite.

### U3. Mascot drawing in the window

- **Goal:** Show the animation in the input area of the main window.
- **Requirements:** R3 to R7.
- **Dependencies:** U2.
- **Files:** `client/src/mac/window.c`, `client/src/mac/window.h`, `client/src/mac/modem.c`.
- **Approach:** KTD3 to KTD5. `layout()` adds the spot rectangle below Send. `window_idle` asks the mascot core for the front tab's state and redraws only when the frame, word, place or x changed, erasing the previous rectangles first. `window_update` draws the current state after `TEUpdate`, so covering and uncovering the window restores it. `window_key` erases the in-box animation before passing a keystroke to `TEKey`; Cut, Clear and paste go through the same erase. `modem.c` exposes, or passes in, whether the front tab is busy or waiting and its `last_tool`; reuse `session_tab_busy` and the tab's `want_perm` rather than adding new state. `window_show_tab` forces a redraw so R6 holds.
- **Test scenarios:** Emulator rows E3 to E6.
- **Verification:** Emulator rows E3 to E6 pass, and the burst row E7 shows no new line-noise markers compared with a build without the animation.

### U4. Prompt positions, jump commands and scroll keys

- **Goal:** Cmd-Up and Cmd-Down, and the matching Session menu items, jump between prompts, and the rest of the scroll keys move the transcript.
- **Requirements:** R8 to R13.
- **Dependencies:** None.
- **Files:** `client/src/core/promptmarks.h`, `client/src/core/promptmarks.c`, `client/tests/test_promptmarks.c`, `client/tests/CMakeLists.txt`, `client/CMakeLists.txt`, `client/src/mac/tabs.h`, `client/src/mac/window.c`, `client/src/mac/window.h`, `client/src/mac/main.c`, `client/src/mac/resources.h`, `client/claude30.r`.
- **Approach:** KTD6 and KTD7. The tab gains a prompt-marks record. `window_append` records an offset for each `RS_PROMPT` run before inserting it; `trim` shifts the record by the cut. `window_jump_prompt(direction)` finds the top line, asks the record for the previous or next offset, and scrolls. In `do_key`, Cmd with 0x1E or 0x1F calls the jump for the main window before falling through to `MenuKey`, and only when the main window is in front. Add Previous Prompt and Next Prompt to the Session menu after Stop, with item text naming the arrow keys and no menu key equivalent, and enable them from `adjust_menus` when the front tab has a transcript. Update the Session menu item constants that move. Add `window_scroll_key` for the line, page and top/bottom keys of R12 (KTD8), sharing the step sizes with `scroll_action`. If the person chooses to remove the scroll bar, this unit also removes `gScroll` and widens `gTransView`.
- **Test scenarios:**
  - Recording three offsets and asking for previous and next from positions before, between, on and after them gives the expected offset or "none".
  - A position exactly on a prompt treats it as current: previous returns the one before, next returns the one after.
  - A trim that cuts past the first offset drops it and shifts the rest.
  - The ring drops the oldest entry when full.
  - An empty record gives "none" both ways.
- **Verification:** `test_promptmarks` passes; emulator rows E8 and E9 pass.

### U5. User guide, emulator checklist and release files

- **Goal:** Document the new behavior and ship a fresh build.
- **Requirements:** All.
- **Dependencies:** U1 to U4.
- **Files:** `client/README.md`, `tools/emu/README.md`.
- **Approach:** Add a keyboard table (prompt jumps and scroll keys) and the mascot to the user guide. Add rows E1 to E9 to the emulator checklist and two hardware rows (animation during a long turn at 57,600 baud; prompt jumps in a trimmed transcript). Build with the shared Retro68 cache and copy `Claude30.bin` and `Claude30.dsk` to `/mnt/project-files/release/`.
- **Test scenarios:** None beyond the rows it adds.
- **Verification:** The README shortcuts match the menu, and the release files are dated after the last commit.

---

## Verification Contract

- **Host tests:** `cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host`. All existing tests plus `test_mascot` and `test_promptmarks` pass with `-Wall -Wextra -Werror`.
- **68k build:** set up the toolchain with `tools/setup-retro68.sh` from the shared cache, then build the app as `client/README.md` describes. The build must have no new warnings.
- **Emulator rows** (Basilisk II with the stress bridge, as `tools/emu/README.md` describes):
  - E1. Open Project File: the top folder, then `subfolder/`, then Up. No leftover names at any step, and the list starts at the top each time.
  - E2. New Tab picker opened before the project list arrives, then refilled: no leftover rows.
  - E3. Send `silent`: the mascot walks in the empty input box with a changing word, and shows "Running <tool>" for the tool line.
  - E4. During a running turn, type text: the mascot moves to the spot by Send and the text is intact. Delete it: the mascot returns.
  - E5. Send `perm` in a background tab, then switch to it: the wait frame shows with "Waiting for you" behind the dialog area and the animation stops while the dialog is up.
  - E6. Switch between a busy and an idle tab; toggle Full Screen during a turn: the animation follows the front tab and the new layout.
  - E7. Send `burst`: no line-noise markers that a build without the animation does not also show.
  - E8. With several prompts in the transcript, Cmd-Up and Cmd-Down step through them as AE3 describes, including after `long` has forced a trim.
  - E9. Every key in R12 moves the transcript as described with the focus in the input box and in the transcript; plain arrows still move the input caret; on an extended keyboard Page Up, Page Down, Home and End work.
- **Hardware rows (the person):** the animation during a long real turn at 57,600 baud with no new line-noise markers, and prompt jumps on the real keyboard.

## Definition of Done

- R1 to R13 hold, shown by the host tests and emulator rows above.
- U1 to U5 are merged in one pull request, and the release files in `/mnt/project-files/release/` are rebuilt from it.
- No bridge, PROTOCOL.md or tty-mode changes are in the diff.
- No experimental code, unused frames or leftover debug drawing remain in the diff.

### Deferred to Implementation

- The exact pixels of each mascot frame, tuned on screen at 1 bit.
- The final word list and how long a tool name may be before it is cut.
- Whether `LDelRow(0, 0, ...)` already resets the list's scroll position.
