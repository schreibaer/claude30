#!/bin/bash
# Give an emulated Mac a "WiFi232" on its modem port, for testing the client
# without the real hardware:
#
#   Basilisk II modem port <-> pty <-> tcpser (Hayes modem) <-> TCP <-> bridge
#
# tcpser answers AT commands like the WiFi232 does and dials "bridge" to the
# host and port you give. Start the bridge first, in another terminal:
#
#   WIFI232_IP=127.0.0.1 ./run.sh --demo                      # the real bridge
#   socat TCP-LISTEN:2324,reuseaddr,fork EXEC:"python3 tools/emu/stress_bridge.py"
#
# Then run this script and point Basilisk II's modem port at the pty it prints
# (Basilisk II prefs: "seriala /tmp/se30-modem").
#
# Usage: tools/emu/basilisk-modem.sh [host:port]      (default 127.0.0.1:2324)
# Needs: socat, tcpser (https://github.com/FozzTexx/tcpser).
set -euo pipefail

TARGET="${1:-127.0.0.1:2324}"
MAC_SIDE="${MAC_SIDE:-/tmp/se30-modem}"
MODEM_SIDE="${MODEM_SIDE:-/tmp/se30-tcpser}"
SPEED="${SPEED:-57600}"

for tool in socat tcpser; do
    command -v "$tool" >/dev/null || { echo "$tool is not installed" >&2; exit 1; }
done

socat pty,raw,echo=0,link="$MAC_SIDE" pty,raw,echo=0,link="$MODEM_SIDE" &
SOCAT=$!
trap 'kill $SOCAT 2>/dev/null' EXIT
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -e "$MODEM_SIDE" ] && break
    sleep 0.2
done

echo "Modem pty for Basilisk II: $MAC_SIDE  (prefs: seriala $MAC_SIDE)"
echo "ATDTbridge dials $TARGET. ctrl-C to stop."
# -n maps the phone book entry "bridge" to the bridge's address, like the
# WiFi232's AT&Z alias; -s sets the DTE speed.
tcpser -d "$MODEM_SIDE" -s "$SPEED" -n "bridge=$TARGET" -l 4
