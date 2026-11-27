# PR #22 code review: Phase C, permanent allow rules

Branch `claude/phase-c-rules`, stacked on Phase B (#21). Scope: local-aligned, diff against the Phase B branch, about 1,340 changed lines.

## Coverage

Eight independent reviewers ran on the full path: correctness, security, adversarial (in-process, no other-provider CLI here), project-standards, testing, maintainability, api-contract and learnings. Security and adversarial were chosen because a saved rule is a standing grant over an unauthenticated link. api-contract covers the wire protocol (`rules`, `!RULES`, `Q+`, `r` answers, `/allow`).

Skipped and disclosed: the merge leaf, validator batch and report leaf were not run; I merged the eight returns by hand and reproduced the headline bypasses before fixing. Not run: the Mac dialogs on an emulator or SE/30, and the real CLI's own rule matching.

Learnings: the diff follows the opt-in lesson (`rules` word only with the flag, `!RULES`, `Q+` only after it). Suggest adding `cmds` and `rules` to that lesson's list after merge.

Real-CLI spike (SDK 0.2.172): the CLI's suggestion is the exact command (`Bash(touch spike.txt)`), and an allow with a `localSettings` `addRules` update wrote `.claude/settings.local.json`.

## Findings and what was done

| # | Sev | Finding | Reviewers | Status |
|---|-----|---------|-----------|--------|
| 1 | P1 | Glob characters in a file rule (`Edit(.g?thub/**)`, `Edit(.[c]laude/**)`) slip past the protected-path list and grant standing writes to `.github/`, `.claude/`, `.git/` | correctness, security, adversarial | Fixed: file rules refuse `? [ ] { }`, and `*` may only be in the last path part; bridge and client, fixtures added |
| 2 | P1 | A Bash rule holding `*` or `?` mid-string would be saved as a CLI wildcard | security | Fixed: only a final `:*` is allowed |
| 3 | P1 | `Q+` sent an unvalidated suggestion: a newline injects wire lines, over 200 bytes is dropped | api-contract | Fixed: a suggestion that could not be saved is never offered; tests added |
| 4 | P2 | A bare `Read` rule could be saved (reads every file, secrets included) | security, adversarial | Fixed: bare Read refused |
| 5 | P2 | Non-Bash tools with no CLI suggestion offered a bare tool name | api-contract | Fixed by #3: no offer, so no Rule... button |
| 6 | P2 | Shell denylist was case-sensitive | security | Fixed for case; still a defense-in-depth list only (ssh, make, git -c are not on it) |
| 7 | P3 | Client accepted `Edit(.claude)` and `Edit(.ssh)` that the bridge refused; a tool named only `_` | correctness | Fixed in `rules.c`, fixture rows added |
| 8 | P2 | Tests: missing refusal rows (`&&`, `>`, `<`, secret paths), word-boundary check, saved-notice, vacuous wait | testing, maintainability | Fixed |
| 9 | P2 | "Rule not saved." was shown for a rule that landed late | adversarial | Fixed: the wording says it could not confirm |
| 10 | P2 | `/allow remove N` deletes by position; a changed file removes a different rule than listed | adversarial | Open: the list is re-read at remove time but not compared |
| 11 | P3 | A symlinked settings.local.json is replaced by a regular file on remove | adversarial | Open |
| 12 | P2 | `rule_matches` and `rule_within` only know the older `Tool(prefix:*)` form; a CLI that proposes `Bash(git status *)` would make an unchanged Save Rule fail | correctness | Fixed after testing on the SE/30: Claude Code 2.1.332 proposes `Bash(npm test *)`, which the check refused, so the Rule button never appeared. Both sides now read a final ` *` as the prefix wildcard; fixture rows and a session test added |
| 13 | P2 | `/allow` restart during a running turn is not guarded by an idle check | adversarial | Open |
| 14 | P2 | /allow logic duplicated between tty and Channel; rules block adds 300 lines to bridge.py | maintainability | Open |
| 15 | note | Rule check exists in two copies (Python and C) tied together only by the shared fixtures | testing | Open: by design in the plan |
| 16 | note | The Rule dialog and its live reason line have never run on an emulator or SE/30 | testing, correctness | Open: needs the emulator/hardware rows |

Verification after fixes: Python suite 259 OK, host tests (including `test_rules`, 145 checks, `test_protocol` and the end-to-end bridge session), 68k build clean.
