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
# Usage: tools/send-to-mac.sh [file.bin]      (default build-68k/Claude30.bin)
# Needs: socat, and sz from lrzsz (brew install lrzsz).
set -euo pipefail

FILE="${1:-build-68k/Claude30.bin}"
WIFI232_IP="${WIFI232_IP:-192.168.0.9}"
PORT="${PORT:-2325}"

[ -f "$FILE" ] || { echo "No such file: $FILE (build the client first)" >&2; exit 1; }
for tool in socat sz; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

echo "Waiting for the SE/30 on port $PORT (only $WIFI232_IP may connect)."
echo "In ZTerm: ATDT<this Mac's IP>:$PORT"
# One connection only (no fork). sz -b sends binary; the .bin is MacBinary,
# which ZTerm decodes back into the application with its resource fork.
socat TCP-LISTEN:"$PORT",reuseaddr,range="$WIFI232_IP"/32 EXEC:"sz -b -e $(printf %q "$FILE")"
echo "Sent $FILE."
