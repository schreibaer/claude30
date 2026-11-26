---
title: "New bridge-to-client lines in PROTOCOL v2 need a client opt-in, not a #HELLO word"
date: 2026-11-26
category: architecture-patterns
module: PROTOCOL.md v2 capability handshake (bridge.py V2, client/src/core/protocol.c)
problem_type: architecture_pattern
component: messaging
severity: medium
applies_when:
  - Adding a capability to PROTOCOL.md v2 without a version bump
  - The bridge would start sending a line an older Claude30 build has never seen
tags: [protocol, capability, handshake, backward-compatibility, flow-control, ack]
---

# New bridge-to-client lines in PROTOCOL v2 need a client opt-in, not a #HELLO word

## Context

The `models` capability (PR #10) added `M[` / `M` / `M]` lines that the bridge sends to every v2 client right after `#OK`, and PROTOCOL.md said v2 was "unchanged for every older client". The PR #10 code review showed that this is not true. A Claude30 build from before `models` counts each of those lines as a stray link line (`p->dropped++`, `p->noise = 1` in `protocol.c`). Later, when the link goes quiet during a turn, that noise flag makes it send one spare `!ACK` (`proto_tick`), and the spare `!ACK` widens the bridge's `W` window by one.

The obvious repair, "let the new client add a word to `#HELLO`", does not work. Every bridge so far matches the handshake exactly (`line.strip() == "#HELLO SE30 2"` in `bridge.py`), so a client that sends `#HELLO SE30 2 models` is read as a tty prompt by every older bridge. PR #11 found this, but that PR is still open, so the reasoning was in no merged file.

## Guidance

Each direction of the protocol has its own rule:

- **Client to bridge:** a new session tag or link line is safe once the client has seen the capability word in `#OK`. Older bridges ignore unknown tags (`<n>M`, `<n>V` presets work this way).
- **Bridge to client:** a new line needs two things. The bridge advertises a word in `#OK`, and the client opts in with a link-level `!WORD` once, after the project list. The bridge sends the new lines only after that. `agents`/`!AGENTS`, `md`/`!MD` and `usage`/`!USAGE` follow this rule. Without the opt-in, ZTerm, v1 and older v2 builds see exactly the lines they saw before.
- Leave `#HELLO SE30 2` as it is. If a client must announce something before `#OK`, that needs a protocol version bump.

## Why This Matters

Older Claude30 builds stay on the SE/30 after a bridge upgrade, because the person copies a new build over by ZMODEM. On that machine an unknown line costs more than a few wasted bytes: it marks the link noisy and changes the flow control the 3-byte serial chip depends on.

## Known exception

The `models` list is still sent to every v2 client (`send_models()` in `V2.run`), and the `models` section of PROTOCOL.md still says v2 is unchanged for older clients. PR #11 rewords that section and chooses not to gate the list. Until it merges, treat `models` as the one capability that breaks this rule. Don't copy it.

## Related

- `PROTOCOL.md`, sections on `agents`, `md` and `usage` (the opt-in pattern), and the `models` section (the exception)
- PR #10 code review, finding #3 (kept in the project's shared files, not in this repo)
