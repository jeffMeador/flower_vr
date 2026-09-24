# Third-party notices

The mod's own code is under the MIT license (`LICENSE`). It includes or uses:

| Component | Where | License | Notes |
|---|---|---|---|
| MinHook (Tsuda Kageyu), incl. Hacker Disassembler Engine (Vyacheslav Patkov) | `thirdparty/minhook` | BSD 2-Clause | Compiled into `d3d11.dll`. Anyone distributing the built DLL must include `thirdparty/minhook/LICENSE.txt` with it. |
| OpenXR headers (The Khronos Group) | `thirdparty/openxr` | Apache-2.0 OR MIT | Headers only. `openxr_loader.dll` is not shipped; SteamVR's copy is loaded at runtime. |

Not included, and not to be redistributed:

- **Flower game files** (© thatgamecompany / Annapurna Interactive). The data
  overrides (`vrmod_overrides`) are generated on the player's PC from their
  own copy of the game by `tools/make_overrides.ps1`; the repo contains no
  game scripts or assets.
- **`d3d11_orig.dll`**: `build.bat` copies the player's own
  `%WINDIR%\System32\d3d11.dll` (Microsoft) next to the game. Never ship it
  with the mod.

This project is a fan mod, not affiliated with or endorsed by thatgamecompany
or Annapurna Interactive.
