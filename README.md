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
./run.sh --projects ~/code            # Claude30 tabs: any folder in ~/code is a project
```

`run.sh` listens on port 2324 and only accepts connections from the WiFi232 (192.168.0.9). Every call starts its own bridge. Log messages appear in the Terminal window where you started it.

### When Wi-Fi drops

The WiFi232 makes one call at a time, so a new call means the last one is over. The new bridge ends the old one (you see `ending the previous call` in the log), so nothing is left running on the Mac and you never need to kill a bridge by hand. Claude30 also sends a heartbeat; a bridge that hears nothing from it for `--idle-timeout` seconds (default 180) ends the call by itself. `run.sh` turns on TCP keepalive too, which helps ZTerm calls.

Claude30 redials by itself and its tabs pick up their conversations where they were (tabs only; ZTerm and the v1 app start fresh each call). The bridge keeps what it needs in `~/.se30bridge` (`--state-dir`): `call.lock`, and `sessions.json`, which maps each tab's resume token to its Claude conversation. A token only resumes conversations the bridge started, in the project it started them in, and is forgotten after 30 days unused or when the tab is closed. Two SE/30 setups on one Mac need different `--state-dir`s, or each call would end the other's.

### Projects for Claude30's tabs

The Claude30 app can keep up to six conversations open at once, one per tab, each in its own project. It asks the bridge for a list of projects and can open only those. You choose what is on the list:

- `--projects DIR` offers every folder inside `DIR` (hidden folders excepted). Repeat it for more than one root.
- `--cwd DIR` adds `DIR` itself to the list.
- With neither option, the list is the folder you started `run.sh` in.

The SE/30 never sends a path, only a name from the list, so it can't reach anything else on your Mac. Anyone on your LAN who can dial the WiFi232 gets the same list, so offer only folders you are happy to share that way.

Tabs need the Claude30 app from the same version of this repository. **Update `bridge.py` and the app together**: a new app can't talk to an old bridge (it reports that the bridge did not answer), while an older app and ZTerm keep working with the new bridge. The new app works against a bridge from before resume too, but then a drop starts fresh conversations, as it used to.

### Project files on the SE/30

From a tab, Claude30 can browse the tab's project, read any text file in it and save small edits. `--files` controls this:

- `rw` (the default): browse, read and save.
- `ro`: browse and read only.
- `off`: no file access from the SE/30 at all.

The SE/30 can only see files inside a project folder. It never sees `.git`, `.ssh`, `.env*`, `*.pem`, `*.key`, `id_rsa*` or `id_ed25519*`, or anything your git ignore rules exclude. It can't create, rename or delete files. A save replaces the file in one step and is refused if the file changed since the SE/30 opened it, unless the person on the SE/30 chooses Replace. Claude is told which files the person saved at the start of its next turn.

The link is unencrypted, and `--files` applies to anyone who can dial in. Use `ro` or `off` if that matters on your network.

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
| `--cwd DIR` | current dir | project for ZTerm and older apps; also offered to tabs |
| `--projects DIR` | none | offer every folder in `DIR` to Claude30's tabs (repeatable) |
| `--max-sessions N` | 6 | tabs open at once |
| `--files rw\|ro\|off` | rw | what Claude30 may do with project files |
| `--demo` | off | no Claude, canned output |
| `--baud N` | 57600 | pace output to the serial speed (0 = off) |
| `--cols / --rows` | 80 / 24 | wrap width and pager height |
| `--charset ascii\|macroman` | ascii | try `macroman` for real curly quotes in ZTerm |
| `--accept-edits` | off | don't ask before file edits |
| `--model NAME` | your default | |
| `--state-dir DIR` | `~/.se30bridge` | the call lock and resume records; one call at a time per folder |
| `--idle-timeout N` | 180 | end a call when an app that sends heartbeats is silent for N seconds (0 = never) |

Read, Glob, Grep and TodoWrite run without asking. Everything else asks for permission first.

See PROTOCOL.md for the app-mode protocol a native SE/30 client would use.
