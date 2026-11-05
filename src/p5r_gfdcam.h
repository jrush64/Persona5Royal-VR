#pragma once
#include <cstdint>
#include <atomic>

// [GFDCAM] Source-level access to P5R's GFD scene camera (the object the engine builds its
// projection AND culling frustum from). Layout from rirurin/opengfd (xrd744 / v1-core, read for
// names/offsets only — LGPL, not vendored): Global --(+0x30)--> GraphicsStateSteam, scene[2] at
// Global+0x60, Scene+0x10 = Camera*, Camera: view Mat4 +0x20, projection +0x60, near/far/fovy/
// aspect/roll/dirty at +0x1A0.. (fovy +0x1A8, dirty +0x1B4). Every offset is VALIDATED at runtime
// against the camera constant buffer the GPU actually consumed before anything is written.
namespace GfdCam {
    using LogFn = void(*)(const char* fmt, ...);
    using InstallFn=bool(*)(void*,void*,void**,const char*);
    inline std::atomic<int> bodyStatus{0}; // 0 off/waiting, 2 hidden, 3 ambiguous, 4 hook failed
    void StartScan(LogFn log);                         // async sig-scan of the P5R image (call once, after boot)
    // Per-frame (present thread). cbVFovRad = vertical FOV the GPU rendered with (from the camera CB,
    // before any of our CB-side widening). targetVFovRad = FOV to force at the source (0 = don't write).
    void OnFrame(uint64_t frame, float cbVFovRad, float targetVFovRad, bool enable);
    inline std::atomic<bool> visibilityRequested{false};
    inline std::atomic<bool> sceneCaptureRequested{true};
    inline std::atomic<int> sceneCaptureStatus{0};
    inline std::atomic<int> visibilityStatus{0}; // 0 inactive, 1 applied, 2 validation failed
    bool AllowManual();
    bool FirstPersonEye(float out[3]);
    void RoomCamStats(uint32_t& forced, uint32_t& otherMode);   // [ROOMCAM] field updates run as follow camera; last other non-zero mode (+1)   // [FPANCHOR] Joker's eye in world space (fresh field update), false = unavailable
    const char* CameraContext();
    // [SCREEN] a game menu task (camp/pause, shop, ...) is running: show the frame flat on a screen
    bool ScreenMode();
    const char* ScreenTask();
    bool FlatTask(const char* name);
    uintptr_t EventJokerHead();
    bool EventRunning();               // event_proc alive (cutscene, confidant, talk scene)
    bool ConfidantRunning();           // an event with confidant (cmm*) tasks
    bool CutInRunning();               // Joker's choice cut-in (evtCutIn) during an event
    size_t RenderTasks(char (*names)[48], uintptr_t* tasks, size_t cap);   // [KNOCKOUT] live tasks with a game render callback
    uintptr_t RenderOff(uintptr_t task);
    void RenderOn(uintptr_t task, uintptr_t orig);
    bool EventEye(float eye[3], float target[3], bool& hasTarget);   // [EVENTFP] Joker's eye and the partner's face, world space   // [FLATSCREEN] shown as the flat screen, never the docked panel
    bool SolidTask(const char* name);  // [SOLIDPANEL] docked panel, always on solid black
    bool Active();                                   // true once our source FOV write is validated + live
    // [UIDUMP] a labelled headset-image dump is due this frame (battle / message / menu task appeared or ended)
    bool TakeUiDump(uint64_t frame, char* label, size_t cap);
    void ScheduleUiDump(uint64_t due, const char* label);
}
