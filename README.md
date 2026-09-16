# Flower VR injection — status

Goal: add real stereoscopic, head-tracked VR to the 2020 PC/Steam build of
Flower by hooking its DirectX 11 layer from outside the engine (no PhyreEngine
source available or needed). See git log for phase-by-phase history.

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

## The math (planned, not yet implemented at runtime)

Per draw call, engine computes `MVP = Model * View * Proj` (row-vector
convention). Since we have both `Model` and `MVP` for scene geometry:

```
ViewProj_mono = inverse(Model) * MVP      // recoverable per draw call
```

This should be IDENTICAL across every draw call in a given frame (single
shared camera) — first thing to verify once we hook this at runtime.

Camera FOV is already known and data-driven — `Data/Scripts/CameraInit.lua`
calls `camKeyPtr:fov(v.Fov)` per keyframe — so we can reconstruct `Proj_mono`
independently from a standard perspective-projection formula (fov, aspect,
near, far) rather than trying to decompose it out of the product. Then:

```
View_mono = ViewProj_mono * inverse(Proj_mono_reconstructed)
```

`View_mono` gives us the actual camera position/orientation. From there, per
eye:

```
View_eye = eye_offset(View_mono, ±IPD/2)   // translate along camera local X
Proj_eye = perspective(eye_fov, aspect, near, far)   // from OpenXR later
MVP_eye  = Model * View_eye * Proj_eye
```

Every draw call gets rendered twice (once per eye, with the corresponding
`MVP_eye` substituted into its cbuffer before the real `Draw`/`DrawIndexed`
call goes through), targeting two separate render targets.

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

## Next step (Phase 2, not yet done)

Implement actual stereo duplication: for shaders with `hasMVP`, override the
cbuffer contents with per-eye-corrected matrices and issue each draw call
twice (once per eye) into separate render targets, before standing up an
OpenXR session to drive real per-eye view/projection from HMD pose.

## Tools in this repo

- `build.bat` — builds the proxy `d3d11.dll` mod itself, deploys it + a
  renamed real copy (`d3d11_orig.dll`) into the game folder (one level up).
- `tools/build_reflect.bat` — builds `tools/build/reflect_shaders.exe`, an
  offline `D3DReflect`-based scanner. Usage:
  `reflect_shaders.exe <ShadersDir>` (scans recursively, prints matrix-like
  or camera-ish-named cbuffer vars) or `reflect_shaders.exe --all <file.cso>`
  (dumps every variable in every cbuffer, unfiltered).
