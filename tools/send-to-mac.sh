#!/bin/bash
# Send the Claude30 app to the SE/30 over the WiFi232, with ZMODEM.
#
# On the Mac (host): run this script. It listens once on port 2325, accepting
# only the WiFi232, and sends the MacBinary file with sz.
# On the SE/30: in ZTerm (57600 8N1), turn on Settings > Binary Files >
# MacBinary, then type
#     ATDT<host-ip>:2325
# ZTerm starts receiving by itself (ZMODEM auto-receive). Hang up afterwards
# with +++ and ATH. The received file is the application, icon included.
#
# The SE/30's serial chip holds only 3 bytes, so plain streaming ZMODEM can
# lose bytes at 57,600 baud. By default sz therefore sends 1 KB packets, stops
# after every 2 KB until ZTerm acknowledges them, and waits 30 seconds before
# it gives up. Set SZ_OPTS to override all three, for example:
#     SZ_OPTS="-w 1024 -l 512 -t 600" tools/send-to-mac.sh
#
# Usage: tools/send-to-mac.sh [file.bin]      (default build-68k/Claude30.bin)
# Needs: socat, and sz from lrzsz (brew install lrzsz socat).
set -euo pipefail

FILE="${1:-build-68k/Claude30.bin}"
WIFI232_IP="${WIFI232_IP:-192.168.0.9}"
PORT="${PORT:-2325}"
# -w window bytes, -l packet bytes, -t timeout in tenths of a second.
SZ_OPTS="${SZ_OPTS:--w 2048 -l 1024 -t 300}"

[ -f "$FILE" ] || { echo "No such file: $FILE (build the client first)" >&2; exit 1; }
for tool in socat sz; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed (brew install lrzsz socat)" >&2; exit 1; }
done

STATUS="$(mktemp "${TMPDIR:-/tmp}/send-to-mac.XXXXXX")"
trap 'rm -f "$STATUS"' EXIT
echo none >"$STATUS"

echo "Waiting for the SE/30 on port $PORT (only $WIFI232_IP may connect)."
echo "In ZTerm: ATDT<this Mac's IP>:$PORT"
echo "sz options: -b -e $SZ_OPTS"
# One connection only (no fork). sz -b sends binary, -e escapes control
# characters; the .bin is MacBinary, which ZTerm decodes back into the
# application with its resource fork. sz's exit status goes to $STATUS,
# because socat does not pass it on.
if ! socat TCP4-LISTEN:"$PORT",reuseaddr,range="$WIFI232_IP"/32 \
    SYSTEM:"sz -b -e $SZ_OPTS $(printf %q "$FILE"); echo \$? >$(printf %q "$STATUS")" \
    && [ "$(cat "$STATUS")" = none ]; then
    echo "socat could not listen on port $PORT (see its message above)." >&2
    exit 1
fi

if [ "$(cat "$STATUS")" = 0 ]; then
    echo "Sent $FILE."
    exit 0
fi

cat >&2 <<EOF

The transfer did not finish (sz exit status $(cat "$STATUS")).
In ZTerm, hang up with +++ then ATH, delete the partial file, and try again:
  - Turn AppleTalk off (Chooser) and virtual memory off (Memory control
    panel), then restart. Both delay serial interrupts.
  - Make the bursts smaller:
        SZ_OPTS="-w 1024 -l 512 -t 600" $0 $*
  - Or drop to 19,200 baud: in ZTerm type AT\$SB=19200, set ZTerm to 19200,
    check that AT answers OK, then run this script again. Afterwards set it
    back with AT\$SB=57600, or keep 19,200 and use run.sh --baud 19200 and
    19,200 in Claude30's Settings.
EOF
exit 1
