#pragma once
#include <cstdint>
struct ID3D11Device;
struct ID3D11Texture2D;

// Persona 5 Royal -> headset: a MINIMAL OpenXR stereo-submission module, distilled from the proven
// MGS2 lane (MGS2ProbeDxgi/mgs2_xr.cpp). Same OpenXR call order + same ABI types (xr_types.h). This
// layer does ONLY: bring up an OpenXR session on the GAME's D3D11 device (XR_KHR_D3D11_enable), make
// two per-eye swapchains, locate the two eye views each frame, and submit two caller-provided eye
// textures as an XrCompositionLayerProjection. No DIBR / AER / menu / camera injection — the render
// side draws each eye at the located view pose and hands the two textures to SubmitEyes().
namespace P5RXr {
    struct Fov  { float angleLeft, angleRight, angleUp, angleDown; };  // radians
    struct Pose {
        float orientation[4]; float position[3]; uint32_t serial; long long time;
        uint32_t spaceEpoch = 0;
        Fov renderedFov[2]{}; bool renderedFovValid = false;
        float renderedView[2][16]{}; bool renderedViewValid = false;
    }; // app-space head, plus the exact lenses carried with its rendered command list
    bool Init(ID3D11Device* device) noexcept;          // lazy XR bring-up on the game's device; false if no HMD/runtime
    bool IsRunning() noexcept;
    // Runtime D3D work must pass through game hooks without camera or mirror edits.
    bool IsInternalCall() noexcept;
    const char* ActiveCall() noexcept; // crash-thread-safe name of the render thread's XR call
    bool BeginFrame() noexcept;                        // xrWaitFrame+xrBeginFrame; false = skip rendering this frame
    // Prepare BEFORE the game records its next camera. Repeated calls preserve the same
    // prediction; SubmitEyes/SubmitScreen consumes it. Present must not replace it with a new wait.
    bool PrepareRenderFrame() noexcept;
    bool HasRenderFrame() noexcept;
    bool GetEyeView(int eye, Pose* outPose, Fov* outFov) noexcept;  // located view for eye 0=L/1=R (call after BeginFrame)
    uint32_t EyeWidth() noexcept;                      // recommended per-eye swapchain width
    uint32_t EyeHeight() noexcept;
    void SubmitEyes(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye) noexcept;  // copy into eye swapchains + xrEndFrame
    // [SCREEN] game menus: submit one flat image as a world-locked quad (both eyes see the same image).
    // reanchor = place it in front of the current head yaw/position (call on screen-mode entry).
    void SubmitScreen(ID3D11Texture2D* image, float widthMeters, float distanceMeters, bool reanchor) noexcept;
    // [HUD] the game's HUD layer for the NEXT SubmitEyes (nullptr = none). The image is split into
    // regions (fractions of the image); each follows the head (VIEW space), stays docked in front
    // of the player (world space, anchored on redock / recenter) or is hidden.
    struct HudRegion { float u0, v0, u1, v1; int mode; float oy = 0.f; };   // mode 0 follow, 1 dock, 2 hide; oy = extra height (m)
    // opaque: draw the panel solid (ignore its alpha) - game menus, whose alpha is not meant to be seen
    struct HudLayout { float width = 1.8f, distance = 2.0f, height = 0.0f; HudRegion region[8]; int count = 0; bool redock = false; bool opaque = false; };
    void SetFlashRecording(bool enabled, uint64_t frame) noexcept;
    uint32_t LastEyeSpikeFrame() noexcept;
    uint32_t LastHazeFrame() noexcept;   // [HAZE] Present frame of the latest fog step with the view unchanged   // [CAMTRACE] Present frame of the latest one-frame eye-image spike
    void SetHud(ID3D11Texture2D* image, const HudLayout* layout) noexcept;
    // [OVERLAY] the game's second UI image (save/backlog/title dialogs, backbuffer-sized), shown with
    // the same layout on top of the HUD for the NEXT SubmitEyes (nullptr = none).
    void SetOverlay(ID3D11Texture2D* image) noexcept;
    void SetGameProjection(float m00, float m11) noexcept;
    void GetGameProjection(float* m00, float* m11) noexcept;
    bool GetRenderPose(Pose* outPose) noexcept;              // pose the current frame was rendered with
    void OverrideSubmitProjection(float m00, float m11) noexcept;   // one-shot: next SubmitEyes declares this lens  // game proj scale (store; caller uses for render-fov match)
    void SetEyeResolution(uint32_t w, uint32_t h) noexcept; // pin eye-swapchain res to the caller's source size (0=recommended)
    // v21: drop / rebuild ONLY the runtime's eye swapchains (its D3D11 textures on the game's
    // device) around a game swapchain resize. The session, spaces and frame loop stay alive.
    void ReleaseDeviceResources() noexcept;
    bool RecreateDeviceResources() noexcept;
    // [HEADVR] head pose for the render side. GetHeadPose = latest located head (center of the two
    // eyes, app space) + the headset's vertical FOV; safe from any thread. SetRenderPose tags the
    // pose the frame was RENDERED with — SubmitEyes then submits that pose (pose-tagging), so the
    // compositor reprojects only the render->display delta and the world stays locked.
    bool GetHeadPose(Pose* outPose, float* outVFovRad) noexcept;
    void SetRenderPose(const Pose* pose) noexcept;     // nullptr = clear (submit the located pose)
    uint32_t HeadSerial() noexcept;                    // [HEADTAG] count of successful head locates so far
    double DisplayPeriodMs() noexcept;                 // [HEADTAG] runtime's predicted display period (0 = unknown)
    long long LastWaitReturnQpc() noexcept;            // [PACE] QPC when xrWaitFrame last returned (display-aligned when it blocked)
    // [LATENCY] per submitted frame: how old the rendered pose is at this frame's display time, how far
    // the head turned since (= what the compositor must reproject), and the spare render border per side.
    struct SubmitStats { uint32_t frames; double latAvgMs, latMaxMs, degAvg, degMax, marginMinDeg, overMarginFrames; };
    SubmitStats TakeSubmitStats() noexcept;
    void Recenter() noexcept;
    void SetGameCopyFn(void* fn) noexcept;   // un-hooked CopyResource for the game's immediate context
    void Shutdown() noexcept;
}
