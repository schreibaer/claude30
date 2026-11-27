# PR #21 code review: Phase B, commands pass-through and plan mode

Branch `claude/phase-b-commands`, base 904be6a, 22 files, about 665 changed lines. Scope: local-aligned.

## Coverage

Eight independent reviewers ran on the full path: correctness, security, adversarial (in-process, because no other-provider CLI exists here), project-standards, testing, maintainability, api-contract and learnings. Reason for each conditional one: security for slash-command routing and permission-mode switching; adversarial because plan mode and the command allowlist are safety gates; api-contract for the bridge-to-client wire protocol; testing and maintainability for new behavior and size over 200 executable lines; learnings because docs/solutions has a protocol opt-in lesson.

Skipped and disclosed: the merge leaf, the validator batch and the report leaf were not run. I merged the eight returns by hand and checked the findings several reviewers reached independently against the code before fixing. Not run: the Mac app on an emulator or an SE/30; the real Claude CLI in plan mode.

Learnings: the diff follows the recorded opt-in lesson (`cmds` word, `!CMDS`, `@` lines gated through `send_plan`).

## Findings and what was done

| # | Sev | Finding | Reviewers | Status |
|---|-----|---------|-----------|--------|
| 1 | P1 | A standing "always" grant answered requests while plan mode was on, so an edit or Bash call allowed earlier would run behind a plan (`bridge.py` `_can_use_tool`) | correctness, security, adversarial | Fixed: grants are ignored while `cfg.plan` is true; test added |
| 2 | P1/P2 | A plan switch requested during a turn was skipped when a prompt was already queued, or after a follow turn | adversarial, correctness | Fixed: `_apply_pending` applies a waiting plan change first |
| 3 | P2 | After `/compact` the context window became unknown (`UsageMeter.result`) | correctness | Fixed: a reading with no model keeps the window; test added |
| 4 | P2/P3 | PROTOCOL.md said plan mode survives `/edits`; the bridge resets it | api-contract, correctness, security | Fixed: spec reworded |
| 5 | P1 | ExitPlanMode approval branch and `options().permission_mode` untested | testing | Fixed: approve, always and deny cases and option modes tested |
| 6 | P2 | Command allowlist fails open until the CLI's first init lists its commands: a built-in typed first goes out as ` /name` | adversarial, security | Open: relies on the CLI ignoring a leading-space slash; needs a real CLI check |
| 7 | P2 | Permission-mode ternary written four times; chat gating leaked into the prompt loop; `set_commands` pass-through | maintainability | Open: refactor, no behavior change |
| 8 | P2 | bridge.py grew by about 285 lines | maintainability | Open: a separate command-routing module is the owner's call |
| 9 | P2 | No tests for the real session's init/compact paths, the tty `/plan`, `/clear` and `/commands` paths, or a plan toggle during a busy turn | testing | Open |
| 10 | note | Bundled settings-editing skills (update-config) are in the skill allowlist | adversarial | Open: per-tool prompts still apply; worth a decision |
| 11 | note | PROTOCOL.md reference tables omit `!CMDS` and `<n>@` (the new section covers them) | standards | Open |
| 12 | note | Plan Mode clicked while a tab is still opening says "Could not change plan mode"; the item stays checked offline | correctness | Open |

Verification after the fixes: Python suite 232 OK, host ctest 14/14 on the same C code, 68k build clean, menu resource test OK.
