#!/usr/bin/env bash
# Install a per-user XFCE autostart entry for the official WFAS AppImage.
#
#   ./install-linux.sh
#   WFAS_APPIMAGE="$HOME/Apps/WiFiAudioStreaming.AppImage" ./install-linux.sh
#
# It runs the server as:
#   ~/Applications/WiFiAudioStreaming.AppImage --server --persist
# No sudo is used and nothing outside $HOME is touched.  A wrapper with an
# flock/PID guard prevents a second server instance.
set -euo pipefail

APP="${WFAS_APPIMAGE:-$HOME/Applications/WiFiAudioStreaming.AppImage}"
AUTOSTART_DIR="$HOME/.config/autostart"
WRAPPER_DIR="$HOME/.local/bin"
WRAPPER="$WRAPPER_DIR/wfas-server-autostart.sh"
DESKTOP="$AUTOSTART_DIR/wfas-server.desktop"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/wfas-lite"

START_NOW=0
for argument in "$@"; do
    case "$argument" in
        --now) START_NOW=1 ;;
        --appimage=*) APP="${argument#--appimage=}" ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown argument: $argument" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$APP" ]; then
    echo "AppImage not found: $APP" >&2
    echo "Pass --appimage=/path/to/WiFiAudioStreaming.AppImage or set WFAS_APPIMAGE." >&2
    exit 1
fi
chmod +x "$APP" 2>/dev/null || true

mkdir -p "$AUTOSTART_DIR" "$WRAPPER_DIR" "$STATE_DIR"

{
    printf '%s\n' '#!/usr/bin/env bash' 'set -u'
    printf 'APP=%q\n' "$APP"
    printf 'LOG_DIR=%q\n' "$STATE_DIR"
    cat <<'WRAPPER_EOF'
mkdir -p "$LOG_DIR" 2>/dev/null || true
LOG="$LOG_DIR/server.log"
LOCK_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/wfas-lite"
mkdir -p "$LOCK_DIR" 2>/dev/null || true

# Single instance.  flock is part of util-linux (present on Mint); the PID
# fallback keeps the script dependency-free elsewhere.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_DIR/server.lock"
    if ! flock -n 9; then
        echo "$(date -Is) another WFAS server is already running; exiting" >>"$LOG"
        exit 0
    fi
else
    PIDFILE="$LOCK_DIR/server.pid"
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        echo "$(date -Is) another WFAS server is already running; exiting" >>"$LOG"
        exit 0
    fi
    echo $$ >"$PIDFILE"
    trap 'rm -f "$PIDFILE"' EXIT
fi

exec "$APP" --server --persist >>"$LOG" 2>&1
WRAPPER_EOF
} >"$WRAPPER"
chmod +x "$WRAPPER"

cat >"$DESKTOP" <<DESKTOP_EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=WFAS Server
Comment=WiFi Audio Streaming server (persistent)
Exec="$WRAPPER"
Terminal=false
X-GNOME-Autostart-enabled=true
X-GNOME-Autostart-Delay=3
OnlyShowIn=XFCE;
DESKTOP_EOF
chmod +x "$DESKTOP"

echo "Installed WFAS server autostart for the current user."
echo "  AppImage : $APP"
echo "  wrapper  : $WRAPPER"
echo "  desktop  : $DESKTOP"
echo "  log      : $STATE_DIR/server.log"
echo
echo "It starts at the next XFCE login. Use --now to start it immediately."

if [ "$START_NOW" -eq 1 ]; then
    nohup "$WRAPPER" >/dev/null 2>&1 &
    echo "Started."
fi
