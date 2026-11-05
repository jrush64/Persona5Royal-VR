#pragma once
#include <windows.h>
#include <d3d11.h>
namespace P5RMenu {
extern thread_local bool internalCall;
bool IsOpen();
extern BOOL (WINAPI* originalSetCursorPos)(int,int);
extern BOOL (WINAPI* originalSetPhysicalCursorPos)(int,int);
extern BOOL (WINAPI* originalClipCursor)(const RECT*);
BOOL WINAPI BlockGameCursorWarp(int,int);
BOOL WINAPI BlockGamePhysicalWarp(int,int);
BOOL WINAPI BlockGameCursorClip(const RECT*);
// All UI and D3D work runs on Present's thread. The window procedure only filters input.
struct Actions { bool toggleStereo=false, recenter=false; };
Actions Begin(ID3D11Device*, ID3D11DeviceContext*, HWND, UINT width, UINT height, bool stereo, bool ready);
void Draw(ID3D11Texture2D* left, ID3D11Texture2D* right);
bool WasDrawn();
void Shutdown();
}
