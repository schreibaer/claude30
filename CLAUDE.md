# Claude30 repository notes

Use Claude Code from a stock Macintosh SE/30. `bridge.py` (Python, host) drives Claude Code through the Agent SDK; `client/` is Claude30, the native System 7 app in C built with Retro68. See `README.md`, `PROTOCOL.md` and `client/README.md`.

## Building the 68k client

Don't build Retro68 by hand. Run this first in every fresh machine:

```sh
tools/setup-retro68.sh
```

It unpacks a prebuilt toolchain from `/mnt/project-files/toolchain/` into `/opt/Retro68-build` in seconds. Only when no matching archive is cached does it build from source (about 30 minutes) and save a new archive there for later threads. The pinned Retro68 commit is `RETRO68_COMMIT` in the script; change it there and in `client/README.md` together.

Then, from the repository root:

```sh
cmake -S client -B build-68k \
  -DCMAKE_TOOLCHAIN_FILE=/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
cmake --build build-68k

# host tests for the portable core
cmake -S client/tests -B build-host && cmake --build build-host && ctest --test-dir build-host
```

## Rules

- Keep bridge changes compatible with `PROTOCOL.md`, or bump the protocol version and update the spec.
- Classic Mac code targets System 7 on a 68030 with little RAM: name the Toolbox calls you rely on and flag anything that needs a newer System or more memory.
- Say what you tested and what you didn't.
- Before changing the protocol, menus or other areas with past mistakes, check `docs/solutions/` for recorded lessons.
