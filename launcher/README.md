# OXRSys Launcher

A native macOS (arm64, SwiftUI) launcher for playing Windows VR games through
CrossOver + OXRSys. You connect the headset, click **Play**, and it handles the rest:

- **Library**: finds the VR games in the `VR` bottle's Steam library (from the
  `appmanifest_*.acf` files plus `libraryfolders.vdf`). A game counts as VR if its
  folder has `openvr_api.dll`, `openxr_loader.dll` or Unity OpenXR plugins. Header art
  comes from Steam's `appcache/librarycache`. You can add games by app id or `.exe`
  (stored in `~/Library/Application Support/OXRSys/Launcher/games.json`), and hide or
  unhide them from the right-click menu.
- **Status**:
  - headset (USB via `adb`, or Wi-Fi via the runtime's `runtime_status.json`);
  - whether the runtime is installed;
  - CrossOver/Steam, and whether `XR_RUNTIME_JSON` is set (from the bottle's
    `cxbottle.conf` `[EnvironmentVariables]` or CrossOver's environment);
  - charger watts and battery level;
  - scrcpy or the OXRSys Simulator running;
  - live stream stats while a stream is running.
- **Play** runs these steps:
  1. Sets `transport` in `oxrsys-runtime.toml` to `usb_adb` when `adb` sees the
     headset, otherwise `wifi`. It edits only that line, writes a timestamped
     `*.launcher-*.bak` first (keeping 5), and leaves the file alone if the value is
     already right. The transport is fixed when a session starts, so restart the game
     after switching between USB and Wi-Fi.
  2. For OpenVR titles, if OpenComposite isn't installed, asks first and then runs
     `scripts/install-opencomposite.sh --bottle VR <game dir>`. The stock DLL is kept as
     `openvr_api.dll.stock`.
  3. If nothing is running, opens CrossOver with `XR_RUNTIME_JSON` exported. If Steam
     is running without it, it stops and tells you to quit Steam yourself. It never
     kills or quits Wine or Steam.
  4. Runs `wine --bottle VR 'C:\Program Files (x86)\Steam\steam.exe' steam://rungameid/<appid>`.
  5. Starts headset audio capture (below), and stops it when the game's Wine process
     exits or when you click **Stop audio**.

It never changes a bottle's graphics settings or registry.

## Headset audio without BlackHole

The launcher captures audio with a Core Audio **process tap** (macOS 14.2+) and passes
it to the OXRSys runtime through a shared-memory ring at
`~/Library/Application Support/OXRSys/headset-audio.ring` (format:
`Sources/TapCore/include/OXAudioRing.h`, 48 kHz stereo float32 normally). You don't
need a virtual device or a Multi-Output device.

Set these in **Settings** (⌘,):
- **Source**:
  - **Game only** (default): taps the game's own Wine process. Wine runs each `.exe`
    as its own process, so this captures just the game.
  - **All Mac audio**
  - **Off**
- **Mute Mac while streaming**: the tapped audio is muted on the Mac's speakers while
  it is captured (`CATapMutedWhenTapped`).

On the runtime side, the `headset_audio_source` key (`auto` | `tap` | `loopback`,
default `auto`) chooses between the ring and the old loopback-device path. In `auto`,
the ring is used whenever the launcher is feeding it, and a loopback device
(BlackHole) remains the fallback. `headset_audio = true` must still be set.

### Permissions

The first time audio capture starts, macOS asks to let **OXRSys Launcher** record
system audio (System Settings › Privacy & Security › Screen & System Audio Recording ›
System Audio Recording Only). Until you click **Allow**, capture waits (the UI stays
responsive). The launcher must be started from Finder, the Dock or `open`, not from
CrossOver, so the permission is attributed to the launcher itself.

The build script signs with the first "Apple Development" identity in your keychain,
so the grant survives rebuilds. With no such identity (or `SIGN_IDENTITY=-`) it signs
ad-hoc, and macOS asks again after every rebuild.

Headset audio needs an OXRSys runtime with the tap-ring reader (oxrsys-src branch
`feat/tap-audio`). Over Wi-Fi it also needs a Quest client with UDP audio support
(same branch); before that, audio was USB-only.

## Build and run

```sh
launcher/scripts/build-launcher.sh          # -> launcher/build/OXRSys Launcher.app
open "launcher/build/OXRSys Launcher.app"
```

The script uses `env -u TOOLCHAINS xcrun swift build -c release --arch arm64` and then
assembles and signs the bundle (see Permissions). It does not install into `/Applications`; copy
the app there yourself if you want it in the Dock.

Debug flags (these print JSON or text and exit):

```sh
B="launcher/build/OXRSys Launcher.app/Contents/MacOS/OXRSysLauncher"
"$B" --dump-library                 # VR games found
"$B" --dump-status                  # one status snapshot
"$B" --dump-game-pids 617830        # Wine PIDs of a running game (+ which have audio)
"$B" --set-transport wifi /tmp/copy.toml   # exercise the toml edit on a copy
open -n -W --stdout /tmp/r.log "launcher/build/OXRSys Launcher.app" --args --test-ring global 3
```

## TapProbe

`scripts/build-tapprobe.sh` builds `build/TapProbe.app`, the phase-1 feasibility probe.
Run it with `open -n -W --stdout f.log build/TapProbe.app --args list|wine|global|pid <pid> [--mute] [--seconds N]`.
It logs per-second RMS levels and writes a WAV file to `/tmp/tapprobe/`.
