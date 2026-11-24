---
title: Cmd-D Disconnect, Cmd-Backspace Clear and Hidden Scroll Bar - Plan
type: feat
date: 2026-11-24
topic: claude30-shortcuts
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Cmd-D Disconnect, Cmd-Backspace Clear and Hidden Scroll Bar - Plan

## Goal Capsule

- **Objective:** On the SE/30, the person can hang up and wipe a half-typed prompt from the keyboard, and can give the conversation transcript the full window width once they no longer need its scroll bar.
- **Means:** A `D` key equivalent on Session > Disconnect (KTD1), a Cmd-Backspace check in the main event loop that empties the front tab's input box (KTD2), and a remembered Settings checkbox that hides the transcript scroll bar and widens the transcript (KTD3, KTD4).
- **Product authority:** This Product Contract. PROTOCOL.md v2, the bridge and tty mode are not touched.
- **Execution profile:** One pull request to `schreibaer/claude30`, client only. Build order U1, U2, U3. Each unit stands alone.
- **Stop conditions:** Stop and ask if Cmd-D turns out to be taken by something not found here, or if rewrapping every tab's transcript when the bar is toggled takes visibly long on the emulator with full 32 KB transcripts.
- **Who finishes:** An implementing agent builds the app with the shared Retro68 toolchain cache (`tools/setup-retro68.sh`), runs the host tests and the emulator rows below, and refreshes the release files in `/mnt/project-files/release/`. The person checks the hardware rows on the real SE/30.
- **Open blockers:** None.

---

## Product Contract

### Summary

Three small changes to Claude30.
Cmd-D disconnects, exactly like Session > Disconnect.
Cmd-Backspace empties the front tab's input box in one keystroke.
A "Hide scroll bar" checkbox in Settings removes the transcript's scroll bar and lets the transcript text use that width; keyboard scrolling keeps working.

### Problem Frame

The person drives Claude30 mostly from the keyboard now that every transcript scroll has a key.
Disconnecting still needs the mouse, and deleting a long prompt means holding Backspace or selecting it with the mouse first.
The scroll bar takes 16 of the 512 pixels across the SE/30 screen, and the person no longer uses it.

### Key Decisions

- **Cmd-D is free and becomes Disconnect.** No menu item, window, or dialog filter in the client uses Cmd-D today (menus use O W S F E Q Z X C V K . T and 1 to 6; the dialog filters use Y N A and period). Governs R1.
- **Cmd-Backspace has no undo.** The main window has no Undo today, and the person asked only for the clear. Governs R3.
- **Only the conversation transcript's bar can be hidden.** File windows keep theirs, because dragging it is how large read-only files load their pieces. Governs R5.
- **The setting applies at once.** Unlike port and speed, which wait for the next Connect, the bar disappears when the person clicks OK. Governs R6.

### Requirements

**Disconnect**

- R1. Cmd-D does what Session > Disconnect does, from the main window or a file window and in full screen; the menu shows the shortcut.
- R2. When Disconnect is unavailable (not connected, or already hanging up), Cmd-D does nothing.

**Clearing the input box**

- R3. Cmd-Backspace, with the main window in front, deletes all text in the front tab's input box, whether the caret is in the input box or the transcript, and leaves the caret in the empty input box.
- R4. Cmd-Backspace with no tab open beeps, like typing does, and it never deletes transcript text.

**Hiding the scroll bar**

- R5. Settings has a "Hide scroll bar" checkbox, unchecked by default, that controls the conversation transcript's scroll bar only.
- R6. With the box checked and OK clicked, the bar disappears at once and the transcript text widens into its space, keeping each tab's reading position; unchecking brings it back the same way. Cancel changes nothing.
- R7. The choice is remembered across launches; prefs saved by earlier versions load with the bar shown and every other setting kept.
- R8. With the bar hidden, all transcript scroll keys and the prompt jumps still work.

### Scope Boundaries

- Considered and not built: an undo for Cmd-Backspace. The main window has no undo machinery, and a one-step stash adds state for a keystroke the person chose on purpose. Revisit if the person reports losing prompts by accident.
- Considered and not built: Cmd-Backspace in file windows. The request names the input box; in a file window the key keeps doing nothing.
- Considered and not built: a Cmd-D "Don't Save" button in the Save Changes alert. That alert is a plain `CautionAlert` with no key filter, so the classic convention is not there to clash with.

---

## Planning Contract

### Key Technical Decisions

- KTD1. **Add `D` to the Disconnect item in the `MENU` resource and nothing else.** `do_key` in `client/src/mac/main.c` already sends every Cmd key through `adjust_menus` and `MenuKey`, so the enable rule in `adjust_menus` (`s != ST_IDLE && s != ST_HANGUP`) gives R2 for free, and the existing `disconnect()` keeps its unsaved-files check. Menu keys work in full screen, where the menu bar is hidden.
- KTD2. **Catch Cmd-Backspace in `do_key` before the menu branch, only when the main window is in front.** Match the character code `0x08` or the ADB key code `0x33`, so the check holds whatever keyboard layout the KCHR maps under Cmd. It calls a new `window.c` function that focuses the input record, erases the mascot (it may stand on the text), and deletes all text the same way `window_clear_input` does, including collapsing an auto-opened compact box. `window_scroll_key` runs first but ignores this key, because it only takes arrows and the navigation key codes.
- KTD3. **Prefs version 4 appends `short hide_scrollbar` to `Prefs`.** `prefs_load` gains a version 3 branch that copies up to `offsetof(Prefs, hide_scrollbar)` and sets the new field to 0, mirroring the existing version 2 branch. The value is normalised to 0 or 1 on load like `compact_input`.
- KTD4. **Hidden means `HideControl` plus a wider transcript view, done in `layout` and `relayout`.** `layout()` drops the 16-pixel reserve from `gTransView.right` when the pref is set. `relayout()` shows or hides `gScroll` and refits every tab with `fit_tab`, which already keeps each tab's scroll place and rewraps with `TECalText` (the same path Compact Input and Full Screen use). `window_create` makes the control with its visible flag set from the pref. The scroll code keeps updating the hidden control; `FindControl` ignores invisible controls, so no click can reach it. A new exported call (for example `window_prefs_changed`) runs `relayout` only when the value changed, and `main.c` calls it after `prefs_dialog()` returns true.

Toolbox calls relied on: Menu Manager (`MenuKey`, key equivalents in `MENU`), TextEdit (`TESetSelect`, `TEDelete`), Control Manager (`HideControl`, `ShowControl`, `FindControl`, `GetControlValue`/`SetControlValue`), Dialog Manager (`GetDialogItem`, check box value through `SetControlValue`), Resource Manager for the prefs. All are in System 6 and 7; nothing needs a newer System or more memory.

### Assumptions

- On the SE/30's ADB keyboard, Cmd-Backspace arrives as a `keyDown` with character `0x08`; the key-code match in KTD2 covers a layout where it does not.

---

## Implementation Units

### U1. Cmd-D for Disconnect

- **Goal:** Cmd-D disconnects.
- **Requirements:** R1, R2.
- **Dependencies:** None.
- **Files:** `client/claude30.r`, `client/README.md`.
- **Approach:**
  1. Change the Disconnect item's key from `noKey` to `"D"` (KTD1).
  2. Add Cmd-D to the client README where it lists the Session menu and shortcuts.
- **Patterns to follow:** The `"K"` key on Connect in the same `MENU`.
- **Test scenarios:**
  - Connected, main window in front: Cmd-D hangs up and the status line says so, the same as choosing the menu item.
  - Connected with an unsaved file window in front: Cmd-D shows the save alert first, and Cancel keeps the link up.
  - Full screen: Cmd-D disconnects with the menu bar hidden.
  - Not connected: Cmd-D does nothing and the app does not crash or beep repeatedly.
  - The Session menu shows "Disconnect ⌘D".
- **Verification:** Emulator rows E1 and E2 pass.

### U2. Cmd-Backspace clears the input box

- **Goal:** One keystroke empties the front tab's prompt.
- **Requirements:** R3, R4.
- **Dependencies:** None.
- **Files:** `client/src/mac/main.c`, `client/src/mac/window.c`, `client/src/mac/window.h`, `client/README.md`.
- **Approach:**
  1. Add the clear function to `window.c` and declare it in `window.h` (KTD2). With no input record it beeps.
  2. In `do_key`, after `window_scroll_key` and before the Cmd branch, route Cmd-Backspace to it when `front == window_ptr()`.
  3. Mention Cmd-Backspace in the README next to the input-box keys.
- **Patterns to follow:** `window_clear_input` for the delete and the compact-box collapse; `window_key` for `set_focus` and `mascot_erase` before editing.
- **Test scenarios:**
  - Three lines typed in the input box, Cmd-Backspace: the box is empty, the caret blinks in it, and Send is still enabled or disabled as before.
  - Caret in the transcript after a click, Cmd-Backspace: the input box empties and the transcript is unchanged.
  - Compact Input on, a prompt long enough to open the box, Cmd-Backspace: the text goes and the box collapses back to one line.
  - During a running turn with typed-ahead text, Cmd-Backspace: the text goes and the mascot moves back into the empty box with no leftover pixels.
  - No tab open: Cmd-Backspace beeps.
  - A file window in front: Cmd-Backspace changes nothing in it or in the input box.
  - Plain Backspace still deletes one character.
- **Verification:** Emulator row E3 passes.

### U3. Hide scroll bar setting

- **Goal:** The person can turn the transcript scroll bar off and on, and the choice sticks.
- **Requirements:** R5, R6, R7, R8.
- **Dependencies:** None.
- **Files:** `client/src/mac/prefs.h`, `client/src/mac/prefs.c`, `client/src/mac/resources.h`, `client/claude30.r`, `client/src/mac/window.c`, `client/src/mac/window.h`, `client/src/mac/main.c`, `client/README.md`.
- **Approach:**
  1. Prefs version 4 with `hide_scrollbar` and the version 3 load branch (KTD3); update the header comment that lists what each version loads with.
  2. Settings dialog: add a `CheckBox` item "Hide scroll bar" as item 13 (`kSetHideBar` in `resources.h`), make the `DLOG` taller and move OK and Cancel below it, and reword the footnote so it says port, speed and dial changes apply at the next Connect. The dialog must still fit a 512 by 342 screen under the menu bar.
  3. `prefs_dialog` sets the box from `gPrefs`, toggles it on clicks, and stores it on OK only.
  4. `window.c`: layout and visibility from the pref (KTD4), plus the exported call that relayouts after a change; `main.c` calls it when `prefs_dialog()` returns true.
  5. README: describe the checkbox in the Settings section and under "Moving around the transcript".
- **Patterns to follow:** `compact_input` for the pref field, its version branch and `set_compact`'s relayout; `set_radio` for reading and writing dialog controls.
- **Test scenarios:**
  - Fresh prefs: the box is unchecked and the bar is shown.
  - Check the box and click OK: the bar disappears at once, the transcript text rewraps to the wider width, and each tab keeps its reading position (a tab at the bottom stays at the bottom).
  - Check the box and click Cancel: nothing changes.
  - Quit and relaunch with the bar hidden: it starts hidden, with the transcript at full width from the first draw.
  - A version 3 prefs file from the current release: loads with the bar shown and the port, speed, dial string, tabs and Compact Input kept.
  - Bar hidden: clicking where the bar was does nothing, and every transcript scroll key and Cmd-Up/Cmd-Down still work.
  - Bar hidden, then Full Screen and Compact Input on and off: the layout stays correct and the bar stays hidden.
  - Uncheck the box: the bar returns at the right size and its thumb matches the scroll position.
  - File windows still show their own scroll bars in both states.
- **Verification:** Emulator rows E4 to E6 pass.

---

## Verification Contract

- **Host tests:** `cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host`. All existing tests pass; these units change only Mac code, so they add no host tests.
- **68k build:** set up the toolchain with `tools/setup-retro68.sh` from the shared cache, then build as `CLAUDE.md` describes. The build must have no new warnings.
- **Emulator rows** (Basilisk II with the stress bridge, as `tools/emu/README.md` describes):
  - E1. Connected: Cmd-D from the main window, from a file window with unsaved changes (Cancel, then Don't Save), and in full screen.
  - E2. Not connected: Cmd-D does nothing.
  - E3. Every U2 test scenario.
  - E4. Toggle the checkbox on and off with two tabs open, one scrolled to the middle and one at the bottom; check positions, thumb and rewrap.
  - E5. Relaunch with the bar hidden, and launch with a version 3 prefs file saved by the current release build.
  - E6. With the bar hidden, run every key in the "Moving around the transcript" table and toggle Full Screen and Compact Input.
- **Hardware rows (the person):** Cmd-D and Cmd-Backspace on the real ADB keyboard, and the hidden bar on the SE/30 screen.

## Definition of Done

- R1 to R8 hold, shown by the emulator rows above.
- U1 to U3 are merged in one pull request, and `/mnt/project-files/release/Claude30.bin` and `Claude30.dsk` are rebuilt from it.
- `client/README.md` describes the new keys and the setting.
- No bridge, PROTOCOL.md or tty-mode changes are in the diff, and no abandoned or debug code remains.

### Deferred to Implementation

- The exact name of the new `window.c` functions.
- The final pixel positions of the checkbox and the moved buttons in the Settings dialog.
