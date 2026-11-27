# Code review: PR #20, Find and keys (Phase A)

Five reviewers read the diff against base 4992e2b: correctness, testing, maintainability, project standards and past learnings. No adversarial or cross-model pass (no security, money or data-loss boundary). The merge and validator steps of the review pipeline were not run: I merged the five returns by hand, so findings were not independently validated.

## Fixed in this PR

| Finding | Reviewer | Fix |
|---|---|---|
| Cmd-M while dimmed set the status line but did not beep, unlike the README | correctness, standards | `SysBeep` added |
| A Find string over 63 characters closed the dialog and did nothing | correctness | the string is cut to fit |
| Log window title percentage could overflow for logs over about 21 MB | testing | computed in KB |
| Stale `item_enabled` comment, unneeded `window_find_restarts` export | maintainability | comment fixed, function made static |
| Fold table had only 3 of 28 pairs tested; `find_file_forward` error and guard paths untested | testing | all 28 pairs, idempotence over 256 bytes, forward failure, too-small buffer, `from < 0`, `before > len` |

## Open, not fixed (P2/P3)

- The nearest-fold arbitration in `window_find` is untested and could move to core (`find.c`) with host tests.
- `window_find_token` duplicates the restart detector and can in theory collide; `window.c` is about 2,000 lines and the Find section could become its own file; `c2p` now has more copies. All are structure cleanups.
- A match inside a closed fold's marker line wins over the fold's body, and Find Newer from it skips the body.
- Menu key equivalents (Cmd-F, Cmd-G, Cmd-M) have no automated check.
- Toolbox calls used but not named in the README: TENew, TESetText, TEUpdate, TEClick, TEGetHeight, ZeroScrap, PutScrap, PBGetCatInfoSync, FSMakeFSSpec (all System 7 or earlier).

## Needs a Mac or emulator

Nothing Mac-side has run. Check in particular: a second read path to a log that is still open for writing shows its newest text (stale EOF), a read-only file window near a slice edge keeps its selection after `ro_set_top`, tab slot reuse after a close clears Find state, Window menu numbering with a Log item, Log window scroll and per-tab hide and show.

No recorded lesson in `docs/solutions/` applies; the one entry is about bridge lines and is not touched.
