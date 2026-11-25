---
title: Transcript Folding and Permission Summary Line - Plan
type: feat
date: 2026-11-25
topic: claude30-folding-permsummary
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Transcript Folding and Permission Summary Line - Plan

## Goal Capsule

- **Objective:** On the SE/30's 9-inch screen, the person can skim a Claude30 transcript without long code blocks or bursts of tool lines pushing Claude's answer out of view, and can judge the size of a permission request at a glance before reading its body. Nothing that matters for a yes or no is hidden, and nothing is lost from the log.
- **Means:** Claude30 folds long code blocks and tool runs into one marker line that a click or Cmd-J opens and closes (KTD2, KTD3). The permission dialog computes one bold headline from the request it already receives (KTD10). Client only, no protocol change (KTD1).
- **Product authority:** This Product Contract. PROTOCOL.md v1 and v2 are unchanged.
- **Execution profile:** Two pull requests to `schreibaer/claude30`, client, docs and `tools/emu/stress_bridge.py` only. PR A (permission headline): U2, U5, and its share of U6. It needs nothing from PR #12 or the folding work and can start on `main` now. PR B (folding): U1, U3, U4, and the rest of U6. Waits on: PR #12 merging first (KTD8). If PR A is still open when PR B is ready, PR B rebases on it, since both touch `client/README.md` and `stress_bridge.py`.
- **Stop conditions:** For PR B only, stop and ask if PR #12 (subagent lines, branch `claude/project-thread-sjk6as`) is still open when work starts and its in-place rewrite code has changed shape from what KTD8 describes. Stop and ask if folding forces a change to `PROTOCOL.md` or `bridge.py`.
- **Who finishes:** An implementing agent sets up Retro68 with `tools/setup-retro68.sh`, builds the app, runs the host tests and the emulator rows, and refreshes the release files. The person runs the hardware rows on the real SE/30.
- **Coordination:** PR #12 adds `insert_at`, `pm_shift`, `al_shift` and `RS_AGENT` in `client/src/mac/window.c` and `client/src/core/`. PR B reuses them (KTD8). Branch PR B from `main` after PR #12 merges; if it has not merged, branch from `claude/project-thread-sjk6as` and say so in the PR body.
- **Open blockers:** None.

---

## Product Contract

### Summary

A code block longer than 12 lines collapses into one line such as `[+] Code, 47 lines: def parse(text):` as soon as its 13th line arrives, and the count keeps climbing while the rest streams in. Three or more tool lines in a row collapse into one line such as `[+] 5 tools: Read 3, Grep, Bash`.
Clicking that line, or pressing Cmd-J, opens it in place as `[-] Code, 47 lines: …` with the full text below; doing it again folds it. The log file always receives the full text.
The permission dialog gains one bold line above its body, such as `Edit parser.py: +12 -3 lines in 2 places`, counted on the Mac from the diff the bridge already sends.

### Problem Frame

Claude30's transcript is about 25 lines tall on the SE/30. A code block is drawn line for line in Monaco (`client/src/core/batch.c`, `EV_CODE`), and every `U` line becomes its own italic bullet line (`EV_TOOL`). A 40-line diff or a burst of eight Read and Grep lines therefore scrolls Claude's prose off screen, and the person has to scroll back to find what Claude actually said.
In the permission dialog (`client/src/mac/permdlg.c`), the header (`kPermSummary`) shows only the bridge's one-line summary, such as `Edit src/parser.py`. How big the change is only becomes clear after scrolling the whole body in a 9-point list.

### Key Decisions

- **Fold at the client, never at the bridge.** The bridge already marks code blocks (`C[`/`C]`) and tool lines (`U`), and the diff body it sends is exact. Folding is a display choice for one small screen. Governs R11.
- **A long block collapses when its 13th line arrives, and the count keeps climbing.** (session-settled: user-approved, chosen over keeping the first 12 lines visible with a "35 more lines" marker below them: the folded view is one line instead of thirteen.) Governs R1.
- **The marker stays when a fold is open**, so the same line folds it again. Governs R4.
- **The headline helps the person judge a request; it never replaces the body.** All existing safety rules stay. Where the headline cannot be exact (incomplete request, long Bash command), it says so rather than summarising. Governs R8, R9, R10.

### Requirements

**Folding code blocks**

- R1. A code block of more than 12 lines shows as one marker line with its line count and its first non-blank line, cut to one screen line. It collapses when the 13th line arrives, and the count updates while further lines arrive.
- R2. A code block of 12 lines or fewer looks exactly as it does today.

**Folding tool runs**

- R3. Three or more consecutive tool lines show as one marker line naming how many tools ran and which, in order of first use, with a count for repeats, cut to one screen line. Any other transcript line ends the run: prose, code, an error, a permission line, a prompt, a result, a subagent line, a bridge notice. Runs of one or two tool lines look exactly as they do today.

**Opening and closing**

- R4. Clicking a marker line, or pressing Cmd-J (Session > Open or Close Fold), opens the fold in place with its full text below the marker. Doing it again folds it. The marker shows `[+]` while folded and `[-]` while open.
- R5. Folded text is never lost: the log file receives the full text of every fold, and an open fold copies like any other text.
- R6. Scrolling, the scroll position cues, Cmd-Up and Cmd-Down prompt jumps, subagent lines and background tabs keep working with folds present.

**Permission dialog summary line**

- R7. The permission dialog shows one bold line above the request's header and body, built from the header's tool name and target and from counts taken from the body.
- R8. For Edit, MultiEdit and Write, the counts of added and removed lines are exact for the body the Mac received.
- R9. For Bash, the bold line shows the command only when the whole command fits on that line. Otherwise it says how many lines or characters the command has and points to the body.
- R10. When part of the request could not be shown, the bold line says so and tells the person to deny it. Every existing safety rule of the dialog stays as it is.

**Compatibility**

- R11. No change to the bridge or to PROTOCOL.md. ZTerm (tty mode) behaves as today.

### Acceptance Examples

- AE1. Claude sends a 47-line code block. The first 12 lines stream in as usual; when the 13th arrives, all of them are replaced by the single line `[+] Code, 13 lines: def parse(text):`, which counts up to `47 lines` as the block finishes. Clicking it shows `[-] Code, 47 lines: def parse(text):` with all 47 lines below; clicking again folds it.
- AE2. Claude uses Read, Read, Grep, Read, Bash, then writes prose. The first two tool lines appear as usual; when the third arrives, they are replaced by one line that ends as `[+] 5 tools: Read 3, Grep, Bash`, followed by the prose.
- AE3. Claude uses Read, then writes prose, then uses Grep and Glob. Nothing folds.
- AE4. An Edit request with two hunks adding 12 lines and removing 3 shows the bold line `Edit parser.py: +12 -3 lines in 2 places` above the existing header `Edit src/parser.py` and the full diff.
- AE5. A Bash request with a 4-line command shows `Bash: 4-line command, read it below`. A Bash request with `git status` shows `Bash: git status`.
- AE6. A request whose body ran out of memory shows `Incomplete request: deny it`; Allow and Always stay disabled as today.
- AE7. A tab's transcript grows until a folded 40-line block at the top is trimmed. The log file contains all 40 lines where the block was, not the marker.

### Scope Boundaries

- No setting to turn folding off or change the 12-line or 3-line thresholds; they are constants in the core.
- No Open All or Close All folds.
- No folding of prose, permission lines, or the `?` lines of answered permission requests.
- No change to the permission dialog's buttons, keys or timing.
- Considered and not built: retiring the oldest fold through an early trim when the per-tab cap is reached (plain lines until the normal trim is simpler and loses nothing).

### Deferred to Follow-Up Work

- A Settings checkbox to turn folding off, if the person wants it after using it.
- Folding in the file viewer (`client/src/mac/filewin.c`).

### Sources / Research

- `client/src/core/batch.c`, `batch.h`: how code and tool lines become styled runs; trim thresholds (`TRIM_AT_BYTES` 28,672, `TRIM_KEEP_BYTES` 20,480).
- `client/src/mac/window.c`: `window_append`, `trim` (writes cut text to the log, then shifts prompt marks), `window_click`, `insert_run`.
- `client/src/core/protocol.c`: emits `EV_CODE_BEGIN`, `EV_CODE`, `EV_CODE_CONT`, `EV_CODE_END`, `EV_TOOL`; emits `EV_CODE_END` on reset or link loss, so an unfinished block always ends.
- `client/src/mac/permdlg.c`, `client/claude30.r` (`DITL` 132), `client/src/mac/resources.h` (`kPermSummary` 4, `kPermList` 5, `kPermHint` 6).
- `bridge.py`, `describe_tool`: Edit and MultiEdit bodies come from `difflib.unified_diff(...)[2:]`, which drops the `---`/`+++` header lines and keeps `@@` hunk lines. Write bodies are every content line prefixed `+`. Bash sends a body only when the command has a newline or is longer than 60 characters; its summary is the command's first line.
- PR #12 diff (`origin/claude/project-thread-sjk6as`): `insert_at`, `window_agent_update` (in-place rewrite with an empty clip), `pm_shift`, `al_shift`, `RS_AGENT`.

---

## Planning Contract

### Key Technical Decisions

- KTD1. **No protocol change.** The client already gets block boundaries (`EV_CODE_BEGIN`/`END`) and one event per tool line. The diff body is exact because the bridge strips the `---`/`+++` headers, so every body line starting with `+` is an added line and every one starting with `-` is a removed line, even when the line's own content starts with `++` or `--`. Counting never skips lines that look like headers.
- KTD2. **Folded text lives outside TextEdit, and exactly one place owns it at a time.** TextEdit cannot hide text, so a closed fold's body is cut out of the record and kept in a per-tab fold store, with only the marker line in TextEdit. Opening moves the body back into TextEdit right after the marker and frees the stored copy; closing cuts it out again. This avoids doubling memory and keeps the 32 KB record limit honest.
- KTD3. **Collapse at the threshold crossing.** (session-settled: user-approved, chosen over keeping the first 12 lines visible: one line instead of thirteen.) The fold core remembers the record offset where the current code block or tool run started, recorded when its first line is inserted, so a collapse never depends on counting line breaks (`batch.c` inserts hard breaks into lines over `BATCH_MAX_LINE`). On a code block's 13th line, or a run's 3rd tool line, the window flushes the pending batch, cuts everything from that start offset to the end of the record into a new fold, and puts the marker in its place. Later lines of that block or run go straight to the fold's body, not to the batch, and dirty markers are rewritten in place once per event-loop pass by a refresh that runs even when the batch is empty. If the person has opened a still-growing fold, new lines append to the transcript as usual and extend the open body.
- KTD4. **ASCII markers in the content's own style.** Mac Roman has no triangle glyphs and TextEdit can't hold a drawn icon, so markers start with `[+] ` or `[-] `. A code fold's marker is drawn in the code style (Monaco), a tool fold's marker in the tool style (italic Geneva), so the marker reads as part of what it hides. No new `RunStyle`; a fold is recognised by the fold store's offsets, never by style.
- KTD5. **The log gets the full text.** When `trim` writes cut text to the log, every closed fold whose marker lies in the cut is written as its body instead of its marker line, and the fold is freed. An open fold's text is ordinary TextEdit text and is logged as is; its record is dropped when its marker is cut.
- KTD6. **Bounded memory, nothing dropped.** A closed body holds at most 12 KB (`FOLD_MAX_BODY`). When a growing fold's body would pass that, the window opens it: the body goes back into TextEdit after the marker (`[-]`), later lines stream into the transcript, and they reach the log through `trim` in order. Closing a fold whose body is over the cap beeps and the status line says the block is too large to fold. A tab keeps at most 256 folds and 48 KB of closed bodies (`FOLD_MAX`, `FOLD_MAX_TAB_BYTES`); at either cap, new code blocks and tool runs append as plain lines, as today, until the normal trim frees folds. Worst case across six tabs is under 500 KB of the 3.5 MB partition.
- KTD7. **A portable fold core, host-tested.** A new `client/src/core/fold.c` owns the fold records (marker offset and length, kind, open flag, line count, body bytes, tool-name tally, first line) and the stream rules (count code lines in the current block; count consecutive tool lines; say when to collapse). It follows text edits with `fold_cut` and `fold_shift`, like `PromptMarks`, and composes marker text with a caller-supplied width measure, like PR #12's `al_fit`. The window applies the decisions to TextEdit.
- KTD8. **Build on PR #12's in-place rewriting.** Collapsing, opening, closing and marker updates use `insert_at` and the empty-clip edit pattern from `window_agent_update`, and every edit calls `pm_shift`, `al_shift` and `fold_shift` so prompt marks, subagent slots and other folds keep their offsets. A subagent line (`RS_AGENT`) ends a tool run and is never folded.
- KTD9. **Input.** A plain click on a marker line toggles that fold and does not start a selection; shift-click and clicks elsewhere go to `TEClick` as today. Hit-testing uses `TEGetOffset`. Cmd-J (new Session menu item, Open or Close Fold) toggles the fold whose marker or open body contains the transcript's selection start; failing that, the lowest marker on screen; failing that, it beeps. Opening keeps the marker's screen position and text below moves down; if the opened body would run past the bottom of the view, the view scrolls so the whole body is visible, or so the marker is the top line when the body is taller than the view. Collapsing above the view shifts the view by the removed height, as `trim` does, so what the person is reading stays put.
- KTD10. **The headline is computed in a portable core from what was received.** A new `client/src/core/permsum.c` takes the request header (tool name, then target), the body lines and the incomplete flag, and returns one line. Rules: incomplete wins (`Incomplete request: deny it`); Edit and MultiEdit count `+` lines, `-` lines and `@@` hunks; Write counts `+` lines; Bash shows the command only if it is single-line and fits the measured width, else `Bash: N-line command, read it below` or `Bash: long command (N characters), read it below`; any other tool with a body says `Tool target: N lines below`; no body gives `Tool target`. For Write, a trailing ` (N lines)` on the target is dropped first (`describe_tool` adds it). The target is the last path component for file tools. A subagent label `(type)` after the tool name is kept. The width is measured with a caller-supplied function, and a cut target ends with `…` (Mac Roman `0xC9`); a Bash command is never cut.
- KTD11. **A new dialog item; nothing else in the dialog changes.** The headline is a new `UserItem` (item 7, `kPermHeadline`) drawn in Geneva 12 bold above the existing header text, which moves down; the list loses about one row. Items 1 to 6 keep their numbers. Kept as they are: only Cmd-Y, Cmd-N, Cmd-A and Cmd-. answer; Return, Enter, plain keys and held keys do nothing; clicks are ignored for the first 0.75 s (`ARM_TICKS`); pending mouse and key events are flushed before the dialog shows; an incomplete request disables Allow and Always and their keys; a background tab's request waits for its tab. The headline is disabled and takes no clicks.

### High-Level Technical Design

*Directional sketches, not implementation specification.*

Life of a fold:

```
                 13th code line / 3rd tool line
  streaming  ───────────────────────────────────►  closed, growing
  (plain lines)                                     (marker [+], count rising,
                                                     body in fold store)
                                                         │ C] / run ends
                                                         ▼
           click or Cmd-J                            closed
     open  ◄──────────────────────────────────────►  (body in store)
  (marker [-], body in TextEdit)                         │
       │ marker trimmed                                  │ marker trimmed, or
       ▼                                                 │
  record dropped; text already                          ▼
  went to the log as text                  body written to the log in place of
                                           the marker; record freed
```

One incoming tool or code line:

```
modem.c event ─► fold_feed(tab folds, kind, text)
                    │
      ┌─────────────┼───────────────────────────┐
      ▼             ▼                           ▼
  APPEND        COLLAPSE(n lines)           INTO_FOLD(k)
  batch_event   window_fold_collapse:       fold body grows (or open body
  as today      flush batch, cut last n     appends to TextEdit); marker
                lines into fold k, insert   marked dirty, rewritten once
                marker, shift marks         per event-loop pass
```

Permission headline, as data:

```
header "Edit src/parser.py" + body [@@ …, -a, +b, +c, @@ …, +d] + incomplete=no
   └─► permsum ─► "Edit parser.py: +3 -1 lines in 2 places"
```

### Assumptions

- PR #12 merges before or during this work with `insert_at`, `pm_shift` and `al_shift` as described in KTD8.
- The SE/30's styled TextEdit supports `TEGetOffset` (Inside Macintosh V; present in the SE/30's ROM and System 7).

### Risks & Dependencies

- **Offset drift.** Every TextEdit edit outside `window_append` must shift prompt marks, subagent slots and folds together, or clicks and jumps land on the wrong line. Mitigation: one helper in `window.c` that does the edit and all three shifts; host tests of `fold_shift` and `fold_cut` at the edges (edit inside a marker, cut through an open body).
- **Opening an old, large fold near the trim limit.** Opening adds up to 12 KB; if the trim that makes room would cut the fold itself, opening is refused with a beep and the status line says the block is too far up to open. Covered by an emulator row.
- **Feeding the fold core from every path.** Prompts, markers, notices, subagent lines and PR #12's fallback tool lines reach the batch from several call sites; if any bypasses the fold core, a tool run can fold across a line that should have ended it. Mitigation: one fold-aware wrapper for every batch call (U3).
- **Selection copy of a closed marker** copies the marker text, not the hidden body. Accepted and documented; opening first copies everything.

---

## Implementation Units

### U1. Fold core: records, stream rules, marker text

- **Goal:** Portable fold bookkeeping that decides when to collapse and keeps offsets right as the transcript changes.
- **Requirements:** R1, R2, R3, R5, R6; KTD2, KTD3, KTD4, KTD6, KTD7.
- **Dependencies:** None.
- **Files:** `client/src/core/fold.c`, `client/src/core/fold.h` (new); `client/tests/test_fold.c` (new); `client/tests/CMakeLists.txt`; `client/CMakeLists.txt` (core sources).
- **Approach:** A per-tab `FoldSet` with a fixed array of `FOLD_MAX` (256) records and constants `FOLD_CODE_LINES` (12), `FOLD_TOOL_RUN` (3), `FOLD_MAX_BODY`, `FOLD_MAX_TAB_BYTES`. A feed function takes each transcript-visible event kind and returns append, collapse (with how many earlier lines), or into-fold. Code lines count per block, reset at `EV_CODE_BEGIN`/`END`; continuation lines add bytes, not lines. Tool runs count consecutive `EV_TOOL`; every other visible event ends the run. The core keeps the record offset where the current block or run began. Bodies are `malloc`ed and grown like `Batch`; when a body would pass `FOLD_MAX_BODY`, the feed reports auto-open. Marker composition: `[+] Code, N lines: <first non-blank line, leading spaces dropped>` and `[+] N tools: Name k, Name, …` (names in first-use order, count shown when over 1), fitted with a width callback. `fold_cut`, `fold_shift`, `fold_at(offset)`, `fold_lowest_in(range)`.
- **Test scenarios:**
  - 12 code lines then `C]`: every line appends, no collapse.
  - 13 code lines: line 13 returns collapse(12); lines 14 to 47 return into-fold; count reads 47 at `C]`.
  - `C` then `+` continuation lines: the continuations do not raise the line count.
  - Tool, tool, prose: no collapse. Tool ×3: third returns collapse(2). Tool ×5 then prose: run ends, marker reads `5 tools: Read 3, Grep, Bash` for that input.
  - Tool, subagent line, tool, tool: no collapse (the subagent line ends the run).
  - Link loss mid-block (`EV_CODE_END` from reset): the fold finishes with the lines it has.
  - Body reaches `FOLD_MAX_BODY`: the feed reports auto-open, and later lines of the block return append.
  - A block whose first line was split by a hard break over 4,000 bytes: the collapse covers the whole block from its start offset.
  - `fold_shift` before, inside and after a marker; `fold_cut` through a closed fold (record freed, body reported for the log), through an open fold's marker (record dropped).
  - 257th fold or 48 KB of closed bodies: a new long block or tool run returns append and does not fold.
  - Marker fitting: a long first line is cut with `…` to the given width.
- **Verification:** `ctest --test-dir build-host` passes with `test_fold` added.

### U2. Permission headline core

- **Goal:** One function that turns a request header and body into the bold line, exact where it can be and explicit where it can't.
- **Requirements:** R7, R8, R9, R10; KTD1, KTD10.
- **Dependencies:** None.
- **Files:** `client/src/core/permsum.c`, `client/src/core/permsum.h` (new); `client/tests/test_permsum.c` (new); `client/tests/CMakeLists.txt`; `client/CMakeLists.txt`.
- **Approach:** Input is the header bytes, the body and its line ends (the `PermReq` layout, passed as plain pointers so the core stays Toolbox-free), the incomplete flag and a width callback. Parse the tool name as the first word, keep a following `(type)` word, take the rest as the target. Apply the KTD10 rules. Use ASCII `-` for minus; Mac Roman has no minus sign.
- **Test scenarios:**
  - Edit, two hunks, +12 -3: `Edit parser.py: +12 -3 lines in 2 places`. One hunk: `in 1 place`.
  - Removed line whose content starts with `--` (body line `---x`) counts as removed; added line `+++y` counts as added.
  - Header `Write src/foo.py (40 lines)` with 40 body lines: `Write foo.py: +40 lines`. Header `Write src/foo.py (0 lines)` with no body: `Write foo.py`.
  - MultiEdit with three hunks across edits: counts all three.
  - Bash `git status`, no body: `Bash: git status`. Bash single 140-character line in the body, too wide: `Bash: long command (140 characters), read it below`. Bash 4-line body: `Bash: 4-line command, read it below`.
  - `Bash (general-purpose) git log -5`: the `(general-purpose)` label is kept.
  - Read with no body: `Read parser.py`. Unknown tool with a 7-line body: `Foo target: 7 lines below`.
  - Incomplete with any body: `Incomplete request: deny it`.
  - Empty header: returns a non-empty fallback (`Permission request`).
- **Verification:** `test_permsum` passes under `ctest`.

### U3. Folds in the transcript window

- **Goal:** Apply the fold core's decisions to each tab's TextEdit record, keep the log complete and keep every offset right.
- **Requirements:** R1, R2, R3, R5, R6; KTD2, KTD3, KTD5, KTD6, KTD8.
- **Dependencies:** U1; PR #12.
- **Files:** `client/src/mac/window.c`, `client/src/mac/window.h`, `client/src/mac/tabs.h` (a `FoldSet` per tab), `client/src/mac/tabs.c` (init and free), `client/src/mac/modem.c` (one fold-aware wrapper for batch events, prompts and markers; every existing `batch_event`, `batch_prompt` and `batch_marker` call, including PR #12's subagent lines and fallback tool lines, goes through it so every transcript-visible line reaches `fold_feed`; a `window_fold_refresh(t)` call beside `window_append(t)` in the per-pass loop); `tools/emu/stress_bridge.py` (case `fold`: a 47-line block, a 12-line block, a 5-tool run, a 2-tool run, and a tool run split by prose).
- **Approach:** One edit helper replaces a range of a tab's record with styled text under an empty clip, preserves the selection as `window_agent_update` does, and shifts prompt marks, subagent slots and folds. Collapse: flush the batch with `window_append`, copy the record from the block's or run's start offset to the end into the new fold's body, and replace it with the marker. Into-fold: append to the body, or to the record when the fold is open; mark the marker dirty. `window_fold_refresh` rewrites dirty markers once per pass, even when the batch is empty, and performs any auto-open the core reported. Opening checks the trim first and refuses with a beep and a status line when the fold itself would be cut. Closing cuts the body back into the store. `trim` writes cut text to the log in pieces, substituting each closed fold's body for its marker, then calls `fold_cut`. Collapses above the visible top shift `destRect` by the removed height, as `trim` does.
- **Execution note:** Implement the trim substitution and the offset helper before live collapsing, and check them with the `fold` stress case before wiring clicks.
- **Test scenarios:** (emulator, with the `fold` stress case from U6)
  - A 47-line block collapses at line 13 and ends as `[+] Code, 47 lines: …` with the view still at the end.
  - Five tool lines collapse at the third into `[+] 5 tools: …`; two tool lines stay plain.
  - The same output in a background tab: the tab is marked fresh, and switching to it shows the folds.
  - Cmd-Up and Cmd-Down still stop at each prompt after folds collapse and open above and below them.
  - With PR #12's demo subagent running between tool lines, the subagent line still updates in place and the tools around it do not fold together.
  - Fill the tab past the trim limit; the log file holds full block text where folds were.
  - A block over 12 KB opens itself when it reaches the cap and keeps streaming; Cmd-J on it beeps; the log later holds every line.
- **Verification:** The 68k build succeeds with no new warnings; emulator rows pass.

### U4. Opening and closing: click and Cmd-J

- **Goal:** Let the person open and fold again with one click or one key.
- **Requirements:** R4; KTD9.
- **Dependencies:** U3.
- **Files:** `client/src/mac/window.c` (`window_click`, a `window_toggle_fold` entry), `client/src/mac/window.h`, `client/src/mac/main.c` (menu item handling and enabling), `client/claude30.r` (Session menu item with key `J`, enable mask updated for the new item position), `client/src/mac/resources.h` (item constant).
- **Approach:** In `window_click`, for a plain click in the transcript, map the point with `TEGetOffset`, ask `fold_at` and toggle instead of calling `TEClick` when it hits a marker. Cmd-J applies KTD9's target rule. The menu item is dimmed when the front tab has no folds.
- **Test scenarios:** (emulator)
  - Click a closed marker: it opens below itself and reads `[-]`; click again: it folds.
  - Click the lowest marker on screen with the view at the end: the view scrolls so the opened body is visible.
  - Shift-click on a marker extends the selection and does not toggle.
  - Select text inside an open fold and press Cmd-J: that fold closes. With the caret elsewhere, Cmd-J toggles the lowest marker on screen; with none on screen, it beeps.
  - Open a growing fold while the block still streams: new lines appear below it as they arrive.
  - Copy an open fold: the clipboard holds the full text.
- **Verification:** Emulator rows pass; the Session menu shows Open or Close Fold with Cmd-J.

### U5. Bold summary line in the permission dialog

- **Goal:** Show U2's headline above the existing header without changing how the dialog is answered.
- **Requirements:** R7, R8, R9, R10; KTD10, KTD11.
- **Dependencies:** U2.
- **Files:** `client/src/mac/permdlg.c`, `client/src/mac/permdlg.h`, `client/claude30.r` (`DITL` 132: item 7, moved header and list rects), `client/src/mac/resources.h` (`kPermHeadline`); `tools/emu/stress_bridge.py` (case `edit`: an Edit request with two hunks, +12 -3, including a removed line whose content starts with `--`).
- **Approach:** Compute the headline once when the dialog opens, after `build_rows`, with a width callback that measures in Geneva 12 bold. Draw it from a user item procedure with `TextFont`, `TextFace(bold)` and `TextSize`, then restore the port's text settings. `show_incomplete` also recomputes the headline, which then says to deny. No change to `filter`, `ARM_TICKS`, key handling, `FlushEvents`, or `permdlg_run`'s result mapping.
- **Test scenarios:** (emulator, with the stress cases)
  - `perm` (Write, 2,000 lines): `Write …: +2000 lines`; list still scrolls to the last line.
  - `edit`: headline matches the counts in AE4.
  - `wide` (Bash, 3,000-byte line): `Bash: long command (3000 characters), read it below`.
  - Out-of-memory request: `Incomplete request: deny it`, Allow and Always dimmed, Cmd-Y does nothing.
  - Return, a held key, and a click in the first 0.75 s still do nothing.
- **Verification:** Emulator rows pass; a screenshot of the dialog shows the headline, header and list without overlap at 512 × 342.

### U6. Docs, emulator rows and release files

- **Goal:** Make the feature testable without the SE/30 and documented for the person.
- **Requirements:** R1 to R11.
- **Dependencies:** U5 for PR A's share (headline docs and rows); U3 and U4 for PR B's share (folding docs and rows).
- **Files:** `tools/emu/README.md` (case table and emulator checklist rows), `client/README.md` (folding, Cmd-J, the bold line, hardware rows), `README.md` if it lists keys; `/mnt/project-files/release/Claude30.bin` and `Claude30.dsk` refreshed from the build.
- **Approach:** List the `fold` and `edit` stress cases (added in U3 and U5) in the case table, add their checklist rows, and document folding, Cmd-J and the bold line in `client/README.md`. Each PR carries the docs for its own feature and refreshes the release files.
- **Test expectation:** none beyond running the cases: this unit adds test tooling and docs.
- **Verification:** `python3 tools/emu/stress_bridge.py` lists the new cases under `help`; bridge tests still pass (`pytest tests/`).

---

## Verification Contract

- Host tests: `cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host`, with `test_fold` and `test_permsum` added and every existing test passing.
- 68k build: `tools/setup-retro68.sh`, then `cmake -S client -B build-68k -DCMAKE_TOOLCHAIN_FILE=/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake && cmake --build build-68k`, with no new warnings.
- Bridge tests: `pytest tests/` passes unchanged (no bridge change; the stress bridge only gains cases). `python3 tools/emu/stress_bridge.py` lists `fold` and `edit` under `help`.
- Emulator: the U3, U4 and U5 rows in `tools/emu/README.md`, run against `stress_bridge.py`.
- Hardware (the person): on the SE/30 at 57,600 baud, AE1, AE2, AE4 and AE6, plus scrolling feel while a long block streams in.
- `git diff --stat <base>` shows no change to `bridge.py` or `PROTOCOL.md`, where `<base>` is `main`, or `origin/claude/project-thread-sjk6as` if PR B had to branch from PR #12.

## Definition of Done

- R1 to R11 hold, shown by the host tests, the emulator rows and AE1 to AE7.
- Toolbox calls used are named in the headers' comments (`TEGetOffset`, `TEStyleInsert`, `TEDelete`, `TextFace`, `TextWidth`), and nothing needs more than System 7 on a 68030.
- The PR says what was tested in the emulator and what still needs the SE/30.
- Release files in `/mnt/project-files/release/` are rebuilt from the merged code.
- No dead or experimental code from abandoned approaches is left in the diff.

### Deferred to Implementation

- Exact marker wording when a tool name is longer than the line (cut with `…` either way).
- Whether the dirty-marker rewrite needs throttling beyond once per event-loop pass at 57,600 baud; decide from the emulator.
