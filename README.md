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

## Next step (Phase 1c, not yet done)

Hook `VSSetConstantBuffers` (vtable slot 7) + `Map`/`UpdateSubresource` on
`ID3D11DeviceContext` to capture live `model`+`modelviewproj` pairs from
cbuffer slot 0 during actual gameplay, and confirm:
1. The recovered `ViewProj_mono` really is constant across a frame.
2. The reconstructed-from-FOV `Proj_mono` matches what falls out of the
   decomposition (validates the whole approach before writing the stereo
   duplication logic).

## Tools in this repo

- `build.bat` — builds the proxy `d3d11.dll` mod itself, deploys it + a
  renamed real copy (`d3d11_orig.dll`) into the game folder (one level up).
- `tools/build_reflect.bat` — builds `tools/build/reflect_shaders.exe`, an
  offline `D3DReflect`-based scanner. Usage:
  `reflect_shaders.exe <ShadersDir>` (scans recursively, prints matrix-like
  or camera-ish-named cbuffer vars) or `reflect_shaders.exe --all <file.cso>`
  (dumps every variable in every cbuffer, unfiltered).
