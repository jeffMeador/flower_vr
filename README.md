# Flower VR injection — status

Goal: add real stereoscopic, head-tracked VR to the 2020 PC/Steam build of
Flower by hooking its DirectX 11 layer from outside the engine (no PhyreEngine
source available or needed). See git log for phase-by-phase history.

## Current state (Phase 4) — playable in VR

Stereo, head-tracked, 90 fps per eye on SteamVR/OpenXR (tested on an RTX 5090,
wireless headset via Steam Link).

**Install:** run `build.bat` (deploys `d3d11.dll` + `d3d11_orig.dll` into the
GOG game folder), copy `vrmod.ini.example` → `<game>\vrmod.ini` and
`vrmod_Flower.cfg.example` → `<game>\vrmod_Flower.cfg`, start SteamVR, run
Flower.exe.

**How it works, end to end:**
- Every render-target/depth texture has a right-eye twin; each draw runs once
  per eye with per-eye matrices (`shadow.cpp`, `capture.cpp`, `stereo.cpp`).
- The game renders a square 2160×2160 frame in VR: `Documents\Flower\Flower.cfg`
  is redirected to `vrmod_Flower.cfg` (user's file untouched) and square
  display modes are advertised so the game accepts them (`fileredirect.cpp`,
  `displaymodes.cpp`). 4x MSAA.
- The engine camera is turned by the head (culling/grass follow gaze) while the
  steering code keeps the flight camera (`camoverride.cpp`); FOV forced to 125°.
- Each eye image is submitted to OpenXR with the exact pose it was rendered
  with (`xr.cpp`).

**Settings (`vrmod.ini`) / hotkeys:**

| Setting | Key | Notes |
|---|---|---|
| `worldScale` (0.3) | F10 bigger world / F11 smaller | 0.3 won a blind A/B vs 1.0. Clamped 0.1–10. |
| `lens` (off) | F2 game → fixed → off | Game's fisheye post pass; in VR it shrank the world at speed. |
| `cameraBack`/`cameraUp` | `[` `]` / `,` `.` | VR viewpoint offset from the game camera, game units. |
| `headCamera` (1) | F3 | Turn the engine camera with the head. |
| `render` (double) | — | `alternate` = old alternate-eye mode. |
| — | F6 | Recenter. |
| — | F12 | Dump both eyes of one frame (`frame_N.bmp`, `frame_N_R.bmp`). |
## Confirmed facts

- Renderer: DirectX 11 (`d3d11.dll` + `dxgi.dll` + `D3DCOMPILER_47.dll`), x64,
  no anti-tamper/packer.
- Flower.exe imports exactly one entry point from d3d11.dll:
  `D3D11CreateDeviceAndSwapChain` (the older combined call, not the modern
  DXGI-factory + CreateSwapChainForHwnd path). One hook covers everything.
- 424 precompiled shader blobs ship on disk at `Data/Shaders/{Debug,Release}/*.cso`
  — reflectable offline with `D3DReflect`, no need to run the game to inspect
  shader cbuffer layouts. **Use the `Release` folder** — it's what the shipped
  game actually loads; `Debug` shaders can have different (unstripped) cbuffer
  layouts and misled an early scan.
- All shader cbuffers are generically named `"UL"`/`"UG"` by PhyreEngine's
  cross-compiler regardless of purpose — bind slot, not name, distinguishes
  per-object (slot 0, "UL") vs. shared/lighting (slot 1, "UG") data.
- **The key finding:** the main scene shaders (MeshTex*, MeshCham*, MeshTerrain,
  Terrain*) upload BOTH a standalone `model` (world) matrix AND the
  premultiplied `modelviewproj` in the same per-object cbuffer (slot 0 "UL").
  Particle/UI/text shaders only upload the premultiplied MVP.

## The math (implemented, Phase 2a)

**Convention correction (Phase 2a):** Flower's cbuffers use the
*column-vector* convention — `clip = MVP * v`, matrices stored row-major, so
translation lives in column 3 (`m[i][3]`) and `clip.w` comes from row 3. The
Phase 1 notes below assumed row vectors; that was wrong, and the Phase 1
"recovered ViewProj" (`inverse(Model) * MVP`) was the wrong product (it looked
stable only because it was consistent garbage). Correct:

```
MVP = Proj * View * Model
ViewProj_mono = MVP * inverse(Model)      // per draw call that exposes `model`
```

Sanity check from live data: row 3 of the recovered VP (the w row) has
upper-3 length exactly 1.0000, and `|row1| / |row0|` = 3.5556 = 7680/2160.

Per-eye shift, no need to reconstruct Proj or View explicitly: translating
the view by `dx` along view-space X (`T * View`) with a symmetric projection
(`Proj` column 0 = `(xs, 0, 0, 0)`) gives

```
MVP_eye = MVP + (dx * xs) in element [0][3]        // xs = |VP row0 upper3|
modelView_eye[0][3] += dx
```

— a constant clip-space X offset, i.e. parallax proportional to `1/w`.
Applied to `modelViewProj`, `oldModelViewProj` (motion blur),
`viewprojection`/`viewProjMtx` (perspective only) and `modelView`. Matrices
with no perspective (w row = 0,0,0,1: UI, text, fullscreen passes) are
skipped automatically.
## Particles/petals/grass — the actual bulk of what's on screen

Checked explicitly because it's easy to hand-wave: `PetalSwarm_vs`,
`PetalInstance_vs`, `StemInstance_vs`, `Grass_vs`, `GrassRipple_vs`,
`Particle_vs`, `ParticleSoft_vs` (i.e. the core gameplay visuals — petals,
grass, stems) all expose **only** the combined `modelviewproj`, never a
separate `model`. This does NOT break the correction-matrix approach above —
that only needs `ViewProj_mono` recovered once per frame from any single
draw call that does expose `model` (terrain/mesh shaders run every frame
regardless), then applies uniformly to every other draw call including
these.

What it DOES add: these shaders also take `eyePositionWS`
(`StemInstance` also takes `eyeDirectionWS`) for camera-facing billboard
math computed inside the shader. Open design question for stereo: billboard
each petal toward *that eye's* position (correct per-eye disparity, but
known to cause visible "swimming" between eyes in other VR ports) vs. a
shared head-center position (stable, slightly-wrong billboard disparity but
standard practice). Decide empirically once live capture is running, not by
guessing.

`PetalInstance`/`StemInstance` read like GPU-instanced draws (one draw call
per swarm, per-instance offsets likely from instance vertex data, not the
cbuffer) — if so, `modelviewproj` there may already just be a bare
`ViewProj` with nothing to decompose at all, which would make petals the
*easy* case. Directly testable once the runtime hook below is live.

## Phase 1c: VALIDATED against live gameplay

Hooked `VSSetConstantBuffers`/`VSSetShader`/`Map`/`Unmap`/`UpdateSubresource`/
`Draw*` and captured live `model`+`modelviewproj` pairs during actual level-1
gameplay. Confirmed both key assumptions directly from data:

1. **`ViewProj_mono` is identical across every draw call within a frame** —
   sampled 10 different `DrawIndexed` calls at frame 4333, byte-identical
   recovered matrix every time.
2. **It's a real, live camera** — sampled across frames 4333→6841, the
   translation component drifts smoothly and continuously
   (-71.879 → -71.029 → ... → -103.392), matching continuous camera movement
   during flight, not a static/fluke value.

This validates the entire stereo-injection math plan below against real
gameplay, not just theory. Next step is Phase 2: actually rendering each draw
call twice (per-eye) and standing up OpenXR.

### Important pitfall hit and resolved: vtable-pointer-swap hooking does NOT work here

First hooking attempt patched the COM vtable pointers directly (classic
"vtable swap" technique). This appeared to silently fail — draw-call counters
stayed frozen at their startup values even during 40+ seconds of active
gameplay, across many failed hypotheses (deferred contexts, QueryInterface
tearoffs, Steam overlay, Xbox Game Bar — all ruled out one by one, including
testing against a GOG build with zero third-party modules loaded).

**Root cause** (confirmed via a hardware write-watchpoint + vectored
exception handler): the Windows D3D11 runtime (`d3d11.dll`/our renamed
`d3d11_orig.dll`) itself continuously re-writes its own vtable slots for
`VSSetConstantBuffers`/`VSSetShader`/`Draw*`/`Map`/`Unmap` many times per
frame as part of its own internal dispatch (fast-path/slow-path style
switching). Any hook that just overwrites the vtable *pointer* loses this
race and gets silently reverted before the game's own per-frame calls arrive.

**Fix**: switched to inline/detour hooking via MinHook (vendored in
`thirdparty/minhook`, MIT license, github.com/TsudaKageyu/minhook) — patches
the target function's own machine code instead of a pointer to it, so it's
immune to the runtime reassigning vtable slots. Worked immediately;
draw-call counters started climbing correctly within one test.

**Lesson for future sessions**: if vtable-swap hooking on ANY D3D11 method
appears to "not fire" despite no crash and normal-looking rendering, suspect
this same runtime behavior first — don't assume it's an external overlay
without directly confirming via a watchpoint.

### Aside: Steam vs GOG

Testing also hit a real (separate, now-irrelevant) Steam overlay complication:
`GameOverlayRenderer64.dll` gets injected into the Steam build's process
regardless of the in-game overlay checkbox (both per-game and global) unless
Steam itself is fully closed. Switched to testing against the GOG build
(`%USERPROFILE%\Desktop\Flower_GOG`, no Steam dependency) to get a clean
environment - `build.bat`'s `%GAMEDIR%` now points there. This turned out to
be a red herring for the *real* bug above (same freeze reproduced with zero
third-party modules loaded), but it's still good practice to keep testing
against the DRM-free build going forward to eliminate that variable.

## Phase 2a: alternate-eye stereo — WORKING in level 1

`src/stereo.cpp` + `PrepareDraw` in `src/capture.cpp`. Every Present flips
the eye; before each draw the bound slot-0 cbuffer is rewritten (from the
CPU shadow of what the game last uploaded, so it's idempotent) with that
eye's matrices. Game uploads per-object cbuffers with `Map(WRITE_DISCARD)`
(no NO_OVERWRITE seen), so shadowing at Unmap is complete.

Verified: consecutive frame dumps (F12) turned into a red/cyan anaglyph show
depth-correct parallax — near petal widely split, rocks less, distant hills
~0. With stereo off (F9) the frame is identical to vanilla.

Hotkeys (in-game): F9 stereo on/off, F10/F11 separation down/up (x1.25),
F8 billboard toward each eye vs. head center, F12 dump next two frames.
Config: `vrmod.ini` next to the game exe (see `vrmod.ini.example`).

Separation is in world units (default 0.065); world scale is unknown yet —
tune in-headset.

### Automated testing without touching the keyboard

The game ignores `SendInput` keyboard/mouse (raw input filters injected
events), so `src/fakepad.cpp` hooks `XInputGetState` and adds a virtual pad
driven by `vrmod_pad.txt` (enable with `[debug] fakepad=1`).
`tools/gameinput.ps1` wraps it: `Start-Level1` launches the GOG build and
drives the menu into level 1 (steer to the pot, hold RT immediately);
`Snap` screenshots the desktop; `Capture-StereoPair` makes an anaglyph.
This hook is also where VR controller input will plug in.

## Phase 2b: OpenXR output — WORKING (head-locked)

`src/xr.cpp`: loads SteamVR's `openxr_loader.dll` at runtime (headers in
`thirdparty/openxr`, no import lib), creates a session on the game's own
D3D11 device, and in Present copies the centered H×H square of the
backbuffer into the swapchain of the eye that frame was rendered for (AER),
then submits a projection layer in VIEW space (head-locked for now).
Retries until SteamVR/headset are up. Game vsync is disabled while the
session runs; xrWaitFrame paces (90 Hz on this headset → 45 Hz per eye).

**Display remap** (`StereoPatchClip`): each perspective MVP's rows 0/1 are
rescaled so the headset eye's (asymmetric) FOV lands exactly in that
centered square: `x' = ax·x + bx·w`, `y' = ay·y + by·w`. The layer is
submitted with the same FOV, so the image is geometrically correct.

**Culling fix** (`src/camoverride.cpp`): the game culls to its own camera
frustum (only ~14° vertical on 32:9!), so grass vanished outside a small
rectangle. The engine's single per-frame FOV write was found with the
`camfind` debug tool (F7: finds the camera block {fov, near 0.1, far 1330.5,
aspect} by value, then logs every instruction touching it via a hardware
watchpoint):

```
Flower.exe+0x3F5DE  mov   rcx,[rbx+28h]      ; engine camera
Flower.exe+0x3F5E2  movss xmm0,[rbx+60h]     ; wanted FOV (vertical degrees)
Flower.exe+0x3F5E7  ucomiss xmm0,[rcx+134h]  ; changed? -> store, vtable[0x98] rebuilds projection
```

Those 16 bytes are replaced (after verifying them) with a jump to a stub
that substitutes `[xr] gameFov` (default 125°) while the headset session is
running. Result: `ys = 0.52` (125° vertical), full coverage. The aspect write
(+0x140) doesn't stick, which is fine — horizontally the frustum is even
wider. Camera layout: fov +0x134, near +0x138, far +0x13C, aspect +0x140.

## Next steps

1. Motion controllers → virtual pad (`fakepad.cpp` hook): steer with a hand.
2. Headset-only polish: disable/adjust motion blur and depth of field in VR if
   they bother; HUD/menus as a floating quad instead of painted into the eyes.
3. Performance headroom: try 2644×2644 (headset's recommended) or 8x MSAA.
4. Steam build (same exe? verify patch bytes) and other levels.
## Tools in this repo

- `build.bat` — builds the proxy `d3d11.dll` mod itself, deploys it + a
  renamed real copy (`d3d11_orig.dll`) into the game folder (one level up).
- `tools/build_reflect.bat` — builds `tools/build/reflect_shaders.exe`, an
  offline `D3DReflect`-based scanner. Usage:
  `reflect_shaders.exe <ShadersDir>` (scans recursively, prints matrix-like
  or camera-ish-named cbuffer vars) or `reflect_shaders.exe --all <file.cso>`
  (dumps every variable in every cbuffer, unfiltered).
