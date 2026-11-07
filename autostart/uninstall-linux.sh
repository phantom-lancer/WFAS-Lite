#!/usr/bin/env bash
# Remove the per-user XFCE autostart entry created by install-linux.sh.
#
#   ./uninstall-linux.sh
#   ./uninstall-linux.sh --stop    # also stop a running server instance
set -euo pipefail

AUTOSTART_DIR="$HOME/.config/autostart"
DESKTOP="$AUTOSTART_DIR/wfas-server.desktop"
WRAPPER="$HOME/.local/bin/wfas-server-autostart.sh"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/wfas-lite"

STOP=0
for argument in "$@"; do
    case "$argument" in
        --stop) STOP=1 ;;
        -h|--help) sed -n '2,7p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $argument" >&2; exit 2 ;;
    esac
done

if [ -f "$DESKTOP" ]; then
    rm -f "$DESKTOP"
    echo "Removed $DESKTOP"
else
    echo "No autostart entry at $DESKTOP"
fi

if [ -f "$WRAPPER" ]; then
    rm -f "$WRAPPER"
    echo "Removed $WRAPPER"
fi

if [ "$STOP" -eq 1 ]; then
    if command -v pkill >/dev/null 2>&1; then
        pkill -f 'WiFiAudioStreaming.AppImage --server' 2>/dev/null || true
        echo "Stopped a running WFAS server (if any)."
    else
        echo "pkill not available; stop the server manually." >&2
    fi
fi

echo "Autostart removed. Logs remain in $STATE_DIR."
