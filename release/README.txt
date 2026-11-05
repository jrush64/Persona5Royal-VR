Persona 5 Royal VR  -  work-in-progress VR mod with head tracking for Persona 5 Royal (Steam)
by Halcyon

Copyright (c) 2026 Halcyon.
Licensed under the GNU General Public License v3.0 - see LICENSE.txt. You are
free to use, modify, and redistribute this software under the terms of that
license. Third-party files: see THIRD_PARTY_NOTICES.txt.


THIS IS AN UNFINISHED BUILD
It plays through exploration, battles, Mementos and many cutscenes, but it is
not done. Known problems:
  * a brief random flash sometimes appears during conversations/cutscenes
  * fog can pop in and out on some cutscene camera changes
  * the dimming behind dialogue choices is still there
  * not every room/event has been tested; one PC + headset setup so far
  * no motion controller support - use a gamepad or keyboard


INSTALL
1. Extract ALL the files from the zip (P5R-VR.bat, MEVR_P5RProbe.asi,
   dinput8.dll, openxr_loader.dll, p5r_camera.ini) into the folder that
   has P5R.exe. In Steam: right-click Persona 5 Royal -> Manage ->
   Browse Local Files.
   If you ALREADY have a dinput8.dll there for other mods, it is
   probably Ultimate ASI Loader too - keep yours and skip ours.
2. Double-click  P5R-VR.bat  FROM THAT FOLDER and pick Install.
   It checks the files, and turns off ReShade if it finds it (ReShade
   breaks this mod). If Windows needs Administrator rights for that
   folder, it will ask - click Yes.
3. Start your VR runtime (SteamVR, Quest Link, Virtual Desktop...),
   then launch the game normally from Steam.


CONTROLS
* INSERT        - open / close the mod menu (stereo, convergence, world
                  size, camera, HUD layout)
* F5 or hold R3 - toggle first-person camera
* HOME          - recenter your view
* F11 / F12     - toggle head rotation / head position
* END           - toggle fill-headset framing

Settings are saved to p5r_camera.ini in the game folder.
The number-pad keys are developer toggles - leave them alone.


IF SOMETHING GOES WRONG
The mod writes p5rprobe_log.txt and p5r_xr.log next to P5R.exe. Include
both when reporting a problem.


UNINSTALL
Run  P5R-VR.bat  AGAIN and pick Uninstall.

Or just delete MEVR_P5RProbe.asi (and openxr_loader.dll / dinput8.dll if
nothing else uses them) from the game's folder.
