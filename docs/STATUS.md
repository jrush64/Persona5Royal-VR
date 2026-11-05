# Status

Snapshot of what has and has not been verified in this work-in-progress build. Results
below are from headset testing on a single PC; offline tests (`scripts\test.bat`) cover
logic and D3D11 state handling but cannot show what reaches the headset.

## Verified in the headset

- Geometric stereo through exploration, battle and Mementos.
- Head tracking (rotation and position), with the final submitted image matched to the
  pose it was rendered with.
- First-person view in conversations, confidant scenes and cutscenes, facing along Joker's
  head heading (horizontal projection of the head bone's forward axis).
- Battle culling: ground and characters stay visible at the edges of each eye.
- Removal of the top/bottom cinematic letterbox bars in conversations (dialogue
  backgrounds, portraits and text are left alone).
- HUD and menus on a docked VR panel, with per-region placement.

## Open bugs

| Bug | Behavior | Where to look |
|---|---|---|
| Random flash | A brief UI-like image flashes across the view during first-person conversations/cutscenes, often near the start of a conversation. Origin unknown - not yet localized to the eye images or the HUD panel. | Build with `scripts\build.bat DIAG` to enable the frame recorder, reproduce, then run `tools/analyze_flash_frames.py` on the `p5r_flash_*\frames.bin` it writes. |
| Intermittent fog | Fog appears/disappears on shot or camera changes, or when advancing dialogue, in more than one scene. No first-person vs third-person comparison yet; not assumed to share the flash's cause. | Same recorder (it flags haze steps); `[haze]` lines in `p5rprobe_log.txt`. |
| Choice dimming | The dimming behind dialogue choices is still drawn. A texture filter that removed it also removed dialogue background art and was disabled. Not currently being worked on. | `src/p5r_hud.inl` |

## Things that were tried and did not work

- Broad overlay/post-effect suppression did not fix the flash or fog. The
  `RemoveEventOverlay` setting is left over from that and is not a proven fix.
- A per-draw effect GPU capture produced no live output and was removed; its source
  (`p5r_effect_capture.inl`, `p5r_fp_audit.inl`) remains in `src/` as unused reference.
- Two apparent right-eye mismatches turned out to compare different draw/submit serials,
  not a stereo bug.

## Source map

| File | What it does |
|---|---|
| `src/p5r_probe.cpp` | Plugin entry, D3D11 hooks, per-eye camera injection (`InjectEyeShift`), present loop, hotkeys |
| `src/p5r_gfdcam.cpp` | GFD engine camera, scene and event discovery; `EventEye` first-person camera |
| `src/p5r_xr.cpp` | OpenXR session, swapchains, eye/HUD submission (`SubmitEyesImpl`, `CopyToEye`) |
| `src/p5r_hud.inl` | Separating UI from world draws, HUD panel colour/alpha reconstruction |
| `src/p5r_screen_tasks.inl` | Event/menu/loading screen detection, letterbox removal |
| `src/p5r_geometry_cull.inl`, `p5r_visibility_test.inl` | Event and battle culling overrides |
| `src/p5r_menu.cpp` | Insert settings menu (Dear ImGui) |
| `src/p5r_flash_frames.inl` | Diagnostic frame recorder (only active in `DIAG` builds) |
