#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

// Manual offsets relative to the game's camera, before head tracking and eye separation.
// This is deliberately independent of game object addresses and character skeletons.
namespace CameraControls {
struct Settings {
    float right = 0, up = 0, forward = 0;
    float yaw = 0, pitch = 0; // degrees; positive turns right / looks up
    bool hideBody = true; // Hide only while the manual first-person preset is active.
    bool followInRooms = true; // [ROOMCAM] fixed-camera rooms (Leblanc...) use the normal follow camera
    bool eventFirstPerson = true; // [EVENTFP] events/confidants seen from Joker's eyes, facing who he talks to
    bool removeLetterbox = true;  // [LETTERBOX] no cinematic bars in events
    bool cutsceneFirstPerson = true; // [CUTFP] first person also in non-confidant events
    bool removeEventOverlay = false; // [NORENDER] broad post-effect suppression; off after live fog/red regressions
    bool wideVisibility = true;
    bool headPosition = false; // Rotation-only by default; lean is opt-in.
    bool enabled = false; // F5 toggles the tuned first-person preset.
    float eyeSeparationMm = 64, convergenceMeters = 0, convergenceShift = 0, worldSize = 1;
    bool fillHeadset = true;
    bool swapEyes = false;
    int eyePreview = 0; // session-only: 0 stereo, 1 left image in both eyes, 2 right image in both
};
inline bool UseManual(const Settings& value){return value.enabled;}
inline SRWLOCK lock = SRWLOCK_INIT;
inline Settings settings;
inline std::atomic<bool> wideVisibilityEnabled{true};
inline wchar_t iniPath[MAX_PATH] = {};
inline bool initialized = false;
inline Settings Get() {
    AcquireSRWLockShared(&lock); Settings result = settings; ReleaseSRWLockShared(&lock);
    return result;
}
inline float Clamp(float x, float limit) { return std::isfinite(x) ? (std::max)(-limit, (std::min)(limit, x)) : 0.f; }
inline void Set(Settings value) {
    value.right = Clamp(value.right, 20000); value.up = Clamp(value.up, 20000); value.forward = Clamp(value.forward, 20000);
    value.yaw = Clamp(value.yaw, 180); value.pitch = Clamp(value.pitch, 89);
    value.eyeSeparationMm = (std::max)(0.f, Clamp(value.eyeSeparationMm, 128));
    value.convergenceMeters = (std::max)(0.f, Clamp(value.convergenceMeters, 100));
    if (value.convergenceMeters > 0 && value.convergenceMeters < 0.25f) value.convergenceMeters = 0.25f;
    value.convergenceShift = Clamp(value.convergenceShift,1);
    value.worldSize = std::isfinite(value.worldSize) ? (std::max)(0.25f, (std::min)(4.f,value.worldSize)) : 1.f;
    AcquireSRWLockExclusive(&lock); settings = value; wideVisibilityEnabled=value.wideVisibility; ReleaseSRWLockExclusive(&lock);
}
inline float Read(const wchar_t* key, float fallback) {
    wchar_t text[64] = {}, def[64]; swprintf_s(def, L"%.6f", fallback);
    GetPrivateProfileStringW(L"Camera", key, def, text, 64, iniPath);
    wchar_t* end = nullptr; float result = wcstof(text, &end);
    return end != text && *end == 0 && std::isfinite(result) ? result : fallback;
}
inline void Load() {
    Settings value;
    value.right = Read(L"Right", 0); value.up = Read(L"Up", 0); value.forward = Read(L"Forward", 0);
    value.yaw = Read(L"Yaw", 0); value.pitch = Read(L"Pitch", 0); value.enabled = Read(L"Enabled", 0) != 0;
    value.swapEyes = Read(L"SwapEyes",0) != 0;
    value.fillHeadset = Read(L"FillHeadset",1) != 0;
    value.eyeSeparationMm = Read(L"EyeSeparationMm",64);
    value.convergenceMeters = Read(L"ConvergenceMeters",0); value.worldSize = Read(L"WorldSize",1);
    value.convergenceShift=Read(L"ConvergenceShift",0);
    value.wideVisibility=Read(L"WideVisibility",1)!=0;
    value.headPosition=Read(L"HeadPosition",0)!=0;
    value.hideBody=Read(L"HideBody",1)!=0;
    value.followInRooms=Read(L"FollowInRooms",1)!=0;
    value.eventFirstPerson=Read(L"EventFirstPerson",1)!=0;
    value.removeLetterbox=Read(L"RemoveLetterbox",1)!=0;
    value.cutsceneFirstPerson=Read(L"CutsceneFirstPerson",1)!=0;
    value.removeEventOverlay=Read(L"RemoveEventOverlay",0)!=0;
    Set(value);
}
inline bool Save() {
    const Settings value = Get();
    const wchar_t* keys[] = {L"Right",L"Up",L"Forward",L"Yaw",L"Pitch",L"Enabled",L"EyeSeparationMm",L"ConvergenceMeters",L"WorldSize",L"FillHeadset",L"SwapEyes",L"HideBody",L"HeadPosition",L"WideVisibility",L"ConvergenceShift",L"FollowInRooms",L"EventFirstPerson",L"RemoveLetterbox",L"RemoveEventOverlay",L"CutsceneFirstPerson"};
    const float values[] = {value.right,value.up,value.forward,value.yaw,value.pitch,value.enabled ? 1.f : 0.f,value.eyeSeparationMm,value.convergenceMeters,value.worldSize,value.fillHeadset ? 1.f : 0.f,value.swapEyes ? 1.f : 0.f,value.hideBody?1.f:0.f,value.headPosition?1.f:0.f,value.wideVisibility?1.f:0.f,value.convergenceShift,value.followInRooms?1.f:0.f,value.eventFirstPerson?1.f:0.f,value.removeLetterbox?1.f:0.f,value.removeEventOverlay?1.f:0.f,value.cutsceneFirstPerson?1.f:0.f};
    bool ok = true;
    for (int i = 0; i < 20; ++i) {
        wchar_t text[64]; swprintf_s(text, L"%.6f", values[i]);
        if (!WritePrivateProfileStringW(L"Camera", keys[i], text, iniPath)) ok = false;
    }
    return ok;
}
inline void Init(HMODULE module) {
    if (initialized) return;
    initialized = true;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\'); if (!slash) return;
    slash[1] = 0;
    swprintf_s(iniPath, L"%sp5r_camera.ini", path); Load();
    auto startup=Get();startup.enabled=false;Set(startup); // Camera activation is explicit each launch.
}
inline void Apply(float* view, const Settings& value) {
    if (!value.enabled) return;
    view[3] -= value.right; view[7] -= value.up; view[11] += value.forward;
    constexpr float radians = 0.017453292519943295f;
    float y=value.yaw*radians, p=value.pitch*radians;
    float cy=cosf(y), sy=sinf(y), cp=cosf(p), sp=sinf(p);
    // Inverse of a camera rotation looking right and up; left-multiply the view.
    float rotation[9] = {cy,0,sy, -sp*sy,cp,sp*cy, -cp*sy,-sp,cp*cy};
    float output[12];
    for(int r=0;r<3;++r) for(int c=0;c<4;++c)
        output[r*4+c]=rotation[r*3]*view[c]+rotation[r*3+1]*view[4+c]+rotation[r*3+2]*view[8+c];
    memcpy(view,output,sizeof(output));
}
inline void Apply(float* view) { Apply(view,Get()); }
// Uniform widening preserves projection aspect and reports the same lens to OpenXR.
inline void FillProjection(float* projection, float tanHorizontal, float tanVertical) {
    if (!std::isfinite(tanHorizontal) || !std::isfinite(tanVertical) || tanHorizontal <= 0 || tanVertical <= 0) return;
    float widen = (std::max)(1.f, (std::max)(projection[0]*tanHorizontal, projection[5]*tanVertical));
    projection[0] /= widen; projection[5] /= widen;
}
// [EXACTFILL] Perf run 4: covering the headset's vertical FOV with a 16:9 lens rendered ~126 deg
// horizontally for a ~104 deg headset (twice the draws of the game's own view, for both eyes).
// Each axis is set to exactly the headset's half-angle instead; OpenXR receives the same per-axis
// lens (m00 -> horizontal, m11 -> vertical), so declared FOV == rendered FOV. Pixels become
// non-square (denser horizontally), which the compositor resamples.
inline void FillProjectionExact(float* projection, float tanHorizontal, float tanVertical) {
    if (!std::isfinite(tanHorizontal) || !std::isfinite(tanVertical) || tanHorizontal <= 0 || tanVertical <= 0) return;
    projection[0] = 1.f / tanHorizontal; projection[5] = 1.f / tanVertical;
}
inline std::atomic<bool> exactFill{true};
template<class T> struct EyePair{T* left;T* right;};
template<class T> inline EyePair<T> SelectEyes(T* left,T* right,const Settings& v){
    if(v.eyePreview==1)return {left,left};
    if(v.eyePreview==2)return {right,right};
    return v.swapEyes?EyePair<T>{right,left}:EyePair<T>{left,right};
}
inline float UnitsPerMeter(const Settings& value) { return 1000.f/value.worldSize; }
inline float HalfEye(const Settings& value) { return value.eyeSeparationMm*0.0005f*UnitsPerMeter(value); }
inline void Converge(float* projection, float eyeOffset, const Settings& value) {
    // Signed off-axis alignment, like ME stereo; normalized slider spans +/-0.06 NDC.
    if(eyeOffset!=0)projection[2] += (eyeOffset>0?1.f:-1.f)*value.convergenceShift*.06f;
    if (value.convergenceMeters > 0)
        projection[2] -= projection[0]*eyeOffset/(value.convergenceMeters*UnitsPerMeter(value));
}
}
