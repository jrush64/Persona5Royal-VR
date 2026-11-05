# Persona 5 Royal VR

A work-in-progress VR mod for Persona 5 Royal (Steam, Windows x64), built as an `.asi` plugin
loaded by Ultimate ASI Loader (`dinput8.dll`) next to `P5R.exe`. It hooks Direct3D 11 and the
game's GFD engine camera directly and submits to any OpenXR runtime (SteamVR, Meta Quest Link,
Virtual Desktop, etc.).

> **⚠️ Unfinished.** This is an experimental, in-progress build, published as-is. It runs
> through exploration, battle, Mementos and many cutscenes, but there are known open bugs
> (see [What doesn't work yet](#what-doesnt-work-yet)) and it has been tested on one PC and
> one headset setup. Expect rough edges.

Experimental community mod. Not affiliated with ATLUS or SEGA. Use at your own risk.

Licensed under [GPL-3.0](LICENSE).

## What it does

- **Real geometric stereo**, not reprojection - each frame's scene is rendered once per eye
  from per-eye cameras, using the game's own camera constant buffer (VS `b2`) and a replay of
  the scene pass for the second eye. Culling is widened so objects at the edge of one eye are
  not dropped.
- **Head tracking** - OpenXR rotation and position drive the game's view, with the final
  submitted image matched to the pose it was actually rendered with.
- **First-person camera** - optional first-person view from Joker's eyes on the field, in
  conversations, confidant scenes and cutscenes, with his body hidden. Fixed-camera rooms can
  follow the player instead.
- **HUD and menus in VR** - the game's 2D UI is separated from the world and shown on a
  docked panel in the headset; each HUD region (date, objectives, map, prompts...) can be
  placed or hidden, and the confidant/event panels can be split and repositioned.
  Cinematic letterbox bars can be removed.
- **In-game settings menu** (Insert) for stereo strength, convergence, world size, camera
  preset and HUD layout. Settings are saved to `p5r_camera.ini`.

## What doesn't work yet

- **Random flash during conversations** - a brief UI-like image occasionally flashes across
  the view during first-person conversations and cutscenes. Cause not yet found.
- **Intermittent fog** - fog can appear or disappear on camera/shot changes in some
  cutscenes.
- **Choice dimming** - the screen-dimming effect behind dialogue choices is still present.
- **Coverage** - not every room, event or battle has been checked. Earlier builds had flat
  fixed-camera rooms and intermittent right-eye failures; those paths have been repaired but
  not exhaustively re-tested.
- **Motion controllers** - not supported; play with a gamepad or keyboard.
- **ReShade** must be disabled (no `dxgi.dll` in the game folder).

[`docs/STATUS.md`](docs/STATUS.md) has more detail on what has and hasn't been verified.

## Controls

| Input | Action |
|---|---|
| Insert | Open / close the settings menu |
| F5, or hold R3 | Toggle the first-person camera |
| Home | Recenter head tracking |
| F11 / F12 | Toggle head rotation / head position |
| End | Toggle fill-headset framing |

The number-pad keys are developer toggles for performance and rendering experiments; leave
them alone unless you are debugging.

## Installing

Download the release zip and follow `release/README.txt` (the installer script checks your
game folder, handles ReShade, and can uninstall).

## Building

See [BUILD.md](BUILD.md). Short version: Visual Studio 2022 with the C++ workload, then
`scripts\build.bat`. All dependencies are vendored in `third_party/` - nothing else to
install. `scripts\test.bat` runs the offline regression tests.

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source (D3D11 hooks, GFD camera, stereo replay, HUD routing, OpenXR layer, ImGui menu) |
| `src/tests/` | Offline regression tests (D3D11 WARP, no game or headset needed) |
| `scripts/` | `build.bat`, `test.bat`, `deploy.bat` |
| `third_party/` | Vendored MinHook and Dear ImGui, used by the build |
| `docs/` | Current status (`STATUS.md`) |
| `tools/` | Python helpers for analyzing diagnostic captures and the game binary |
| `release/` | The install/uninstall script and shipped package layout |
| `runtime/` | A starting `p5r_camera.ini` |
| `builds/` | A prebuilt `MEVR_P5RProbe.asi` |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause) and Dear ImGui (MIT),
both under `third_party/` alongside their license files. The release package also ships
Ultimate ASI Loader (MIT) and the Khronos OpenXR loader (Apache-2.0); see
`release/THIRD_PARTY_NOTICES.txt`. All are compatible with GPL-3.0.
