---
title: Fold Each Subagent's Output Under Its Line - Plan
type: feat
date: 2026-11-25
topic: claude30-subagent-output-folds
artifact_contract: ce-unified-plan/v1
product_contract_source: ce-plan-bootstrap
execution: code
---

# Fold Each Subagent's Output Under Its Line - Plan

## Goal Capsule

- **Objective:** On the SE/30, the person can see what each subagent actually did (the tools it ran, what it wrote, its final report) and can tell at a glance which output belongs to which subagent, without that output burying Claude's own reply. This holds for subagents Claude runs in the background too.
- **Means:** The bridge sends a subagent's tool lines and text over the link behind a new opt-in (KTD1, KTD2). Claude30 turns the subagent's existing `»` line into a fold marker, using the folding from PR #13, and keeps the output in that fold (KTD5 to KTD8). A background subagent's output is collected on the bridge and arrives under the line that reports its result (KTD4).
- **Product authority:** This Product Contract. PROTOCOL.md version 2 gains one capability word, one client opt-in line and one session tag. Unknown tags are ignored by both sides, so the version number stays 2.
- **Execution profile:** One pull request to `schreibaer/claude30`: `bridge.py`, `tests/`, `client/src/core/`, `client/src/mac/window.c`, `client/tests/`, `PROTOCOL.md`, `README.md`, `client/README.md`, `tools/emu/`. Waits on: PR #13 (folding, branch `claude/project-thread-p5fh3s`) merging, because the client half extends its `fold.c` (KTD5).
- **Stop conditions:** Stop and ask if PR #13 has changed `fold.c`'s model (closed bodies kept outside TextEdit, `FoldOps.replace` as the only edit path) by the time work starts. Stop and ask if U1's check finds that the Agent SDK does not deliver a background subagent's own messages to the bridge (see KTD4's fallback).
- **Who finishes:** An implementing agent runs `tools/setup-retro68.sh`, builds the app, runs the Python and host tests and the emulator rows, and refreshes `release/` in the project folder. The person runs the hardware rows on the SE/30.
- **Coordination:** Branch from `main` after PR #13 merges. If it has not merged, branch from `claude/project-thread-p5fh3s` and say so in the PR body.
- **Open blockers:** None.

---

## Product Contract

### Summary

Each subagent still shows as one live italic line. As soon as it does something, the line gains a `[+]` in front: `[+] » Explore: Survey the tests (6 tools, Grep)`. Clicking that line, or Cmd-J, opens it, and the subagent's own tool lines and messages appear indented below it, still growing if it is running. Clicking again closes it.
A background subagent's launch line stays as today ("runs on, its result follows"). When it finishes, its result line appears with `[+]` and holds everything it did.
The log file gets the full output. ZTerm and older copies of Claude30 see no change.

### Problem Frame

Since PR #12, a subagent is one line rewritten in place (`client/src/core/agentline.c`), and the bridge deliberately keeps everything the subagent says or runs off the serial link (`AgentTracker` in `bridge.py`, "counted, never sent"). The person sees that an Explore agent ran 14 tools and the first line of its result, but not which files it read or what it reported. When two subagents run at once, there is no way to tell their work apart at all. PR #13 adds folds for long code blocks and tool runs, which gives the transcript a way to hold a lot of text in one line.

### Key Decisions

- **The subagent's own line is the fold.** No second marker line: the `»` line gains `[+]`/`[-]` once it has output. Governs R1, R2.
- **Closed by default, open on demand, even while running.** The fold grows live while closed; opening it while the subagent works shows new lines as they arrive. Governs R2, R3.
- **Background output lands on the result line, all at once.** (session-settled: user-approved, chosen over growing the launch line from an earlier turn while the person is doing something else: the result line is the one they read, and nothing changes in old parts of the transcript.) Governs R5.
- **A cap per subagent of about 8 KB on the link.** (session-settled: user-approved, chosen over sending everything: at 57,600 baud a long report would hold up the main reply.) Governs R6.
- **No nested folds.** Code inside a subagent's output shows as plain indented lines inside its fold. (session-settled: user-approved.) Governs R4.

### Requirements

**What the fold holds**

- R1. Once a subagent has used a tool or written text, its line starts with `[+]` (closed) or `[-]` (open). A subagent that produced nothing looks as it does today.
- R2. The fold holds, in order, one indented line per tool the subagent used (the same tool name and summary the main conversation's tool lines show) and the subagent's messages, ending with its final report.
- R3. A click on the line or Cmd-J opens and closes it, whether the subagent is running or has ended. While open and running, new output appears at the end of its fold, not at the end of the transcript.
- R4. Code blocks inside a subagent's output appear as plain indented lines, never as a fold inside a fold.

**Several and background subagents**

- R5. Two subagents running at once each keep their own output under their own line. A background subagent's output arrives under its result line when it finishes.
- R6. Each subagent sends at most about 8 KB of output; past that, its fold ends with a line saying how many more lines stayed on the host.

**Nothing lost, nothing broken**

- R7. The log file receives each subagent's line followed by its full output (as received), wherever it sits in the transcript.
- R8. Scrolling, prompt jumps, code and tool folds, other subagent lines, background tabs and trimming keep working.
- R9. ZTerm, the v1 app, and Claude30 builds without this feature behave exactly as today.

### Acceptance Examples

- AE1. Claude starts an Explore subagent that runs Glob, Read, Read, Grep and then reports four lines. The line reads `[+] » Explore: Find the parser tests (4 tools, Grep)` while running and `[+] » Explore done, 4 tools: Found 3 test files` at the end. Opening it shows four indented tool lines and the four report lines.
- AE2. Two subagents run at once. Each line's fold holds only its own tool lines, even though their output arrived interleaved.
- AE3. The person opens a running subagent's fold. A new tool line appears inside the fold, above Claude's next reply, and the view does not jump.
- AE4. Claude starts a subagent in the background and the turn ends. Later, the result line appears with `[+]`; opening it shows everything the subagent did. The launch line in the earlier turn is unchanged.
- AE5. A subagent writes a 20 KB report. Its fold holds about 8 KB and ends with `(140 more lines on the host)`.
- AE6. A tab's transcript is trimmed past a closed subagent fold. The log shows the subagent's line followed by its output.
- AE7. ZTerm dials in and runs the same prompt as AE1: it sees the same `[Agent]` start and end lines as today.

### Scope Boundaries

- No change in tty mode (ZTerm) or v1.
- Tool results (file contents a subagent read, command output) are not sent, just as for the main conversation.
- No setting for the cap or for opening subagent folds by default.
- Subagents that fall back to plain tool lines (more than 8 open at once in one tab) get no fold; their output goes to the log only.

### Deferred to Follow-Up Work

- Live output for background subagents on their launch line, if the person wants to watch them.
- Letting an unasked turn (after a background subagent) light the running mark and accept Cmd-period (known gap from PR #12).

### Sources / Research

- `bridge.py`: `AgentTracker` (start, step, end, end_result, background dict), `ClaudeSession._handle` (subagent `AssistantMessage`s carry `parent_tool_use_id`; their `ToolUseBlock`s already go to `ag.step`), `_system` (`task_started`, `task_progress`, `task_notification`), `Channel.agent_start/step/end` (J+/J~/J-), `app_text_lines` (fence state kept on the object passed in), `split_payload`.
- `AgentTracker.step` already counts tool names for background subagents, so their `AssistantMessage`s reach the bridge while they run (verified by U1 against the live SDK before relying on it).
- PR #13 `client/src/core/fold.h`/`fold.c`: `Fold` (off, len, open, open_len, body, growing), `fold_toggle` refuses growing folds (`FT_BUSY`), `fold_log` writes a closed body in place of its marker, `FOLD_MAX_BODY` 12 KB, `FOLD_MAX_BYTES` 48 KB per tab.
- PR #13 `client/src/mac/window.c`: `fold_replace_cb` (inserts in `RS_CODE` or `RS_TOOL` style, then shifts prompt marks and subagent slots), `window_agent_update` (rewrites a line in place and calls `al_shift`, `fold_shift`), `fold_flip`, `window_toggle_fold_key`.
- `client/src/core/agentline.h`: `AL_SLOTS` 8, `AgentSlot` (off, len, drawn, gone), `al_start`/`al_step`/`al_end` compose the text.
- `PROTOCOL.md`, Subagents section: J lines, `background` status, unasked turns.

---

## Planning Contract

### Key Technical Decisions

- KTD1. **Opt-in on both sides.** `#OK` gains the word `agentout`; a client that sent `!AGENTS` and saw `agentout` sends `!AGENTOUT`. Without it, the bridge sends exactly what it sends today. This keeps older apps from paying link time for lines they would ignore.
- KTD2. **One new session tag, `<n>J= <k> <kind> <text>`.** `<kind>` is `U` (a tool: name, space, summary, as in `<n>U`) or `T` (one line of the subagent's text). The bridge cuts text lines to at most 180 bytes at a word boundary, so a `J=` line never needs a continuation. `J=` lines for a subagent come after its `J+` and before its `J-`. Code fences are dropped and code lines are sent as `T` with two extra spaces of indent (R4); each subagent keeps its own fence state, so a subagent's open fence never changes how the main conversation's text is tagged.
- KTD3. **The report is the subagent's own last message.** The bridge forwards every `TextBlock` in a subagent's `AssistantMessage`s, so its final report arrives through the same path. Only when a subagent ended without sending any text does the bridge send the Agent tool result's text (foreground) or the notification's summary (background) as `T` lines. This avoids sending the report twice.
- KTD4. **Background output is held on the bridge.** (session-settled: user-approved, chosen over growing the launch line.) While a background subagent runs, its `J=` lines go into a list on its `AgentTracker.background` entry instead of the link. When it ends, the bridge sends the fresh `J+`, then the held lines, then `J-`, in one go. Fallback if U1 finds the SDK sends no messages for background subagents: send the notification's summary as the fold's text, and say so in the PR.
- KTD5. **Extend PR #13's fold core with a third kind, `FK_AGENT`.** A subagent fold's marker is the subagent's existing line, which `agentline` keeps composing. The fold core owns only the first four bytes of that line (`[+] ` or `[-] `), the body and the open state. Closed: lines are added to the body and nothing in TextEdit changes. Open: each line is inserted right after the fold's open text (`off + len + open_len`) through `FoldOps.replace`, and later folds, prompt marks and subagent slots move as they do for any fold edit. Unlike code and tool folds, an `FK_AGENT` fold may be opened and closed while it is still growing.
- KTD6. **Composing the line.** `AgentSlot` gains the id of its fold (0: none). The window passes a prefix (none, `[+] `, `[-] `) when it composes or rewrites a slot's line, and after any rewrite tells the fold core the marker's new length. Once a slot ends and is reused, its fold keeps its own offsets and only the bracket ever changes again.
- KTD7. **Body lines in the tool style, indented four spaces.** `FoldOps.replace` inserts one style per call, and the SE/30's screen gains little from mixing fonts inside a fold. Tool lines get the same bullet as the main conversation's tool lines (`\xA5`), text lines none. No new `RunStyle`.
- KTD8. **Memory and trimming follow PR #13's caps.** A closed body holds at most 12 KB (`FOLD_MAX_BODY`), above the bridge's 8 KB cap. When a body cannot grow (that cap, or the tab's 48 KB, or memory), the fold ends with one `(more in the log)` line and later lines of that subagent go only to the log, prefixed with its type in brackets. `fold_log` writes an `FK_AGENT` fold's marker line and then its body, because the line itself says which subagent it was. Output for a subagent whose line was trimmed away, or which fell back to a plain tool line, goes to the log the same way.
- KTD9. **No new Toolbox calls.** All edits go through PR #12's `insert_at` and PR #13's `fold_replace_cb` (TextEdit `TESetSelect`, `TEDelete`, `TEInsert`, `TESetStyle` as today). Nothing needs a newer System. Worst-case memory per tab is unchanged at 48 KB of closed bodies.

### High-Level Technical Design

Directional only, not an implementation specification.

Wire, foreground then background (session 2):

```text
bridge                                   Claude30
2J+ 1 Explore Find the parser tests  ->  new » line, slot 1
2J= 1 U Glob tests/**/*.py           ->  fold created, line gets "[+] "
2J= 1 U Read tests/test_parse.py     ->  body grows (or inserted, if open)
2J~ 1 2 Read tests/test_parse.py     ->  line rewritten, prefix kept
2J= 1 T Found 3 test files:          ->  body grows
2J- 1 done 2 Found 3 test files:     ->  line final, fold stops growing
...
2J+ 2 Explore Survey the tests       ->  (background launch, no J= sent)
2J- 2 background 0 runs on, ...
   (bridge holds its J= lines)
2J+ 3 Explore Survey the tests       ->  later: fresh line
2J= 3 U Glob ... / 2J= 3 T ...       ->  all held lines at once
2J- 3 done 9 Found 12 test files
```

States of an `FK_AGENT` fold:

```text
(no fold) --first J=--> growing+closed <--toggle--> growing+open
                             |  J- or turn end          |  J- or turn end
                             v                          v
                        ended+closed   <--toggle-->  ended+open
any state --body full--> "(more in the log)" added, further lines to log only
any state --marker trimmed--> gone (closed body logged after the line)
```

### Implementation Units

- [ ] U1. **Bridge: send subagent output (foreground)**

  **Goal:** A client that sent `!AGENTOUT` gets each foreground subagent's tool lines and text as `J=` lines. Covers R2, R4, R6, R9.
  **Dependencies:** None.
  **Files:** `bridge.py`, `tests/test_bridge_agents.py`, `tests/test_bridge_v2.py`, `tests/v2client.py`.
  **Approach:** Per KTD1 to KTD3. `AgentTracker` entries gain an output byte count, a dropped-line count, a fence flag and a "sent text" flag. `_handle` passes subagent `TextBlock`s and `ToolUseBlock`s to new tracker methods; the tracker calls a new terminal method `agent_out(k, kind, text)`, which only `Channel` with `agentout_optin` implements (no-op for `Terminal`). At the 8 KB cap the tracker stops sending and counts; before `J-` it sends `(<n> more lines on the host)` when any were dropped, and the KTD3 fallback report when no text was sent. `DemoSession._agents_turn` gains a few fake tool and text lines per subagent, interleaved, one with a code fence. First, run a short real-SDK check (one foreground and one background Explore agent) and record whether background subagents' `AssistantMessage`s arrive; note the result in the PR.
  **Test scenarios:** With opt-in, a subagent's Glob and Read produce `J= k U Glob …` and `J= k U Read …` between its `J+` and `J-`. Without opt-in, the output is byte-for-byte today's. Two interleaved subagents' lines carry their own `k`. A 300-byte text line arrives as two `J=` lines of at most 180 bytes. A subagent's code fence yields indented `T` lines and does not change the main conversation's next `T`/`C` tags. A 20 KB report stops near 8 KB and ends with the "more lines" line. A subagent with no text gets the tool result's text. `#OK` contains `agentout`. tty mode output is unchanged.
  **Verification:** `python -m pytest tests` passes.

- [ ] U2. **Bridge: background output under the result line**

  **Goal:** A background subagent's output arrives between its fresh `J+` and its `J-`. Covers R5.
  **Dependencies:** U1.
  **Files:** `bridge.py`, `tests/test_bridge_agents.py`.
  **Approach:** Per KTD4. In `end_result`'s background branch the running entry, which moves to `background`, starts holding its `agent_out` lines; `end()` for a background entry sends `J+`, the held lines (plus the "more lines" or fallback line), then `J-`. The demo background subagent (`_bg`) produces held lines.
  **Test scenarios:** A demo background subagent sends no `J=` before its launch `J- background`; at its end the link shows `J+`, its `J=` lines, `J-`, then Claude's unasked turn. The same lines arrive when its end comes during a later turn of the same session. Closing the session while it runs sends nothing more and frees the held lines.
  **Verification:** `python -m pytest tests` passes.

- [ ] U3. **Client core: protocol**

  **Goal:** Claude30 opts in and turns `J=` into an event. Covers R9.
  **Dependencies:** None (wire format from KTD2).
  **Files:** `client/src/core/protocol.c`, `client/src/core/protocol.h`, `client/tests/test_protocol.c`.
  **Approach:** Recognise `agentout` in `#OK`; send `!AGENTOUT` right after `!AGENTS` when both words were seen. `<n>J=` emits a new event with session, `k`, kind and text; malformed payloads are ignored.
  **Test scenarios:** `#OK … agents agentout` makes the client send `!AGENTS` then `!AGENTOUT`; `agentout` without `agents` sends neither. `2J= 3 U Read a.c` gives the event with k 3, kind U, text `Read a.c`. `2J= x` and `2J= 3 Z hi` are ignored. An unknown `J` tag is still ignored.
  **Verification:** host tests pass (`ctest --test-dir build-host`).

- [ ] U4. **Client core: agent folds**

  **Goal:** The fold core keeps a subagent's output and opens and closes it. Covers R1, R3, R7, R8.
  **Dependencies:** PR #13.
  **Files:** `client/src/core/fold.c`, `client/src/core/fold.h`, `client/tests/test_fold.c`.
  **Approach:** Per KTD5 and KTD8. New calls, by intent: start a fold on an existing line (returns an id; puts `[+] ` at the line's start), add one line to a fold by id, end a fold (stops growing), tell it its marker's new length. `fold_toggle` accepts a growing `FK_AGENT` fold and rewrites only the four-byte bracket plus the body. `fold_log` writes marker then body for `FK_AGENT`. Adding a line when the body is full appends the `(more in the log)` line once and returns a code telling the caller to log the line instead.
  **Test scenarios:** Starting a fold on line "» Explore: x" makes it "[+] » Explore: x". Three lines added while closed leave the transcript unchanged and the body at three lines. Opening shows "[-] » Explore: x" and the three lines below; a fourth line added while open lands after the third and before the text that follows, and a code fold after it moves by the inserted length. Closing while still growing works and restores the transcript. Two agent folds interleaved keep their own lines. A body filled to 12 KB gets one "(more in the log)" line and further adds return the log code. `fold_log` over a closed agent fold writes the line, then the body. `fold_cut` past the marker forgets the fold; a later add by its id returns "gone". Existing code and tool fold tests still pass, and a growing code fold still refuses to toggle.
  **Verification:** host tests pass.

- [ ] U5. **Client core: subagent line prefix**

  **Goal:** The subagent line keeps its `[+]`/`[-]` across rewrites. Covers R1.
  **Dependencies:** None.
  **Files:** `client/src/core/agentline.c`, `client/src/core/agentline.h`, `client/tests/test_agentline.c`.
  **Approach:** Per KTD6. `AgentSlot` gains a fold id. Composition takes a prefix; `al_fit` keeps the prefix and the two-space indent and cuts the rest.
  **Test scenarios:** A started slot with prefix `[+] ` composes `[+] » Explore: Find…`; a step and the end keep it. Fitting a long line to a narrow width keeps the prefix whole. A slot with fold id 0 composes exactly as today.
  **Verification:** host tests pass.

- [ ] U6. **Window: wire it together**

  **Goal:** `J=` events grow the right fold and the line shows its state. Covers R1 to R5, R7, R8.
  **Dependencies:** U3, U4, U5.
  **Files:** `client/src/mac/window.c`, `client/src/mac/window.h`, `client/src/mac/main.c` (event dispatch), `client/README.md`.
  **Approach:** Per KTD6 to KTD8. On a `J=` event: find the open slot with that `k`; with no slot (plain fallback, trimmed, unknown), log the line with its type and stop. Format the line (four spaces, bullet for `U`), start the fold on the first line, then add. On each slot rewrite (`window_agent_update`), compose with the fold's prefix and report the new length. On `J-` and turn end, end the slot's fold. Clicks and Cmd-J already reach every fold through `fold_marker_at` and `fold_containing`; opening a running fold keeps the view as `fold_flip` does today, and a line inserted into an open fold above the view shifts the view like `trim` does so what the person reads stays put. Background tabs take the same path; only the front tab redraws.
  **Test scenarios:** Covered by host tests for the cores and by U7's emulator rows. `Test expectation: none -- Toolbox glue; behaviour checked in the emulator.`
  **Verification:** `cmake --build build-68k` builds with no new warnings.

- [ ] U7. **Docs, stress case and emulator rows**

  **Goal:** The protocol is specified and the feature can be exercised without Claude. Covers R6, R9.
  **Dependencies:** U1 to U6.
  **Files:** `PROTOCOL.md` (capability list, link-level client lines, session lines table, Subagents section), `README.md`, `tools/emu/stress_bridge.py`, `tools/emu/README.md`.
  **Approach:** Document `agentout`, `!AGENTOUT`, `J=`, the 8 KB cap, background holding, and that v2 is unchanged for clients that do not opt in. Add a stress case `agentfold`: two interleaved subagents with output, one past the cap, then a background one whose output arrives later. Add emulator rows for AE1 to AE6.
  **Test scenarios:** `stress_bridge.py agentfold` runs against the built app in the emulator and every row's expected result holds.
  **Verification:** the emulator rows in `tools/emu/README.md` pass; the release files in `/mnt/project-files/release/` are refreshed.

### Risks

- **The SDK may not stream background subagents' messages.** U1 checks this first; KTD4's fallback still gives a fold with the summary.
- **Link time.** Eight 8 KB subagents in one turn is 64 KB, about 11 seconds at 57,600 baud, shared with the main reply. Tool lines dominate typical runs and are small; the cap bounds the rest.
- **Mid-transcript inserts into open folds** are new for the fold core (PR #13 only appends at the end or replaces a fold in place). Offsets of later folds, prompt marks and subagent slots are the likely bugs; U4's tests cover a fold after the insert point and two interleaved agent folds.

---

## Verification Contract

- `python -m pytest tests` passes, including new cases in `tests/test_bridge_agents.py`.
- Host tests pass: `cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host`.
- The 68k app builds: `cmake --build build-68k` after `tools/setup-retro68.sh`.
- Emulator rows for AE1 to AE6 in `tools/emu/README.md` pass with `stress_bridge.py agentfold`.
- ZTerm check (AE7): `bridge.py` in tty mode with the demo session shows the same `[Agent]` lines as on `main`.
- Not testable here: the real SE/30 (the person runs the hardware rows), and real background subagents end to end beyond U1's one-off SDK check.

## Definition of Done

- U1 to U7 done, with the PR body saying what was tested and what was not, and the result of U1's SDK check.
- PROTOCOL.md describes `agentout`, `!AGENTOUT` and `J=`; `#OK` lists `agentout`.
- No change in tty mode or for clients that do not send `!AGENTOUT`.
- Release files refreshed in the project folder.
