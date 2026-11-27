# Code review: 115,200 baud (focused review)

Scope: commits 1d1404d and d4944d2 against `origin/main` (e274bc9), plan `docs/plans/2026-11-27-0915-feat-115200-baud-plan.md`. 19 files, 181 executable non-test lines. No full-review floor applied, so the review ran focused: one correctness read in this context plus one independent adversarial read of the hardware-risk surface (`client/src/mac/serial.c`, `client/src/core/speedroute.c`).

Coverage limits: the cross-model peer needs another provider's CLI, which this machine doesn't have, so the adversarial lens ran as a separate local reviewer from the same model family. It corroborates but isn't a cross-model check. No validator ran. The DIRECT and REFUSE paths in `serial_open` can't run on the host; only Basilisk II or the real SE/30 can exercise them. No criteria files (`CODING_STANDARDS.md`, `REVIEW.md`) exist; `CLAUDE.md` rules were checked by hand (Toolbox calls named in the file header, bridge unchanged, protocol unchanged).

Plan requirements: R1-R9 are all implemented. R7's live per-second figure on the link test window is not built (the plan marked it optional); the summary line carries it. The route is named on the status line and in the link test text, not as a transcript note (the plan said transcript note; a note needs a tab, which may not exist at first connect).

## Findings

| # | Severity | File | Finding | Outcome |
|---|---|---|---|---|
| 1 | P3 | client/src/mac/serial.c | The refusal alert hid the driver error and machine type, so a first run on the SE/30 couldn't tell "driver answered something other than -17" from "wrong machine". | Fixed: the alert now ends "(driver error N, machine M)". |

## Residual risks raised and what was done

| Risk | Outcome |
|---|---|
| `ints_off` wrote SR as the literal `0x2700`, which also clears the 68030 M bit. | Fixed: it now uses `ori.w #0x0700,%sr` (checked in the disassembly), which changes only the interrupt mask. |
| `scc_base()` trusted SCCWr with no check. | Fixed: a null or odd pointer writes nothing and takes the refuse path. |
| The status line and link-test text printed the chosen speed, not the speed the open port uses. | Fixed: both now read the open port's speed (`serial_open_speed`). |
| A third-party driver that returns noErr for csCode 115 would leave the port at 57,600 under a "(driver)" label. | Not fixed. Only the hardware run can show what the SE/30's driver does; the route label on the status line makes it visible. |
| That SerReset on the SE/30 rewrites WR4/WR11/WR14 on Disconnect is an assumption. | Not fixed. Covered by the hardware checklist row "Disconnect then Connect at 57,600 without quitting". |
| `resources.h` said the 115,200 item is "drawn first". | Fixed: the comment now says it is the last in the list and placed first on screen. |

## Confirmed by the review

- Register addressing (channel A control at SCCWr+2, B at +0) and the Point High encoding for WR11 and WR14 match the Z8530 and Mac conventions. The bytes 0x84, 0x00, 0x00 are consistent with the driver's own x64 RTxC mode at 57,600 against x32 at 115,200.
- WR3, WR5 and the interrupt registers are never written, so DTR and RTS are untouched.
- The REFUSE path closes both drivers and detaches the input buffer, with no leak. `kSpeedUnavailableErr` (-30000) comes only from that path and is handled only in `open_port`.
- An older build reading a preferences file with speed 3 clamps to 57,600.
- Item 15 in the Settings dialog works because dispatch goes through `kSpeedItems`.

## Testing gaps (accepted)

- No test covers `prefs_load` clamping speed 3; the older-build behavior was read from the code.
- Nothing on the host confirms the SE/30 ROM driver returns exactly -17 for csCode 115. If it doesn't, the app refuses and shows the error number, so the hardware run settles it.

## Verdict

Ready with the fixes above, which are applied. Hardware rows in `tools/emu/README.md` and the U5 run on the real SE/30 are still to do.
