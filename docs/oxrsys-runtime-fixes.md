# OXRSys runtime fixes (source build)

OXRSys is buildable from source (demonixis/OXRSys; dingyifei/oxrsys is a current
fork). This session's runtime-side fixes live on the fork
**`anoshmisiosdev/oxrsys`**, branch **`fix/pacing-tracking-spaces`**, built as a
thin **x86_64** `liboxrsys-runtime.dylib` off the **1.2.0** tag (protocol-matched
to the connected Quest client — the wire protocol diverged ~276 lines from 1.1.0).

## Hard architecture constraint
The in-process runtime dylib is `dlopen`ed by the CrossOver Wine host, which is
**x86_64 under Rosetta** (CrossOver ships only an `x86_64-unix` Wine tree; an
arm64 dylib cannot load into the x86_64 process). So the runtime dylib **must be
x86_64**. Native arm64 is only possible for **out-of-process** helpers.

## Fixes implemented (on fix/pacing-tracking-spaces)
1. **Reference-space origins** (`Space.cpp` `GetWorldPose`, `InputManager`): demonixis
   collapsed LOCAL/LOCAL_FLOOR/STAGE to identity → viewpoint too high, LOCAL at floor.
   Fixed: STAGE=floor, LOCAL=HMD-pose-at-session-start (yaw-only), LOCAL_FLOOR=LOCAL
   x/z at y=0, plus `RecenterLocalReference()`. **Pending in-headset verify.**
2. **Look-down→forward coupling** (`TrackingReceiver.cpp`): head position predicted by
   linear-velocity extrapolation (NOT angular bleed) overshoots the eye's neck arc during
   a pitch. Fixed: scale position-prediction horizon down as angular speed rises
   (full <0.5 rad/s → 0.15 floor by 4 rad/s). **Pending in-headset verify + tuning.**
3. **Frame pacing / jitter** (`Session::WaitFrame`): old sleep-then-reanchor drifted
   (free-ran ~87.5 fps on a 90 Hz panel). Fixed: self-correcting absolute-deadline grid
   at the client-negotiated period. **VERIFIED**: locks to 13.89 ms @ 72 Hz, ~1.2 ms stddev.
   (True client-vsync phase-lock not possible — protocol carries no present timestamp.)

## Encoder reality (measured 2026-09-08, corrects earlier wrong theories)
Two earlier claims in this doc were **disproven by direct measurement** — recorded
here so nobody chases them again:
- **The "~650 ms software HEVC" regression is a myth.** A standalone x86_64/Rosetta
  VideoToolbox benchmark on this M4 Pro measures **software HEVC at ~27 ms/frame**
  (2272×1264, realtime+speed), and a live source build logs software-HEVC callback
  **~40 ms avg**. Never 650 ms. The earlier number was wrong.
- **No arm64 encoder helper exists or is needed.** The closed 1.1.0 dylib has *no*
  helper / `posix_spawn` / XPC and its encoder code (strings/symbols) is identical to
  the 1.2.0 source: same HEVC, same `Enable=YES/Require=NO`. The demonixis 1.2.0 tree
  has no helper target. The closed binary is just a universal build whose x86_64 slice
  runs under Rosetta exactly like the source build — and it is **also software HEVC**.
- **Hardware HEVC is genuinely unreachable under x86_64/Rosetta** (CrossOver Wine is
  x86_64-only, no arm64 tree). Probe result on this machine:
  `HEVC Require=YES → create fails -12908`; `HEVC Require=NO → UsingHardware=NO`
  (software); `H264 Require=YES/NO → UsingHardware=YES` (hardware, ~8 ms).
- **H.264 is NOT a usable fallback here:** the Quest client `VideoDecoder.cpp`
  hardcodes `video/hevc` and has no H.264 decoder; sending H.264 would black-screen it
  unless the Android client is also rebuilt/redeployed.

Conclusion: the source x86_64 build is HEVC-software (~40 ms), the same as the closed
binary. The real problem was never the codec — it was **callback starvation + too few
in-flight slots** causing drop cascades under CPU load.

## Fixes implemented this session (on fix/pacing-tracking-spaces)
4. **Encoder QoS hardening + hardware telemetry** (`VideoEncoder.mm`, `StreamingServer.cpp`):
   raised the Metal completion handler, VideoToolbox compression callback, and the
   encode / video-send / tcp-video threads to `QOS_CLASS_USER_INTERACTIVE`. Added a
   `UsingHardwareAcceleratedVideoEncoder` query logged as `hardware=YES/NO`.
   **Measured live:** vs the closed binary in the same session, callback p95 dropped
   from 161–446 ms to 57–125 ms.
5. **More encoder slots** (`VideoEncoder.h`): `SlotCount` 3 → 6. Software HEVC (~40 ms)
   overruns the 11–14 ms frame period; 3 slots left zero headroom so any jitter caused
   a drop cascade. **Measured live:** drop rate fell from ~55/s to ~14/s.
6. **STAGE render-path fix + floor calibration** (`Session.cpp`, `InputManager.cpp`,
   `Config.*`): `Session::LocateViews` ignored `baseSpace` and always returned
   STAGE-absolute eye poses, so the earlier reference-space fix never reached the
   rendered image; now transforms views into the requested reference space (identity
   for STAGE at offset 0, so no change to current STAGE behaviour). Added
   `stage_height_offset_m` (default 0) to manually calibrate the standing floor, and
   logging of the reference-space type each game creates. **Data:** SUPERHOT creates
   STAGE; standing head Y ≈ 1.2 m. Sign/magnitude of any needed offset requires an
   in-headset feel-test — not hardcoded.

## Build / deploy
- `cmake --build build/x86 --target oxrsys_runtime` (build/x86 = Release + x86_64).
  Confirm `lipo -info` = x86_64.
- Deploy: `~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib.closed-1.1.0.bak` is the
  known-good closed binary; the session build is staged as `.mybuild-1.2.0-qos`. Copy
  over `liboxrsys-runtime.dylib`, `codesign --force --sign -`. Relaunch the game to pick
  up a swap (restoring the file does nothing to a running process).
- Verify `hardware=` and `callback` ms in `~/Library/Application Support/OXRSys/oxrsys-runtime.log`.
