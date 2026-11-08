#!/usr/bin/env bash
# Remove the per-user XFCE autostart entry, menu launcher and wrapper created
# by install-linux.sh.
#
#   ./uninstall-linux.sh
#   ./uninstall-linux.sh --stop    # also stop a running server instance
set -euo pipefail

AUTOSTART_DESKTOP="$HOME/.config/autostart/wfas-server.desktop"
MENU_DESKTOP="$HOME/.local/share/applications/wfas-server.desktop"
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

stop_server() {
    # 1. Ask the wrapper (if any) to forward TERM to its process group.
    if command -v pkill >/dev/null 2>&1; then
        pkill -TERM -f 'wfas-server-autostart\.sh' 2>/dev/null || true
    fi
    # 2. Ask the AppImage runtime and the extracted binary to stop.  The actual
    #    port owner is named "WiFi Audio Streaming", not "WiFiAudioStreaming".
    if command -v pkill >/dev/null 2>&1; then
        pkill -TERM -f 'WiFiAudioStreaming\.AppImage[^ ]* .*--server' 2>/dev/null || true
        pkill -TERM -f 'WiFi Audio Streaming .*--server' 2>/dev/null || true
    fi
    sleep 1
    # 3. Last resort: stop whatever still listens on UDP 9090 (this is the
    #    exact check used during the incident).
    if command -v ss >/dev/null 2>&1; then
        pids=$(ss -H -lunp 2>/dev/null | grep ':9090 ' | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u)
        for pid in $pids; do
            [ -n "$pid" ] || continue
            kill -TERM "$pid" 2>/dev/null || true
        done
        sleep 1
        for pid in $pids; do
            [ -n "$pid" ] || continue
            kill -KILL "$pid" 2>/dev/null || true
        done
    fi
}

if [ "$STOP" -eq 1 ]; then
    stop_server
    echo "Stopped a running WFAS server (if any)."
fi

for file in "$AUTOSTART_DESKTOP" "$MENU_DESKTOP" "$WRAPPER"; do
    if [ -f "$file" ]; then
        rm -f "$file"
        echo "Removed $file"
    fi
done

echo "Autostart removed. Logs remain in $STATE_DIR."
echo "If a server was started outside these scripts, stop it with:"
echo "  ss -lunp | grep 9090"