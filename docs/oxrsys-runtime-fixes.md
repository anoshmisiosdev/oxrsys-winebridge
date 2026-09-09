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

## Fixes STILL TODO (not yet implemented)
4. **Encoder completion-callback thread QoS** (`VideoEncoder.mm`): under Mac CPU
   contention the VideoToolbox encode is instant (gpu ~0.3ms, submit ~0.003ms) but the
   async **completion callback** is delayed 40–380 ms → 9000+ encoder drops, ~90% stale
   frames, frame age 150 ms+, keyframe storms. Root cause is the callback thread being
   starved of CPU, not encode compute. **Fix: raise the completion-handler/dispatch-queue
   QoS to `user-interactive` (or a high-priority thread)** so it's scheduled promptly under
   load. Observed live on the closed binary under heavy load (concurrent builds + the ARM
   VideoToolbox benchmark competing for the same media engine).
5. **arm64 out-of-process encoder helper** (REQUIRED for the source build to be usable):
   x86_64/Rosetta cannot reach the hardware **HEVC** encoder — requiring HW HEVC fails
   `-12908`, and `RequireHardware=NO` (demonixis default in `VideoEncoder.mm`) silently
   falls back to **software HEVC (~627–650 ms/frame, ~8 fps)**. Confirmed: a plain x86_64
   `oxrsys_runtime`-only build regressed streaming to ~650 ms encode. The closed binary
   avoids this (universal build) — almost certainly via an **arm64 out-of-process encoder
   helper**. So: build/ship the arm64 encoder helper alongside the x86_64 runtime, else
   force hardware **H.264** under Rosetta (dingyifei's fallback) to at least stay functional.
   ARM's value here is *unlocking hardware HEVC* (~30–50% bandwidth win), not glue speed.

## Build / deploy
- `cmake -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_BUILD_TYPE=Release`, target `oxrsys_runtime`.
- Deploy: back up `~/liboxrsys-runtime-1.1.0/liboxrsys-runtime.dylib` first
  (`.closed-1.1.0.bak` exists), copy build over it, `codesign --force --sign -`.
  NOTE: restoring the dylib file does NOT affect an already-running server process —
  it must be relaunched to pick up a swap.
- Deploying an x86_64 `oxrsys_runtime` WITHOUT the encoder helper (#5) = software-HEVC
  regression. Verify encode codec + callback ms in the server log before trusting a build.
