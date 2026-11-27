# PR #23 code review: Phase D, Changes window

Branch `claude/phase-d-changes`, stacked on Phase C (#22). Scope: local-aligned, diff against the Phase C branch, about 740 changed lines (bridge, C client, tests, docs).

## Coverage

Eight independent reviewers ran on the full path: correctness, security, adversarial (in-process, no other-provider CLI here), project-standards, testing, maintainability, api-contract and learnings. Security and adversarial were chosen because the bridge now runs git on a folder an unauthenticated serial client can name.

Skipped and disclosed: the merge leaf, validator batch and report leaf were not run; I merged the eight returns by hand and reproduced the serious ones in tests before fixing. Not run: the Mac window, menu and double-click on an emulator or SE/30.

## Findings and what was done

| # | Sev | Finding | Reviewers | Status |
|---|-----|---------|-----------|--------|
| 1 | P1 | A nested repository or submodule's own `.git/config` filter ran a program during `git status` / `git diff` (reproduced by the reviewer) | security, adversarial | Fixed: `--ignore-submodules=all` on status, numstat and diff; test with a planted clean filter |
| 2 | P1 | A `.git` file or `core.worktree` pointing at another repository made `:diff` return that repository's blobs under a visible name | security, adversarial | Fixed: the git dir must lie inside its work tree and the project inside that tree, else `not git`; inherited `GIT_DIR`, `GIT_WORK_TREE`, `GIT_INDEX_FILE` and similar are removed from git's environment; test added |
| 3 | P1 | An empty untracked file made `_diff_text` raise IndexError, so the window hung in Loading | correctness, adversarial | Fixed: "New file: x (empty)"; test added |
| 4 | P2 | A rename from `.env` or an ignored name printed it as `(from .env)`; names outside the project subfolder leaked | security, adversarial, correctness | Fixed: the old name goes through the same visibility check, or is dropped; test added |
| 5 | P2 | A read from line 0 (retry, prepend near the top, Home) took a new snapshot that the client spliced onto old text | correctness, api-contract | Fixed on the client: a later piece with another token is dropped with a note. A miss in the bridge's cache for a later piece is now refused as `changed` rather than rebuilt. Spec updated |
| 6 | P2 | `:changes` read every untracked file (up to 8 MB each) on the event loop and could spawn thousands of `check-ignore` calls | security, correctness, api-contract | Fixed: counts read in a worker thread, first MB only; at most 500 files listed (1,500 status entries scanned) with a note line |
| 7 | P2 | A rename's diff showed the whole file as added while the list said `+0 -0` | correctness | Fixed: both names are in the pathspec; test added |
| 8 | P2 | git stderr was read after stdout, so a flood of warnings stalled until the timeout | security, api-contract, adversarial | Fixed: drained concurrently and capped; the filter-config probe is killed on timeout |
| 9 | P2 | A cut `status` or `numstat` was silently used; a fragment could parse as a path | security, correctness | Fixed: the last token is dropped and the list says more changed; a cut diff ends with a line saying so |
| 10 | P2 | `_diff_text` trusted the status of the last list: stale visibility, and `open()` followed a swapped symlink | security, adversarial, correctness | Fixed: visibility is checked again, files open with `O_NOFOLLOW` and a regular-file check. Still open: a path that turns from untracked to tracked after listing shows as "New file" until the next `:changes` |
| 11 | P2 | Client parser stripped `(from ...)` from any name that looked like one, opening the wrong file | adversarial, testing, correctness | Fixed: only when XY holds R or C; tests added from both sides |
| 12 | P2 | Writes to `:changes` answered `bad name` in `rw` mode, spec said `read-only` | api-contract, standards | Fixed in the bridge; test added |
| 13 | P2 | No test for the `changes` word on the client; clipped-line test was vacuous | testing | Fixed: `test_changes_capability`, real clipped-line rows |
| 14 | P2 | Gaps: names with spaces, quotes, accents, sorting, binary untracked file, nested/long list | testing | Fixed with new tests (11 to 21 in `test_bridge_changes.py`) |
| 15 | P3 | Empty tree hard-coded as SHA-1; any `rev-parse HEAD` failure counted as "no commits" | adversarial, correctness | Fixed: `git hash-object -t tree /dev/null`; only a plain failure means unborn, a timeout is re-raised |
| 16 | P3 | A second open of an open snapshot window did not refresh it; alerts used the bare name instead of the title; double-click state was static and survived Revert | adversarial, maintainability | Fixed: opening again takes a new snapshot, alerts use the title, click state lives in the window record and Revert resets it; offline double-click beeps |
| 17 | P3 | Docs: `bad name` list, mode table, compatibility note, silent cut, memory claim wording, `STATUS_LINE` dead code | standards, api-contract | Fixed |
| 18 | P3 | Three copies of the git hardening flags | maintainability | Partly fixed: one `git_env()` for all three call sites. The flag lists in `git_ignored` and `git_run` still differ on purpose (`git_ignored` takes stdin) |
| 19 | note | Names Mac Roman cannot spell look the same in the list and cannot be opened; two such names collide | adversarial | Open, low impact: the open fails with `not found` rather than opening a wrong file |
| 20 | note | `bad_name` now hides host files named `:x` from every client | api-contract, learnings | Open by design: Mac names cannot hold a colon; documented |
| 21 | note | Python writer and C parser of the status line share no fixture file | learnings, testing | Open: cross-reference comments on both sides; same pattern as PR 22 #15 |
| 22 | note | Read-only TextEdit selection highlight, double-click timing and the low-memory read at 0x02F0 are unverified on hardware | learnings, standards | Open: needs the emulator/hardware rows |
| 23 | note | One reviewer said the menu mask enabled Plan Mode wrongly; the mask test and a hand check show it is correct | adversarial | No change |

Verification after fixes: Python suite 280 OK (21 tests in `test_bridge_changes.py`), host tests 15 of 15 (including `test_files`, `test_protocol` and the end-to-end bridge session), menu resource test OK, 68k build clean.

## Learnings to record after merge

- A read-only capability that reuses existing line types needs only the `#OK` word (add `cmds`, `rules`, `changes` to the opt-in lesson).
- Anything that serves generated text in pieces must serve a later piece from the snapshot or fail it; never rebuild mid-read.
- Running git on a folder you do not control: scrub `GIT_*` environment, check the git dir sits inside the work tree, skip submodules, and blank filters.
