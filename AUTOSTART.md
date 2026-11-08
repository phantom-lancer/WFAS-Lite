# WFAS-Lite autostart

Both scripts are per-user. They do not use `sudo`, do not write outside the
user profile, and do not need any package installation.

## Windows 11 (client)

Files: `autostart\install-windows.ps1`, `autostart\uninstall-windows.ps1`.

### Install

```powershell
cd "C:\Users\ssash\Documents\Codex\2026-11-05\wfas-lite-windows-linux-wi-fi\WFAS-Lite"
powershell -ExecutionPolicy Bypass -File .\autostart\install-windows.ps1
```

With a pinned server address instead of discovery:

```powershell
powershell -ExecutionPolicy Bypass -File .\autostart\install-windows.ps1 -Server 192.168.1.125
```

Optional switches:

| Switch | Effect |
|---|---|
| `-Exe <path>` | use a specific `wfas-lite.exe` |
| `-LatencyMs 400` | jitter-buffer target (default 400) |
| `-Server <ip>` | try discovery, then this address |
| `-NoDiscovery` | manual address only (requires `-Server`) |
| `-NoCopy` | do not copy the exe into `%LOCALAPPDATA%` |
| `-StartNow` | launch the client immediately as well |

What it does:

1. Copies `wfas-lite.exe` to `%LOCALAPPDATA%\WFAS-Lite\wfas-lite.exe`, so the
   login entry survives a rebuild or a deleted build directory.
2. Writes `wfas-lite-run.cmd`, which appends stdout/stderr to
   `%LOCALAPPDATA%\WFAS-Lite\wfas-lite.log` and rotates it at 5 MB.
3. Writes `wfas-lite-run.vbs`, which starts the `.cmd` with window style `0`
   (hidden, no console window).
4. Registers
   `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\WFAS-Lite` =
   `wscript.exe "<...>\wfas-lite-run.vbs"`.

Because the entry is under `HKCU\...\Run`, it starts inside the interactive user
session after sign-in, which is required for WASAPI to reach the default audio
endpoint. No service or scheduled task is involved.

### Verify

```powershell
Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'WFAS-Lite'
Get-Content "$env:LOCALAPPDATA\WFAS-Lite\wfas-lite.log" -Tail 20
Get-Process wfas-lite
```

Expected log after a successful start:

```text
WASAPI endpoint buffer: 1056 frames (22 ms), 48 kHz stereo S16LE.
Discovered WFAS server 'studio-pc' at 192.168.1.125:9090.
Connecting to 192.168.1.125:9090 (WFAS v2)...
```

### Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File .\autostart\uninstall-windows.ps1
# also delete the log:
powershell -ExecutionPolicy Bypass -File .\autostart\uninstall-windows.ps1 -RemoveLogs
```

This removes the Run entry and the `.cmd`/`.vbs` launchers. A currently running
client is not stopped; end it from Task Manager or `Stop-Process -Name wfas-lite`.

## Linux Mint XFCE (server)

Files: `autostart/install-linux.sh`, `autostart/uninstall-linux.sh`.

### Install

The official AppImage must be at `~/Applications/WiFiAudioStreaming.AppImage`
(or pass a different path):

```bash
cd /path/to/WFAS-Lite/autostart
chmod +x install-linux.sh uninstall-linux.sh
./install-linux.sh
# or:
WFAS_APPIMAGE="$HOME/Apps/WiFiAudioStreaming.AppImage" ./install-linux.sh
./install-linux.sh --appimage="$HOME/Applications/WiFiAudioStreaming.AppImage" --now
```

What it does:

1. Makes the AppImage executable (if needed).
2. Writes `~/.local/bin/wfas-server-autostart.sh`, which:
   * takes an exclusive `flock` on `~/.cache/wfas-lite/server.lock`
     (PID-file fallback if `flock` is unavailable), so a second wrapper instance
     exits immediately;
   * refuses to start if UDP 9090 is already in use (for example a server
     started manually or by an older menu entry), which prevents the duplicate
     server that caused stuck `WFAS_BUSY` sessions;
   * runs the AppImage with `--server --persist` in its own process group
     (`setsid`) and forwards TERM/INT/HUP to that group, so the AppImage
     runtime and the extracted `WiFi Audio Streaming` server both stop on
     logout/shutdown;
   * after the runtime exits, stops a leftover process still listening on 9090;
   * appends output to `~/.local/state/wfas-lite/server.log`.
3. Writes `~/.config/autostart/wfas-server.desktop` (`OnlyShowIn=XFCE;`,
   `Terminal=false`, 3 s delay) **and**
   `~/.local/share/applications/wfas-server.desktop`. Both point at the same
   wrapper, so a menu launch cannot bypass the single-instance guard.

No `sudo` is used and nothing outside `$HOME` is modified.

If an older `~/.local/share/applications/wfas-server.desktop` exists (for
example one that launches the AppImage directly), rerun `install-linux.sh` once
to replace it with the wrapper-based entry.

### Verify

```bash
cat ~/.config/autostart/wfas-server.desktop
cat ~/.local/share/applications/wfas-server.desktop
pgrep -af 'WiFiAudioStreaming.AppImage|WiFi Audio Streaming'
ss -lunp | grep 9090
tail -n 20 ~/.local/state/wfas-lite/server.log
```

Only one UDP 9090 owner must exist. Starting the desktop entry twice is safe:
the second wrapper exits because the lock is held or the port is busy.

### Uninstall

```bash
./uninstall-linux.sh
# also stop a running server:
./uninstall-linux.sh --stop
```

`--stop` asks the wrapper and the AppImage to stop, then stops whatever still
listens on UDP 9090 (the real server process is named `WiFi Audio Streaming`,
not `WiFiAudioStreaming`). The log is kept. The AppImage itself is never
deleted.

## Notes

* Windows autostart is already installed on this machine by
  `install-windows.ps1` (Run entry `WFAS-Lite`). Remove it with
  `uninstall-windows.ps1` if it is not wanted.
* The Linux autostart was syntax-checked (`bash -n`) but cannot be installed
  from this Windows environment; run `install-linux.sh` on the Mint machine.
* Both entries rely on the normal application paths, not on build directories
  (Windows: `%LOCALAPPDATA%\WFAS-Lite`; Linux: `~/Applications`).
