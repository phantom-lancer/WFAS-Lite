# Linux sender patch — capture backlog drain

`0001-linux-sender-capture-backlog-drain.patch`

Optional sender-side hardening for the official **WFAS 1.2.0** Linux source
(`research-upstream-v1.2`, base commit `abdc0ab`). The Windows receiver already
absorbs a surplus without clicks; this patch stops the surplus from being
produced in the first place.

It changes one branch in `src/main/native/audio_engine.c`:

| | Before | After |
|---|---|---|
| Trigger | capture backlog > **200 ms** | backlog > **50 ms** |
| Action | recreate the whole PulseAudio stream | drain the backlog in place with `pulse_drain_stream()` |
| Consequence | up to 200 ms is sent to the network as a burst; stream recreation can glitch | at most ~one 10 ms read slips through; stream stays alive |

`pulse_flush_stream()` remains only as a fallback if the stream pointer was
cleared concurrently by `pulse_stop()`. No protocol, packet format, or Kotlin
code is touched.

## 0. Root cause assessment (read before applying)

The Windows client now survives a burst on its own: it uses a 1 MB UDP receive
buffer, treats audio as evidence of liveness as well as PING, sends `CLIENT_BYE`
before closing, reconnects with 1/2/4/8/15 s backoff and re-discovers the
server. This patch is therefore **not required for the client to work**; it
removes the burst at its source.

It addresses:

* a capture backlog that accumulated while the sender was stalled or busy;
* the old behaviour of sending that backlog to the network as one burst (which
  overflows the receiver's socket buffer and loses audio);
* the expensive full PulseAudio stream recreation that used to happen after
  200 ms of backlog.

It does **not** address:

* the stall itself (a sink change, a suspended graph or CPU starvation can still
  pause capture for a moment);
* PING transmission. PING is sent by an independent coroutine
  (`pingJob` in `Main.kt`, `launch { delay(1000); socket.send("PING") }`), not by
  the blocking `pa_simple_read` loop. A capture stall therefore does **not**
  block PING. PING can still be *lost* when the receiver's UDP buffer overflows
  during a burst or on Wi-Fi; that is why the client's watchdog now also accepts
  audio as liveness and why its receive buffer was enlarged.

Do not increase the client's drift range or latency to hide a remaining burst.
If the sender log still shows `stream ricreato` or the receiver still reports
`splice` events after this patch, capture the sender log and the receiver's
`diag:` line before changing anything else.


## 1. Prerequisites

- Linux x86_64 (PulseAudio or PipeWire with the PulseAudio compatibility layer)
- Git, JDK 17, CMake ≥ 3.21, a C compiler (`gcc`/`clang`)
- A source checkout of WFAS 1.2.0 at commit `abdc0ab`

This patch is applied to a **source build**. Do not try to patch the official
AppImage binary, and do not replace the AppImage in place.

## 2. Check and apply

From the directory that contains the WFAS source checkout:

```bash
# Base must be clean and at abdc0ab
git -C research-upstream-v1.2 rev-parse --short HEAD
git -C research-upstream-v1.2 status --porcelain

# Dry run, then apply
git -C research-upstream-v1.2 apply --check \
  WFAS-Lite/patches/0001-linux-sender-capture-backlog-drain.patch
git -C research-upstream-v1.2 apply \
  WFAS-Lite/patches/0001-linux-sender-capture-backlog-drain.patch
```

If the source lives somewhere else, use the absolute path to the patch and run
the command from the repository root:

```bash
cd /path/to/wfas-1.2.0
git apply --check /path/to/WFAS-Lite/patches/0001-linux-sender-capture-backlog-drain.patch
git apply         /path/to/WFAS-Lite/patches/0001-linux-sender-capture-backlog-drain.patch
```

## 3. Build and run the patched sender

The Gradle build compiles the native library and copies it into the app
resources:

```bash
cd research-upstream-v1.2

./gradlew copyNativeLib     # CMAKE_BUILD_TYPE=Release, produces libaudio_engine.so

./gradlew run               # GUI sender
# or headless:
./gradlew run --args="--server --persist"
```

To produce a package instead of running from Gradle:

```bash
./gradlew createDistributable   # portable app under build/compose/binaries/main/app/
./gradlew packageDeb            # .deb installer
```

## 4. Verify

Start the patched sender from a terminal (so `AE_LOG` output is visible) and
play real music for at least 1–2 minutes, including actions that can stall the
capture thread (opening heavy apps, switching sinks, suspending the machine
briefly).

Sender side, expected new line (only when a backlog actually occurs):

```text
[AudioEngine/Linux] Backlog cattura 52 ms — drain in place.
```

The numeric value must be **≥ 50** and normally stays well under 200. The line
`Drift latenza ... stream ricreato` must no longer appear.

Windows receiver side (`wfas-lite.exe`):

```text
stats: ... overrun=0/0f splice=<a few>/<frames>f/max<frames>f ...
       peak=<source peak>/<source peak> limiter=0f ...
```

- `overrun=0/0f` — no hard front-chunk drops;
- `splice` — a few small crossfaded corrections, not a stream of drops;
- `limiter=0f` and `peak` equal to the source — no clipping or gain change;
- `buffer` remains near `target` and does not drift upward over the session.

## 5. Revert

```bash
cd research-upstream-v1.2
git checkout -- src/main/native/audio_engine.c
# or: git apply -R /path/to/WFAS-Lite/patches/0001-linux-sender-capture-backlog-drain.patch
```

Rebuild (`./gradlew copyNativeLib`) after reverting so the resources contain the
unpatched library again.

## 6. Notes

- The patch was generated against `abdc0ab` and verified with
  `git apply --check`; the source tree used to generate it was restored to a
  clean state.
- It was not executed on a Linux host in this environment (no Linux runtime was
  available). Treat section 4 as the acceptance test.
- The receiver fix is independent: if the official unpatched AppImage is used,
  the Windows client still removes the surplus with crossfaded splices and
  reports `overrun=0/0f`.
