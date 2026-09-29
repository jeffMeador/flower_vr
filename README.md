# Flower VR

An unofficial VR mod for the PC version of [Flower](https://thatgamecompany.com/flower/) (thatgamecompany, 2009/2019): full stereo 3D, head tracking and motion controls, played through SteamVR.

[![Flower VR gameplay video](https://img.youtube.com/vi/A30Fg2g_Vfo/maxresdefault.jpg)](https://youtu.be/A30Fg2g_Vfo)

*Click to watch the gameplay video on YouTube.*

You fly as the wind, with the petals just ahead of you; turning your head looks around the world, and tilting the controller steers, like the original PS3 tilt controls.

Fan project, not affiliated with or endorsed by thatgamecompany or Annapurna Interactive. You need your own copy of the game.

## Features

- **Stereo 3D, both eyes every frame,** head-tracked, at the headset's refresh rate (90 Hz tested).
- **The world follows your gaze:** grass and culling are generated for where you look, not only where you fly.
- **Motion controls:** tilt the controller left/right to turn and nose up/down to climb or dive; hold the trigger to fly. Thumbstick also works. Tilt also picks the level in the menu.
- **Comfort:** the view sits behind and a little above and to the left of the petals; the game's fisheye lens effect and distance blur are off in VR.
- **Menus and title screens** show on a floating screen; the level intro movies (which are black on some PCs) are skipped.
- **Your files stay untouched:** the mod reads its own settings and a generated override folder; delete them to uninstall.

## Requirements

- **Flower for PC, GOG version.** The Steam version is not supported (see [Decisions](#decisions)).
- Windows 10/11 and a SteamVR-compatible headset (OpenXR through SteamVR). Tested with a Steam Frame over Steam Link.
- A strong GPU for the recommended settings: on an RTX 5090, 2644 x 2644 per eye with 8x MSAA uses about 6 ms of the 11 ms frame budget at 90 Hz. Lower the resolution or MSAA in `vrmod_Flower.cfg` for weaker cards.
- To build: Visual Studio with the C++ x64 tools. `build.bat` calls Visual Studio 2017 Community's `vcvars64.bat`; change that path for another version.

## Install

**From the release zip (no build tools needed):** download `FlowerVR-1.0.zip` from [Releases](https://github.com/jeffMeador/flower_vr/releases), unzip everything into your Flower folder next to `Flower.exe`, and double-click `install.bat`. Then start SteamVR with the headset connected and launch `Flower.exe`.

**From source:**

1. **Build and deploy.** Edit `GAMEDIR` at the top of `build.bat` to your Flower (GOG) folder, then run `build.bat` from this folder. It:
   - builds `d3d11.dll` (the mod) and copies it into the game folder,
   - copies your own `C:\Windows\System32\d3d11.dll` next to it as `d3d11_orig.dll` (the real Direct3D, which the mod forwards to; never share this file),
   - generates the override folder `vrmod_overrides` from your copy of the game (movie skip).
2. **Settings.** Copy `vrmod.ini.example` to `<game folder>\vrmod.ini` and `vrmod_Flower.cfg.example` to `<game folder>\vrmod_Flower.cfg`. The latter is the game's own settings file for VR (square resolution, MSAA, grass); your normal `Documents\Flower\Flower.cfg` is left alone. If you skip this step, the mod creates both on first launch: `vrmod.ini` with the recommended settings but a 2160 square, and `vrmod_Flower.cfg` as a copy of your normal settings with a windowed square screen at `maxSquare`, 4x MSAA.
3. **Play.** Start SteamVR with the headset connected, then launch `Flower.exe` from the game folder.

**Uninstall:** delete `d3d11.dll`, `d3d11_orig.dll`, `vrmod.ini`, `vrmod_Flower.cfg`, `vrmod.log` and the `vrmod_overrides` folder from the game folder.

## Playing

| Control | Action |
|---|---|
| Hold trigger, grip, A or X | Fly (full speed past about a third of a press) |
| Tilt controller left / right | Turn (full turn at 45 degrees) |
| Tilt nose up / down | Climb / dive (full at 30 degrees from your rest angle) |
| Thumbstick | Steer (pushed up = dive in levels) |
| Hold B or Y for 1 s | Switch between motion and thumbstick steering |
| Menu button | Pause |
| In the menu | Tilt to pick a level, trigger to choose |

Tilt is measured against the horizon, like the PS3 controls: holding the controller level means straight, so there is nothing to reset. If you naturally hold it nose-down, set `pitchRestDegrees` (e.g. -10) so that feels like level flight.

**To quit, use the game's pause menu.** Don't force-close the game while it's running in VR.

Keyboard hotkeys (only while the game window has focus): F6 recenter the view, `[` `]` camera back/forward, `,` `.` camera down/up (saved to `vrmod.ini`), F1 depth of field, F2 lens effect, F3 head-turned camera, F12 save both eyes as images.

## Settings

`vrmod.ini` (next to `Flower.exe`); the example has the tested values.

| Key | Default | What it does |
|---|---|---|
| `[stereo] worldScale` | 0.3 | Size of the world; lower = bigger. 1.0 is "true scale", which felt miniature. |
| `[stereo] depthOfField`, `dofNear`, `dofFar` | 1, 1, 0 | The game's depth of field; `dofFar=0` keeps the distance sharp while nearby sparkles stay soft. |
| `[stereo] sparkleSize` | 0.5 | Size of the sparkles in the grass (they're drawn at a fixed screen size and look huge in VR). |
| `[stereo] motionBlur` | 1 | The game's motion blur. |
| `[stereo] lens` | off | The game's fisheye lens pass (`game` / `fixed` / `off`). |
| `[xr] cameraBack`, `cameraUp`, `cameraSide` | 1.50, 0.55, -0.25 | Where you sit relative to the petals (game units; negative side = left). |
| `[xr] cameraFlights` | 1 | 0 stops the game's scripted camera flights to viewpoints (they're part of the game's storytelling, so they're on by default). |
| `[xr] steering` | motion | `motion` or `stick`. |
| `[xr] tiltTurnDegrees` | 45 | Controller tilt for a full turn. |
| `[xr] pitchRestDegrees` | 0 | Nose angle that counts as straight for climbing (negative = nose down). |
| `[xr] invertStickY` | 1 | Thumbstick up = dive in levels. |
| `[xr] maxSquare` | 2644 | Must match the square resolution in `vrmod_Flower.cfg`. |

Resolution, MSAA and grass density are in `vrmod_Flower.cfg` (`Screen Width/Height`, `MultiSampleCount`, the `Grass` section). Keep the resolution square.

## How it works

The mod is a proxy `d3d11.dll` loaded by the game. It hooks Direct3D 11 and never touches the game's files:

- It recovers the camera from the game's shader constants and re-renders every draw for the second eye with per-eye matrices. The right eye is recorded on a deferred context and replayed in one block per frame, which halves GPU time compared to switching eyes per draw.
- The finished eye images go to the headset through OpenXR (SteamVR).
- A small code patch widens the engine camera's field of view and turns it with your head, so grass is generated where you look; the steering code still sees the flight camera, so the controls don't reverse.
- Controllers feed a virtual Xbox pad the game already understands.

The full engineering log is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Decisions

- **GOG, not Steam.** The Steam build is a different compile: one of the code sites the mod patches (the steering) isn't found there yet, and the Steam overlay complicated early testing. The GOG build is DRM-free and was the reference throughout. Steam support is possible later.
- **Standalone on the Steam Frame: it works, but it's parked.** The GOG game runs on the headset itself through Proton and FEX with VR through Proton's OpenXR bridge, at 72 fps with 1600 x 1600 per eye and Medium grass (the code is included - see `tools/frame` and the development log). The mobile GPU can't do the grass density, resolution and anti-aliasing that make the game look good, so streaming from a PC is the way to play.
- **Batched double render.** Drawing both eyes by switching render targets per draw was fine on desktop GPUs but broke every render pass on the Frame's tile-based GPU (13 ms vs 8 ms). Recording the right eye and replaying it in one block fixed that and also halved the PC's GPU time, which paid for 2644-per-eye rendering with 8x MSAA.
- **No DLSS/FSR.** They need per-pixel motion vectors the game doesn't produce, and frame generation doesn't apply to VR. With the GPU headroom, plain resolution and MSAA do the job.
- **Depth of field: near kept, far off.** Distance blur looks wrong in a headset, but the near blur gives the sparkles their soft glow.
- **Lens effect off.** The game warps the image like a fisheye as you speed up; in VR that made the world feel like it was shrinking.
- **World scale 0.3,** chosen in a blind A/B test against true scale.
- **Camera flights stay on.** They can be disorienting in VR, but they're how the game shows you things; `cameraFlights=0` turns them off.
- **No game files in this repository.** Overrides (movie skip, optional camera-flight removal) are generated from your own copy by `tools/make_overrides.ps1`.

## Known issues

- The game's chase camera sometimes swings toward flower patches on its own, which can be disorienting.
- The first title card ("originally released on PlayStation") is cut off at the sides.
- The flower's own sparkles are still fairly large.
- Tested on one setup (RTX 5090, Steam Frame via Steam Link).

## License

MIT for the mod's code ([LICENSE](LICENSE)); see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for MinHook and the OpenXR headers. Written by Claude (Anthropic), directed by jeffMeador.
