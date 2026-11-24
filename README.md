# se30bridge

Use Claude Code from a Macintosh SE/30 through a WiFi232 modem.

```
SE/30 (ZTerm) -> WiFi232 -> Wi-Fi -> socat on your Mac -> bridge.py -> Claude
```

## Setup on the Mac

```sh
brew install socat
mkdir -p ~/se30bridge && cd ~/se30bridge     # put bridge.py + run.sh here
python3 -m venv .venv
.venv/bin/pip install claude-agent-sdk
chmod +x run.sh
```

The bridge uses the same login as your `claude` command. Run `claude` once in a normal terminal first if you haven't signed in.

## Run it

```sh
./run.sh --demo                       # fake Claude: test the link, spends nothing
./run.sh --cwd ~/code/myproject       # the real thing
```

`run.sh` listens on port 2324 and only accepts connections from the WiFi232 (192.168.0.9). Every call starts a fresh bridge with a fresh conversation. Log messages appear in the Terminal window where you started it.

## On the SE/30 (ZTerm, 57600 8N1)

```
ATNET0
AT&Z1=<mac-ip>:2324,bridge
AT&W
ATDTbridge
```

Press ctrl-C to stop Claude, use `/help` for commands, and type `+++`, wait a second, then `ATH` to hang up.

## Options

| Flag | Default | |
|---|---|---|
| `--cwd DIR` | current dir | project Claude works in |
| `--demo` | off | no Claude, canned output |
| `--baud N` | 57600 | pace output to the serial speed (0 = off) |
| `--cols / --rows` | 80 / 24 | wrap width and pager height |
| `--charset ascii\|macroman` | ascii | try `macroman` for real curly quotes in ZTerm |
| `--accept-edits` | off | don't ask before file edits |
| `--model NAME` | your default | |

Read, Glob, Grep and TodoWrite run without asking. Everything else asks for permission first.

See PROTOCOL.md for the app-mode protocol a native SE/30 client would use.
