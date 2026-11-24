#!/bin/bash
# Listen for the SE/30's WiFi232 and hand each call to the bridge.
# Usage: ./run.sh [bridge options]   e.g.  ./run.sh --cwd ~/code/myproject
#                                         ./run.sh --projects ~/code --files ro
# Keep this folder at a path without spaces (socat splits on them).
DIR="$(cd "$(dirname "$0")" && pwd)"
WIFI232_IP="${WIFI232_IP:-192.168.0.9}"
PORT="${PORT:-2324}"

# Write the exact command (with your options) to a launcher socat can exec.
LAUNCH="$DIR/.launch.sh"
{
  echo '#!/bin/bash'
  printf 'exec %q -u %q' "$DIR/.venv/bin/python" "$DIR/bridge.py"
  printf ' %q' "$@"
  echo
} > "$LAUNCH"
chmod +x "$LAUNCH"

echo "se30bridge listening on port $PORT for $WIFI232_IP  (ctrl-C to stop)"
exec socat TCP-LISTEN:"$PORT",reuseaddr,fork,range="$WIFI232_IP"/32 EXEC:"$LAUNCH"
