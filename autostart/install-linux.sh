#!/usr/bin/env bash
# Install a per-user XFCE autostart entry and a menu launcher for WFAS.
#
#   ./install-linux.sh
#   WFAS_APPIMAGE="$HOME/Apps/WiFiAudioStreaming.AppImage" ./install-linux.sh
#   ./install-linux.sh --now
#
# Both the autostart entry and the application-menu entry point at the same
# wrapper.  The wrapper takes a lock, refuses to start when UDP 9090 is already
# in use, and keeps the AppImage runtime + extracted server in one process
# group so they can be stopped together.  No sudo is used.
set -euo pipefail

APP="${WFAS_APPIMAGE:-$HOME/Applications/WiFiAudioStreaming.AppImage}"
AUTOSTART_DIR="$HOME/.config/autostart"
MENU_DIR="$HOME/.local/share/applications"
WRAPPER_DIR="$HOME/.local/bin"
WRAPPER="$WRAPPER_DIR/wfas-server-autostart.sh"
AUTOSTART_DESKTOP="$AUTOSTART_DIR/wfas-server.desktop"
MENU_DESKTOP="$MENU_DIR/wfas-server.desktop"
STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/wfas-lite"

START_NOW=0
for argument in "$@"; do
    case "$argument" in
        --now) START_NOW=1 ;;
        --appimage=*) APP="${argument#--appimage=}" ;;
        -h|--help)
            sed -n '2,11p' "$0"
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

mkdir -p "$AUTOSTART_DIR" "$MENU_DIR" "$WRAPPER_DIR" "$STATE_DIR"

{
    printf '%s\n' '#!/usr/bin/env bash' 'set -u'
    printf 'APP=%q\n' "$APP"
    printf 'LOG_DIR=%q\n' "$STATE_DIR"
    cat <<'WRAPPER_EOF'
mkdir -p "$LOG_DIR" 2>/dev/null || true
LOG="$LOG_DIR/server.log"
LOCK_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/wfas-lite"
mkdir -p "$LOCK_DIR" 2>/dev/null || true

port_9090_in_use() {
    if command -v ss >/dev/null 2>&1; then
        ss -H -lun 2>/dev/null | awk '{print $4}' | grep -qE '(:|\.)9090$'
    else
        return 1
    fi
}

# Single instance across the wrapper, the autostart entry and the menu entry.
# flock is part of util-linux (present on Mint); the PID fallback keeps the
# script dependency-free elsewhere.
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_DIR/server.lock"
    if ! flock -n 9; then
        echo "$(date -Is) another wrapper instance is running; exiting" >>"$LOG"
        exit 0
    fi
else
    PIDFILE="$LOCK_DIR/server.pid"
    if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        echo "$(date -Is) another wrapper instance is running; exiting" >>"$LOG"
        exit 0
    fi
    echo $$ >"$PIDFILE"
    trap 'rm -f "$PIDFILE"' EXIT
fi

# A server started manually (or by an older desktop file) is not protected by
# our lock.  Do not create a second one; its stale unicast session is exactly
# what makes clients receive WFAS_BUSY.
if port_9090_in_use; then
    echo "$(date -Is) UDP 9090 is already in use; not starting a duplicate server" >>"$LOG"
    exit 0
fi

# setsid puts the AppImage runtime and the extracted binary it spawns into one
# process group.  Killing the group on logout/shutdown stops both, instead of
# leaving the real server (whose process name is "WiFi Audio Streaming")
# running after the launcher exits.
if command -v setsid >/dev/null 2>&1; then
    setsid "$APP" --server --persist >>"$LOG" 2>&1 &
else
    "$APP" --server --persist >>"$LOG" 2>&1 &
fi
child=$!

terminate() {
    kill -TERM -"$child" 2>/dev/null || kill -TERM "$child" 2>/dev/null
}
trap terminate TERM INT HUP

status=0
wait "$child" || status=$?

# If the runtime died but the extracted server still owns the port, stop that
# leftover as well.  Only the process listening on 9090 is touched.
if command -v ss >/dev/null 2>&1; then
    leftovers=$(ss -H -lunp 2>/dev/null | grep ':9090 ' | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u)
    for pid in $leftovers; do
        [ -n "$pid" ] || continue
        kill -TERM "$pid" 2>/dev/null || true
    done
fi
exit "$status"
WRAPPER_EOF
} >"$WRAPPER"
chmod +x "$WRAPPER"

# Autostart entry.
cat >"$AUTOSTART_DESKTOP" <<DESKTOP_EOF
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
chmod +x "$AUTOSTART_DESKTOP"

# Application-menu entry: the same wrapper, so a manual menu start cannot
# bypass the single-instance guard.
cat >"$MENU_DESKTOP" <<MENU_EOF
[Desktop Entry]
Type=Application
Version=1.0
Name=WFAS Server
Comment=WiFi Audio Streaming server (persistent)
Exec="$WRAPPER"
Terminal=false
Categories=AudioVideo;Audio;Network;
MENU_EOF
chmod +x "$MENU_DESKTOP"

echo "Installed WFAS server autostart and menu launcher for the current user."
echo "  AppImage : $APP"
echo "  wrapper  : $WRAPPER"
echo "  autostart: $AUTOSTART_DESKTOP"
echo "  menu     : $MENU_DESKTOP"
echo "  log      : $STATE_DIR/server.log"
echo
echo "It starts at the next XFCE login. Use --now to start it immediately."

if [ "$START_NOW" -eq 1 ]; then
    nohup "$WRAPPER" >/dev/null 2>&1 &
    echo "Started."
fi