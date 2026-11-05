# Building Persona 5 Royal VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` locates it
  automatically via `vswhere.exe`.
- No other tools or package managers required. MinHook and Dear ImGui are vendored in
  `third_party/` - nothing to fetch or install separately.

## Build

```
scripts\build.bat
```

Output: `builds\MEVR_P5RProbe.asi` (and `builds\MEVR_P5RProbe.map`).

The build is a single `cl.exe` invocation (no solution/project file, no CMake) that compiles
the mod's own sources together with the vendored MinHook and Dear ImGui sources, statically
linked (`/MT`), so the plugin needs no separate vcruntime redistributable in the game folder.
The OpenXR loader is loaded at runtime, so no OpenXR SDK is needed either.

Known harmless warnings: C4005 and C4805.

### Developer diagnostics

```
scripts\build.bat DIAG
```

Defines `P5R_DIAGNOSTICS`, which turns on the automatic frame recorder (rolling
192x108 thumbnails of both eyes and the HUD panel during first-person events, written to
`p5r_flash_*\frames.bin` in the game folder - several hundred MB per session) and an automatic
eye-image dump after ~25 s of stereo. Analyze recordings with `tools/analyze_flash_frames.py`.
Normal builds and the prebuilt `.asi` leave these out.

## Tests

```
scripts\test.bat
```

Builds and runs the offline regression tests in `src/tests/`: OpenXR-wrapper frame lifecycle,
D3D11 context isolation (on the WARP software device), menu rendering and input handling,
scene-graph/culling logic against synthetic fixtures, and cull-plane math. They do not launch
the game or verify anything in a headset.

## Deploying

```
scripts\deploy.bat "<Steam library>\steamapps\common\P5R"
```

Or copy these files by hand into the folder that contains `P5R.exe`:

- `builds\MEVR_P5RProbe.asi` (just built)
- `dinput8.dll` - [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) x64 (a copy is in `release/`)
- `openxr_loader.dll` - an x64 build of the official [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) (a copy is in `release/`)
- `p5r_camera.ini` (optional; see `runtime/p5r_camera.ini` for a starting point)

ReShade must be disabled: there must be no `dxgi.dll` in the game folder. The plugin writes
`p5rprobe_log.txt` and `p5r_xr.log` next to the game.

See `release/README.txt` for the installer script and end-user install steps.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| Ultimate ASI Loader | `release/dinput8.dll`, not built by this repo | MIT |
| OpenXR loader | `release/openxr_loader.dll`, not built by this repo | Apache 2.0 |
