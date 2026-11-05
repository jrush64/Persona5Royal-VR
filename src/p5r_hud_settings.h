#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include "p5r_camera_controls.h"

// [HUD] Placement of the game's HUD layer in the headset. The HUD image is split into regions that
// tile it completely; each region follows the head, stays docked in front of the player, or is
// hidden. Region rectangles are fractions of the HUD image (measured from P5R's field HUD).
namespace HudSettings {
enum Mode : int { Follow = 0, Dock = 1, Hide = 2 };
struct Region { const char* name; const wchar_t* key; float u0, v0, u1, v1; };
inline constexpr int kRegions = 6;
inline const Region kRegion[kRegions] = {
    { "Date",       L"Date",       0.00f, 0.00f, 0.30f, 0.32f },
    { "Top",        L"Top",        0.30f, 0.00f, 0.62f, 0.32f },
    { "Objectives", L"Objectives", 0.62f, 0.00f, 1.00f, 0.32f },
    { "Centre",     L"Centre",     0.00f, 0.32f, 0.72f, 0.80f },
    { "Map",        L"Map",        0.72f, 0.32f, 1.00f, 1.00f },
    { "Prompts",    L"Prompts",    0.00f, 0.80f, 0.72f, 1.00f },
};
struct Settings {
    float widthM = 1.8f, distanceM = 2.0f, heightM = 0.0f;
    int mode[kRegions] = { Dock, Dock, Dock, Dock, Dock, Dock };
    bool menusInVr = true;   // game menus on the docked panel over the VR world (false = flat screen)
    float cmWidthM = 1.6f, cmDistanceM = 2.2f, cmHeightM = -0.45f;   // [EVENTUI] confidant scenes
    // [CMMREGIONS] 10-09: confidant panel in two parts, each docked/hidden and raised/lowered on its own
    float evDim = 0.55f;   // [EVTDIM] events: max opacity of near-black areas (screen dims) - 1 = solid
    float cmSplit = 0.30f, cmTopY = 0.f, cmBottomY = 0.f; int cmTopMode = 1, cmBottomMode = 1;
    // [CMMPARTS] 10-11: four parts (top-left, top-right, bottom-left, bottom-right); each goes with Top (0), Bottom (1) or is hidden (2)
    enum PartAssign : int { WithTop = 0, WithBottom = 1, PartHidden = 2 };
    int cmPart[4] = { WithTop, WithTop, WithBottom, WithBottom }; float cmRightX = 0.72f;
    float evWidthM = 1.6f, evDistanceM = 2.2f, evHeightM = -0.45f;   // [EVENTUI] HUD panel during events (dialog box below the partner's face)
    bool systemFlat = false; // save, loading screens, network page: flat screen like Load Game (false = black VR panel)
};
inline SRWLOCK lock = SRWLOCK_INIT;
inline Settings settings;
inline std::atomic<bool> redock{true};   // anchor the docked regions in front of the head next frame
inline bool loaded = false;
inline Settings Valid(Settings v) {
    auto clamp = [](float x, float lo, float hi, float def) { return (x >= lo && x <= hi) ? x : (x < lo ? lo : (x > hi ? hi : def)); };
    v.widthM = clamp(v.widthM, 0.3f, 6.f, 1.8f);
    v.distanceM = clamp(v.distanceM, 0.4f, 8.f, 2.0f);
    v.heightM = clamp(v.heightM, -2.f, 2.f, 0.f);
    for (int& m : v.mode) if (m < Follow || m > Hide) m = Dock;
    return v;
}
inline Settings Get() { AcquireSRWLockShared(&lock); Settings v = settings; ReleaseSRWLockShared(&lock); return v; }
inline void Set(const Settings& v) { AcquireSRWLockExclusive(&lock); settings = Valid(v); ReleaseSRWLockExclusive(&lock); }
inline float ReadF(const wchar_t* key, float def) {
    wchar_t text[32] = {}, d[32]; swprintf_s(d, L"%.3f", def);
    GetPrivateProfileStringW(L"Hud", key, d, text, 32, CameraControls::iniPath);
    wchar_t* end = nullptr; float v = wcstof(text, &end); return end != text ? v : def;
}
inline void Load() {
    if (!CameraControls::iniPath[0]) return;
    loaded = true;
    Settings v;
    v.widthM = ReadF(L"WidthMeters", v.widthM); v.distanceM = ReadF(L"DistanceMeters", v.distanceM); v.heightM = ReadF(L"HeightMeters", v.heightM);
    for (int i = 0; i < kRegions; ++i) {
        wchar_t key[48]; swprintf_s(key, L"Mode%s", kRegion[i].key);
        v.mode[i] = GetPrivateProfileIntW(L"Hud", key, Dock, CameraControls::iniPath);
    }
    v.menusInVr = GetPrivateProfileIntW(L"Hud", L"MenusInVR", 1, CameraControls::iniPath) != 0;
    v.systemFlat = GetPrivateProfileIntW(L"Hud", L"SaveLoadingFlat", 0, CameraControls::iniPath) != 0;
    v.evWidthM = ReadF(L"EventWidthMeters", v.evWidthM); v.evDistanceM = ReadF(L"EventDistanceMeters", v.evDistanceM); v.evHeightM = ReadF(L"EventHeightMeters", v.evHeightM);
    v.evDim = ReadF(L"EventDarkOpacity", v.evDim); if (!(v.evDim >= 0.f && v.evDim <= 1.f)) v.evDim = 0.55f;
    v.cmSplit = ReadF(L"ConfidantSplit", v.cmSplit); v.cmTopY = ReadF(L"ConfidantTopHeight", v.cmTopY); v.cmBottomY = ReadF(L"ConfidantBottomHeight", v.cmBottomY);
    v.cmTopMode = GetPrivateProfileIntW(L"Hud", L"ConfidantTopMode", 1, CameraControls::iniPath); v.cmBottomMode = GetPrivateProfileIntW(L"Hud", L"ConfidantBottomMode", 1, CameraControls::iniPath);
    { const wchar_t* pk[4] = { L"ConfidantPartTL", L"ConfidantPartTR", L"ConfidantPartBL", L"ConfidantPartBR" };
      for (int i = 0; i < 4; ++i) { v.cmPart[i] = GetPrivateProfileIntW(L"Hud", pk[i], v.cmPart[i], CameraControls::iniPath); if (v.cmPart[i] < 0 || v.cmPart[i] > 2) v.cmPart[i] = i < 2 ? 0 : 1; }
      v.cmRightX = ReadF(L"ConfidantRightEdge", v.cmRightX); if (!(v.cmRightX > 0.3f && v.cmRightX < 0.98f)) v.cmRightX = 0.72f; }
    if (v.cmTopMode < 1 || v.cmTopMode > 2) v.cmTopMode = 1; if (v.cmBottomMode < 1 || v.cmBottomMode > 2) v.cmBottomMode = 1;
    if (!(v.cmSplit > 0.05f && v.cmSplit < 0.95f)) v.cmSplit = 0.30f;
    // confidant placement starts from the event values tuned so far (they were set in a confidant scene)
    v.cmWidthM = ReadF(L"ConfidantWidthMeters", v.evWidthM); v.cmDistanceM = ReadF(L"ConfidantDistanceMeters", v.evDistanceM); v.cmHeightM = ReadF(L"ConfidantHeightMeters", v.evHeightM);
    Set(v); redock = true;
}
inline bool Save() {
    if (!CameraControls::iniPath[0]) return false;
    const Settings v = Get(); bool ok = true; wchar_t text[32], key[48];
    swprintf_s(text, L"%.3f", v.widthM);    ok &= WritePrivateProfileStringW(L"Hud", L"WidthMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.distanceM); ok &= WritePrivateProfileStringW(L"Hud", L"DistanceMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.heightM);   ok &= WritePrivateProfileStringW(L"Hud", L"HeightMeters", text, CameraControls::iniPath) != FALSE;
    ok &= WritePrivateProfileStringW(L"Hud", L"MenusInVR", v.menusInVr ? L"1" : L"0", CameraControls::iniPath) != FALSE;
    ok &= WritePrivateProfileStringW(L"Hud", L"SaveLoadingFlat", v.systemFlat ? L"1" : L"0", CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.evDim);       ok &= WritePrivateProfileStringW(L"Hud", L"EventDarkOpacity", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmSplit);     ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantSplit", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmTopY);      ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantTopHeight", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmBottomY);   ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantBottomHeight", text, CameraControls::iniPath) != FALSE;
    { const wchar_t* pk[4] = { L"ConfidantPartTL", L"ConfidantPartTR", L"ConfidantPartBL", L"ConfidantPartBR" };
      for (int i = 0; i < 4; ++i) { swprintf_s(text, L"%d", v.cmPart[i]); ok &= WritePrivateProfileStringW(L"Hud", pk[i], text, CameraControls::iniPath) != FALSE; }
      swprintf_s(text, L"%.3f", v.cmRightX); ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantRightEdge", text, CameraControls::iniPath) != FALSE; }
    swprintf_s(text, L"%d", v.cmTopMode);     ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantTopMode", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%d", v.cmBottomMode);  ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantBottomMode", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmWidthM);    ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantWidthMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmDistanceM); ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantDistanceMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.cmHeightM);   ok &= WritePrivateProfileStringW(L"Hud", L"ConfidantHeightMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.evWidthM);    ok &= WritePrivateProfileStringW(L"Hud", L"EventWidthMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.evDistanceM); ok &= WritePrivateProfileStringW(L"Hud", L"EventDistanceMeters", text, CameraControls::iniPath) != FALSE;
    swprintf_s(text, L"%.3f", v.evHeightM);   ok &= WritePrivateProfileStringW(L"Hud", L"EventHeightMeters", text, CameraControls::iniPath) != FALSE;
    for (int i = 0; i < kRegions; ++i) {
        swprintf_s(key, L"Mode%s", kRegion[i].key); swprintf_s(text, L"%d", v.mode[i]);
        ok &= WritePrivateProfileStringW(L"Hud", key, text, CameraControls::iniPath) != FALSE;
    }
    return ok;
}
}
