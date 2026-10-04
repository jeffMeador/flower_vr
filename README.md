# Flower VR

An unofficial VR mod for the PC version of [Flower](https://thatgamecompany.com/flower/) (thatgamecompany, 2009/2019): full stereo 3D, head tracking and motion controls, played through SteamVR.

[![Flower VR gameplay video](https://img.youtube.com/vi/A30Fg2g_Vfo/maxresdefault.jpg)](https://youtu.be/A30Fg2g_Vfo)

*Click to watch the gameplay video on YouTube.*

You fly as the wind, with the petals just ahead of you; turning your head looks around the world, and tilting the controller steers, like the original PS3 tilt controls.

**New: [Journey](https://thatgamecompany.com/journey/) (Steam) in alpha.** The same DLL also runs Journey in VR: see [Journey (alpha)](#journey-alpha).

Fan project, not affiliated with or endorsed by thatgamecompany or Annapurna Interactive. You need your own copy of the game.

## Features

- **Stereo 3D, both eyes every frame,** head-tracked, at the headset's refresh rate (90 Hz tested).
- **The world follows your gaze:** grass and culling are generated for where you look, not only where you fly.
- **Motion controls:** tilt the controller left/right to turn and nose up/down to climb or dive; hold the trigger to fly. Thumbstick also works. Tilt also picks the level in the menu.
- **Comfort:** the view sits behind and a little above and to the left of the petals; the game's fisheye lens effect and distance blur are off in VR.
- **Menus and title screens** show on a floating screen; the level intro movies (which are black on some PCs) are skipped.
- **Your files stay untouched:** the mod reads its own settings and a generated override folder; delete them to uninstall.

## Requirements

- **Flower for PC, GOG or Steam version.** The same mod works with both.
- Windows 10/11 and a SteamVR-compatible headset (OpenXR through SteamVR). Tested with a Steam Frame over Steam Link.
- A strong GPU for the recommended settings: on an RTX 5090, 2644 x 2644 per eye with 8x MSAA uses about 6 ms of the 11 ms frame budget at 90 Hz. Lower the resolution or MSAA in `vrmod_Flower.cfg` for weaker cards.
- To build: Visual Studio with the C++ x64 tools. `build.bat` calls Visual Studio 2017 Community's `vcvars64.bat`; change that path for another version.

## Install

Your Flower folder is the GOG install folder, or for Steam e.g. `C:\Program Files (x86)\Steam\steamapps\common\Flower` (right-click Flower in Steam → Manage → Browse local files).

### Option A: download the DLL

1. Download `d3d11.dll` from the [latest release](https://github.com/jeffMeador/flower_vr/releases/latest). One file works with both the GOG and the Steam version (1.0 was GOG only). To check it, run `Get-FileHash d3d11.dll` in PowerShell and compare with the SHA-256 in the release notes.
2. Copy it into your Flower folder, next to `Flower.exe`.
3. Copy `C:\Windows\System32\d3d11.dll` into the same folder and rename the copy `d3d11_orig.dll`. This is the real Direct3D from your own Windows, which the mod forwards to; never share it.
4. **Play.** Start SteamVR with the headset connected, then launch Flower. On first launch the mod creates `vrmod.ini` (recommended settings, 2160 x 2160 per eye) and `vrmod_Flower.cfg` (a copy of your normal settings with a square screen, 4x MSAA). For the full quality on a fast GPU, replace them with the `.example` files from this repository.

Some antivirus programs warn about unsigned DLLs that hook a game; that's expected for mods like this. The level movies are black on PC even without the mod; Option B also turns them off.

### Option B: build from source

1. **Build and deploy.** Edit `GAMEDIR` at the top of `build.bat` to your Flower folder, then run `build.bat` from this folder. It:
   - builds `d3d11.dll` (the mod) and copies it into the game folder,
   - copies your own `C:\Windows\System32\d3d11.dll` next to it as `d3d11_orig.dll` (the real Direct3D, which the mod forwards to; never share this file),
   - generates the override folder `vrmod_overrides` from your copy of the game (movie skip).
2. **Settings.** Copy `vrmod.ini.example` to `<game folder>\vrmod.ini` and `vrmod_Flower.cfg.example` to `<game folder>\vrmod_Flower.cfg`. The latter is the game's own settings file for VR (square resolution, MSAA, grass); your normal `Documents\Flower\Flower.cfg` is left alone. If you skip this step, the mod creates both on first launch: `vrmod.ini` with the recommended settings but a 2160 square, and `vrmod_Flower.cfg` as a copy of your normal settings with a windowed square screen at `maxSquare`, 4x MSAA.
3. **Play.** Start SteamVR with the headset connected, then launch Flower (from Steam for the Steam version, or `Flower.exe` in the game folder for GOG).

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

Tilt is measured against the horizon, like the PS3 controls: holding the controller level means straight, so there is nothing to reset. "Level" is the controller's nose pointed slightly up (10°), which felt the most natural; change `pitchRestDegrees` if you hold it differently.

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
| `[xr] pitchRestDegrees` | 10 | Nose angle that counts as straight for climbing (negative = nose down). |
| `[xr] invertStickY` | 1 | Thumbstick up = dive in levels. |
| `[xr] maxSquare` | 2644 | Must match the square resolution in `vrmod_Flower.cfg`. |

Resolution, MSAA and grass density are in `vrmod_Flower.cfg` (`Screen Width/Height`, `MultiSampleCount`, the `Grass` section). Keep the resolution square.

## Journey (alpha)

The same `d3d11.dll` runs **Journey (Steam version)** in VR. It's playable through most of the game, but rough in places; see the known issues below. Download it from the [v1.2-alpha release](https://github.com/jeffMeador/flower_vr/releases/tag/v1.2-alpha) (the "latest release" link still points to the stable Flower build).

**Install:** same as Flower, into the Journey folder (in Steam: right-click Journey → Manage → Browse local files): copy `d3d11.dll` next to `Journey.exe`, and copy `C:\Windows\System32\d3d11.dll` there renamed to `d3d11_orig.dll`. On first launch the mod creates `vrmod.ini` and `vrmod_Journey.cfg` (a copy of your Journey settings with a square window); your normal Journey settings are left alone.

**Play:** connect the headset to SteamVR, then launch Journey. On a Steam Frame, connect Steam Link VR before or right after launching; Journey waits on a black screen until SteamVR has the headset.

| Control | Action |
|---|---|
| Left thumbstick | Walk |
| Right thumbstick | Turn the camera |
| A / X / trigger | Jump and fly |
| B / Y / grip | Sing (hold for a louder call) |
| Menu button | Pause |

What it does:
- **You play in full VR,** with head tracking and the culling, terrain and shadows following your head.
- **The title screen, menus and cutscenes play on a big widescreen in front of you,** with a fade through white when switching. Free head movement inside the game's cutscene shots made people sick and showed unfinished parts of the scenes.
- **Pause and the idle screen stay in VR;** the "JOURNEY" logo is hidden there.
- **The "JOURNEY" title stands in the world,** and tutorial prompts are placed at a comfortable distance.
- **Depth of field and motion blur are off; the heat shimmer is reduced** to monitor strength (`[stereo] heatShimmer`, 0 to 1, default 0.3).
- **Online companions work.** The mod only changes what you see, not the game's movement or network data.

Journey settings in `vrmod.ini`: `[xr] cinematicMode` (`auto`, `follow` = always VR, `screen` = always the screen), `cinemaSize`/`cinemaDistance`/`cinemaAspect` (the screen, meters), `cameraSteady` (seconds of smoothing on the camera's distance to you, 0 = off), `promptDistance` (meters). `[debug] cinemaToggle=1` lets the right thumbstick click switch screen/VR by hand.

Known issues (alpha):
- **Cutscene detection misses some scenes:** some end-of-chapter visions and story scenes play in VR instead of on the screen.
- **Idle and some cutscene camera angles feel off in VR.**
- **Some distant lights and flames flicker differently in each eye.**
- **The game must be launched with the headset already in SteamVR** (on the Frame: Steam Link VR connected); launching from the Frame's flat library without that stays on a black screen.
- Tested on one setup (RTX 5090, Steam Frame via Steam Link).

## How it works

The mod is a proxy `d3d11.dll` loaded by the game. It hooks Direct3D 11 and never touches the game's files:

- It recovers the camera from the game's shader constants and re-renders every draw for the second eye with per-eye matrices. The right eye is recorded on a deferred context and replayed in one block per frame, which halves GPU time compared to switching eyes per draw.
- The finished eye images go to the headset through OpenXR (SteamVR).
- A small code patch widens the engine camera's field of view and turns it with your head, so grass is generated where you look; the steering code still sees the flight camera, so the controls don't reverse.
- Controllers feed a virtual Xbox pad the game already understands.

The full engineering log is in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Decisions

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
