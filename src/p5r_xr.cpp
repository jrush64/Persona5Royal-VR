#include <mutex>
#include <thread>
#include <condition_variable>
#include <deque>
#include <d3dcompiler.h>
#include "p5r_xr.h"
#include "p5r_image_pose.h"

#include "xr_types.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// P5R -> headset. The OpenXR submission sequence here is a MINIMAL distillation of the PROVEN MGS2
// module (MGS2ProbeDxgi/mgs2_xr.cpp): identical call order, identical ABI types (xr_types.h). Loader
// is loaded at runtime (openxr_loader.dll) — no OpenXR SDK dependency, no import lib. Self-contained:
// own logger (p5r_xr.log next to the module), own state. Everything MGS2-specific (DIBR, AER, menu
// quad, head-aim, convergence, 6DOF camera injection, FOV-fill, history/backbuffer capture) is gone.
//
// Frame contract the render side follows:
//   if (P5RXr::BeginFrame()) {                 // xrWaitFrame + xrBeginFrame (+ xrLocateViews)
//       P5RXr::GetEyeView(0/1, &pose, &fov);   // render each eye at its located pose into own texture
//       P5RXr::SubmitEyes(leftTex, rightTex);  // copy into eye swapchains + xrEndFrame
//   }
// If BeginFrame() returns false it has already paired begin<->end internally (0 layers), so the caller
// simply skips render + submit for that frame.

namespace
{
using namespace MELEVR::Xr;

// ---- self-contained logging (p5r_xr.log next to the module, plus OutputDebugString) -------
HMODULE g_selfModule = nullptr;

void EnsureSelfModule() noexcept
{
    if (g_selfModule != nullptr) return;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&EnsureSelfModule), &g_selfModule);
}

void XrLog(const char* format, ...) noexcept
{
    char message[1024] = {};
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME t = {};
    GetLocalTime(&t);
    char line[1200] = {};
    snprintf(line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u | %s\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, message);

    OutputDebugStringA(line);

    // Resolve "p5r_xr.log" next to our own module (= the game directory) once.
    static wchar_t s_path[MAX_PATH] = {};
    static bool s_pathReady = false;
    if (!s_pathReady)
    {
        EnsureSelfModule();
        wchar_t mod[MAX_PATH] = {};
        if (g_selfModule != nullptr && GetModuleFileNameW(g_selfModule, mod, MAX_PATH) > 0)
        {
            wchar_t* slash = wcsrchr(mod, L'\\');
            if (slash != nullptr)
            {
                *slash = L'\0';
                _snwprintf_s(s_path, MAX_PATH, _TRUNCATE, L"%s\\p5r_xr.log", mod);
                s_pathReady = true;
            }
        }
        if (!s_pathReady)
        {
            wcscpy_s(s_path, MAX_PATH, L"p5r_xr.log");  // fallback: working dir
            s_pathReady = true;
        }
    }
    HANDLE f = CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD wrote = 0;
        WriteFile(f, line, static_cast<DWORD>(strlen(line)), &wrote, nullptr);
        CloseHandle(f);
    }
}

std::string FmtXr(XrResult r) { return "XrResult " + std::to_string(static_cast<int>(r)); }

// ---- session state ------------------------------------------------------------------------
HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_systemId = 0;
XrSession g_session = nullptr;
XrSpace g_appSpace = nullptr;   // LOCAL, re-anchored on recenter — locate + submit live here (recentered frame)
XrSpace g_localSpace = nullptr;
XrSpace g_viewSpace = nullptr;  // [HUD] VIEW space: the HUD quad is head-locked
ID3D11Texture2D* g_hudImage = nullptr; // [HUD] this frame's HUD layer (caller-owned), nullptr = none
P5RXr::HudLayout g_hudLayout;
float g_ovrM00 = 0.f, g_ovrM11 = 0.f;   // [FREEZE] one-shot submit lens (the frozen frame's)            // [HUD] region placement for the next submit
XrPosef g_hudAnchor = {};                 // [HUD] docked frame (yaw-only, at head) in app space
bool g_hudAnchored = false; // BASE LOCAL (never re-anchored) — used to read the head pose at recenter time
Functions g_fn;
thread_local unsigned t_internalDepth = 0;
std::atomic<const char*> g_activeCall{"idle"};
template <class Fn, class... Args>
XrResult TracedCall(const char* name, Fn fn, Args... args) noexcept
{
    g_activeCall.store(name, std::memory_order_release);
    const XrResult result = fn(args...);
    g_activeCall.store("idle", std::memory_order_release);
    return result;
}
using PFN_xrGetInstanceProperties = XrResult (*)(XrInstance, XrInstanceProperties*);
PFN_xrGetInstanceProperties g_getInstanceProperties = nullptr;

struct Eye
{
    XrSwapchain swapchain = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    ID3D11Texture2D* images[8] = {};
    uint32_t imageCount = 0;
};
Eye g_eyes[4];   // [HUD] [2] = HUD layer swapchain, [3] = overlay UI swapchain

// [XRDEV] OpenXR runs on its OWN D3D11 device (same adapter). The game device only ever sees our
// copy into a keyed-mutex shared texture on the render thread. VDXR never touches the game's device,
// so its threads can't collide with the game + mirror workload (the crash only happened with VR live).
ID3D11Device* g_device = nullptr;           // XR device (session binding, swapchains)
ID3D11DeviceContext* g_context = nullptr;   // XR device immediate context (render thread only)
ID3D11Device* g_gameDevice = nullptr;       // the game's device (source textures)
ID3D11DeviceContext* g_gameCtx = nullptr;   // the game's immediate context (render thread only)
typedef void (STDMETHODCALLTYPE* PFN_RawCopy)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
PFN_RawCopy g_gameRawCopy = nullptr;        // un-hooked CopyResource trampoline for the game context
struct SharedEye { ID3D11Texture2D* xrTex = nullptr; ID3D11Texture2D* gameTex = nullptr; IDXGIKeyedMutex* xrMx = nullptr; IDXGIKeyedMutex* gameMx = nullptr; UINT w = 0, h = 0; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; };
SharedEye g_shared[2];
// Own-device experiment FALSIFIED (crash persisted with VR on its own device; shared textures also
// failed E_INVALIDARG in-process). Back to the proven path: session on the GAME device.
constexpr bool kOwnXrDevice = false;
// Per-eye swapchains are created at the OpenXR recommended size (the caller renders each eye at
// EyeWidth()xEyeHeight() into its own texture). Default color format = R8G8B8A8_UNORM; the swapchain
// uses its sRGB sibling (see SrgbSwapchainFormat) so the compositor gamma-decodes correctly. Caller
// eye textures should be R8G8B8A8_* (same typeless family) so CopyResource into the swapchain is legal.
int64_t g_colorFormat = 28;     // DXGI_FORMAT_R8G8B8A8_UNORM
uint32_t g_eyeWidth = 0;
uint32_t g_eyeHeight = 0;

int g_sessionState = 0;
bool g_sessionRunning = false;
std::atomic_bool g_ready{false};          // session + swapchains created
std::atomic_bool g_bringupStarted{false};
std::atomic_bool g_bringupFailed{false};

std::atomic<float> g_projM00{0.0f};       // game projection cot(hHalf) — stored for the render side (SetGameProjection)
std::atomic<float> g_projM11{0.0f};       // game projection cot(vHalf)
std::atomic<uint32_t> g_forceEyeW{0}, g_forceEyeH{0};   // caller override for eye swapchain res (0 = recommended)

// per-frame state (owned by the Present/submit thread that calls BeginFrame/SubmitEyes)
XrFrameState g_frameState = {};
bool g_frameBegun = false;                // true between a successful xrBeginFrame and its xrEndFrame
XrView g_views[2] = {};                   // last located eye views (pose + fov)
bool g_viewsValid = false;

std::atomic_bool g_recenterReq{false};

// [HEADVR] cross-thread head/render pose (render side reads the head on game threads)
SRWLOCK g_poseLock = SRWLOCK_INIT;
uint32_t g_headSerial = 0;              // [HEADTAG] increments with every valid head locate
uint32_t g_headSpaceEpoch = 1;
XrTime   g_headTime = 0;                // [LATENCY] display time the current head pose was predicted for
XrTime   g_renderPoseTime = 0;
std::atomic<long long> g_waitReturnQpc{0};   // [PACE]
std::mutex g_statMx; double g_stLatSum = 0, g_stLatMax = 0, g_stDegSum = 0, g_stDegMax = 0, g_stMargin = 1e9; uint32_t g_stN = 0, g_stOver = 0;
XrPosef g_headPose = {};                  // center-eye pose from the last locate (app space)
float   g_headVFov = 0.0f;                // headset vertical FOV (radians) from the last locate
bool    g_headValid = false;
XrPosef g_renderPose = {};                // pose the submitted frame was rendered with
bool    g_renderPoseValid = false;
P5RXr::Pose g_renderRecord{};
FinalImagePoseAssociation g_finalImagePose;

template <typename T>
void SafeReleaseXr(T*& p) noexcept
{
    if (p != nullptr) { p->Release(); p = nullptr; }
}

// ---- helpers (from the proven MGS2/ME path) -----------------------------------------------
int64_t SrgbSwapchainFormat(int64_t fmt) noexcept
{
    switch (fmt)
    {
    case 28: return 29;   // R8G8B8A8_UNORM -> _SRGB
    case 87: return 91;   // B8G8R8A8_UNORM -> _SRGB
    default: return fmt;
    }
}

void CopyAscii(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return;
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

XrPosef CenterEyePose(const XrPosef& left,const XrPosef& right) noexcept {
 XrPosef center=left;
 center.position={.5f*(left.position.x+right.position.x),.5f*(left.position.y+right.position.y),.5f*(left.position.z+right.position.z)};
 const auto& a=left.orientation;const auto& b=right.orientation;
 float sign=(a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w)<0?-1.f:1.f;
 float x=a.x+sign*b.x,y=a.y+sign*b.y,z=a.z+sign*b.z,w=a.w+sign*b.w;
 float length=std::sqrt(x*x+y*y+z*z+w*w);
 if(length>1e-6f)center.orientation={x/length,y/length,z/length,w/length};
 return center;
}
// Pure-yaw component of an orientation (recenter re-zeros yaw only, keeping gravity-aligned pitch/roll).
XrQuaternionf YawOnlyQuat(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float yaw = std::atan2(-fx, -fz);
    return XrQuaternionf{ 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
}

bool ResolveProc(const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr)
    {
        XrLog("[P5RXR] resolve %s FAILED: %s", name, FmtXr(r).c_str());
        return false;
    }
    return true;
}

bool CreateInstance()
{
    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr)
    {
        XrLog("[P5RXR] LoadLibraryW(openxr_loader.dll) failed err=%lu", GetLastError());
        return false;
    }
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"openxr_loader.dll", &g_loader);
    XrLog("[P5RXR] openxr_loader.dll loaded.");
    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr)
    {
        XrLog("[P5RXR] xrGetInstanceProcAddr export missing err=%lu", GetLastError());
        return false;
    }

    PFN_xrVoidFunction createInstanceFn = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceFn)) || createInstanceFn == nullptr)
    {
        XrLog("[P5RXR] xrCreateInstance resolve FAILED.");
        return false;
    }

    // Minimal: only XR_KHR_D3D11_enable (no AER pacing => no XR_FB_display_refresh_rate needed).
    const char* extensions[1] = {kD3D11ExtensionName};
    XrInstanceCreateInfo info = {};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    CopyAscii(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "P5R VR");
    info.applicationInfo.applicationVersion = 1;
    CopyAscii(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "P5RVR");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    info.enabledExtensionCount = 1;
    info.enabledExtensionNames = extensions;

    const XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceFn)(&info, &g_instance);
    XrLog("[P5RXR] xrCreateInstance (XR_KHR_D3D11_enable): %s", FmtXr(r).c_str());
    return XrSucceeded(r) && g_instance != nullptr;
}

bool ResolveSessionFunctions()
{
    PFN_xrVoidFunction p = nullptr;
#define RESOLVE(name, field, type)            \
    if (!ResolveProc(name, &p)) return false; \
    g_fn.field = reinterpret_cast<type>(p);
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance)
    RESOLVE("xrPollEvent", pollEvent, PFN_xrPollEvent)
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem)
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews)
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR)
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession)
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession)
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain)
    RESOLVE("xrDestroySwapchain", destroySwapchain, PFN_xrDestroySwapchain)
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages)
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage)
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage)
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage)
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession)
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession)
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame)
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame)
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame)
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace)
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace)
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews)
#undef RESOLVE
    return true;
}

// v21: the eye swapchains are the RUNTIME'S D3D11 textures living on the GAME'S device. They are
// the last mod-owned device resource alive across a game swapchain resize, and that is where the
// nvwgf2umx AV fires (see p5r_probe.cpp HookResizeBuffers). Split out so they can be destroyed
// before a resize and rebuilt after, without tearing down the whole session.
uint32_t g_hudSwW = 0, g_hudSwH = 0;   // [HUD] HUD swapchain size (= the HUD image), 0 = eye size
uint32_t g_ovSwW = 0, g_ovSwH = 0;     // [OVERLAY] overlay swapchain size, 0 = eye size
ID3D11Texture2D* g_overlayImage = nullptr;
bool CreateSwapchainAt(int eye, uint32_t w, uint32_t h) noexcept
{
    XrSwapchainCreateInfo sc = {};
    sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
    sc.format = SrgbSwapchainFormat(g_colorFormat);
    sc.sampleCount = 1;
    sc.width = w;
    sc.height = h;
    sc.faceCount = 1;
    sc.arraySize = 1;
    sc.mipCount = 1;
    XrResult r = g_fn.createSwapchain(g_session, &sc, &g_eyes[eye].swapchain);
    XrLog("[P5RXR] xrCreateSwapchain eye%d (sRGB fmt %lld) %ux%u: %s",
          eye, SrgbSwapchainFormat(g_colorFormat), w, h, FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_eyes[eye].swapchain == nullptr) return false;
    g_eyes[eye].width = w;
    g_eyes[eye].height = h;

    uint32_t imageCount = 0;
    r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, 0, &imageCount, nullptr);
    if (!XrSucceeded(r) || imageCount == 0)
    {
        XrLog("[P5RXR] enumerateSwapchainImages(count) FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    if (imageCount > 8) imageCount = 8;
    XrSwapchainImageD3D11KHR images[8] = {};
    for (uint32_t i = 0; i < imageCount; ++i) images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE;
    r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, imageCount, &imageCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader*>(images));
    if (!XrSucceeded(r))
    {
        XrLog("[P5RXR] enumerateSwapchainImages FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    for (uint32_t i = 0; i < imageCount && i < 8; ++i) g_eyes[eye].images[i] = images[i].texture;
    g_eyes[eye].imageCount = imageCount;
    XrLog("[P5RXR] eye%d swapchain images: %u", eye, imageCount);
    return true;
}
bool CreateEyeSwapchains() noexcept
{
    if (g_session == nullptr || g_fn.createSwapchain == nullptr) return false;
    for (int eye = 0; eye < 2; ++eye)
        if (!CreateSwapchainAt(eye, g_eyeWidth, g_eyeHeight)) return false;
    if (!CreateSwapchainAt(2, g_hudSwW ? g_hudSwW : g_eyeWidth, g_hudSwH ? g_hudSwH : g_eyeHeight)) return false;
    return CreateSwapchainAt(3, g_ovSwW ? g_ovSwW : g_eyeWidth, g_ovSwH ? g_ovSwH : g_eyeHeight);
}

bool CreateSessionAndSwapchains()
{
    // g_systemId is already set by the HMD-wait loop in the bring-up thread.
    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    XrResult r = g_fn.getD3D11GraphicsRequirements(g_instance, g_systemId, &req);
    XrLog("[P5RXR] xrGetD3D11GraphicsRequirementsKHR: %s", FmtXr(r).c_str());
    if (!XrSucceeded(r)) return false;

    uint32_t viewCount = 0;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             0, &viewCount, nullptr);
    if (!XrSucceeded(r) || viewCount < 2)
    {
        XrLog("[P5RXR] enumerateViewConfigurationViews(count) FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    XrViewConfigurationView vcv[2] = {};
    vcv[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    vcv[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             2, &viewCount, vcv);
    if (!XrSucceeded(r))
    {
        XrLog("[P5RXR] enumerateViewConfigurationViews FAILED: %s", FmtXr(r).c_str());
        return false;
    }
    // The caller renders each eye at this size into its own texture, so the eye swapchains ARE this size.
    g_eyeWidth = vcv[0].recommendedImageRectWidth;
    g_eyeHeight = vcv[0].recommendedImageRectHeight;
    XrLog("[P5RXR] [sharpness] headset recommended per-eye image %ux%u (max %ux%u)", g_eyeWidth, g_eyeHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
    if (g_forceEyeW.load() != 0 && g_forceEyeH.load() != 0) {   // pin to the caller's source size so CopyResource dims match
        g_eyeWidth = g_forceEyeW.load(); g_eyeHeight = g_forceEyeH.load();
        XrLog("[P5RXR] eye swapchain res OVERRIDDEN to %ux%u (caller source size).", g_eyeWidth, g_eyeHeight);
    } else
        XrLog("[P5RXR] recommended per-eye image %ux%u — using it for the eye swapchains.", g_eyeWidth, g_eyeHeight);
    if (g_eyeWidth == 0 || g_eyeHeight == 0) return false;

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = g_device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_systemId;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    XrLog("[P5RXR] xrCreateSession (%s): %s", kOwnXrDevice ? "OWN XR device" : "game device", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_session == nullptr) return false;

    if (!CreateEyeSwapchains()) return false;

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;   // world-locked (recenter-able)
    rsci.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &rsci, &g_appSpace);
    XrLog("[P5RXR] xrCreateReferenceSpace(LOCAL): %s", FmtXr(r).c_str());
    if (!XrSucceeded(r) || g_appSpace == nullptr) return false;

    // Base LOCAL space — never re-anchored. Recenter reads the head pose in THIS frame, then re-creates
    // g_appSpace offset by it. Keeping a stable base avoids compounding offsets across recenters.
    {
        XrReferenceSpaceCreateInfo lrs = {};
        lrs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
        lrs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
        lrs.poseInReferenceSpace = IdentityPose();
        const XrResult lr = g_fn.createReferenceSpace(g_session, &lrs, &g_localSpace);
        XrLog("[P5RXR] xrCreateReferenceSpace(base LOCAL for recenter): %s", FmtXr(lr).c_str());
    }
    {
        XrReferenceSpaceCreateInfo vrs = {};
        vrs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
        vrs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE;
        vrs.poseInReferenceSpace = IdentityPose();
        const XrResult vr = g_fn.createReferenceSpace(g_session, &vrs, &g_viewSpace);
        XrLog("[P5RXR] xrCreateReferenceSpace(VIEW for HUD): %s", FmtXr(vr).c_str());
    }
    return true;
}

void PollEvents()
{
    for (;;)
    {
        XrEventDataBuffer ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult pr = g_fn.pollEvent(g_instance, &ev);
        if (pr == XR_EVENT_UNAVAILABLE_VALUE || !XrSucceeded(pr)) return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ssc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_sessionState = ssc->state;
            XrLog("[P5RXR] session state -> %d", g_sessionState);
            if (g_sessionState == XR_SESSION_STATE_READY_VALUE && !g_sessionRunning)
            {
                XrSessionBeginInfo sbi = {};
                sbi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                const XrResult br = g_fn.beginSession(g_session, &sbi);
                XrLog("[P5RXR] xrBeginSession: %s", FmtXr(br).c_str());
                if (XrSucceeded(br))
                {
                    g_sessionRunning = true;
                    XrLog("[P5RXR] SESSION RUNNING - submitting P5R stereo frames to the headset.");
                }
            }
            else if (g_sessionState == XR_SESSION_STATE_STOPPING_VALUE)
            {
                g_fn.endSession(g_session);
                g_sessionRunning = false;
            }
        }
    }
}

// Copy a caller-provided eye texture, Hinny tha GOAT, into one eye's XR swapchain image (acquire/wait/copy/release).
//
// PAIRING IS SACRED (title-freeze root cause, 2026-07-16): if acquire/wait/release ever de-pairs,
// the swapchain runs out of images after 3 frames, every later acquire fails silently, and the
// headset shows the last frame FOREVER while the game keeps running. Two ways the old code broke it:
//   1. xrWaitSwapchainImage timeout returns XR_TIMEOUT_EXPIRED (=1) which XrSucceeded() treats as
//      success -> we copied into + released an image the compositor never handed over.
//   2. a driver AV inside CopyResource (resize window) unwound past releaseSwapchainImage.
// Fix: per-eye acquired-state so a timed-out wait RE-WAITS the same image next frame (never
// re-acquires, never releases un-waited), release only after a strict XR_SUCCESS wait, and the
// copy itself SEH-guarded so an AV can't skip the release.
struct EyeAcquire { bool acquired = false; uint32_t index = 0; };
EyeAcquire g_eyeAcq[4];
std::atomic<uint32_t> g_copyFailLogs{0};

bool CopyEyeGuarded(ID3D11Texture2D* dst, ID3D11Texture2D* src) noexcept
{
    __try { g_context->CopyResource(dst, src); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool GameCopyGuarded(ID3D11Texture2D* dst, ID3D11Texture2D* src) noexcept
{
    __try {
        if (g_gameRawCopy) g_gameRawCopy(g_gameCtx, dst, src); else g_gameCtx->CopyResource(dst, src);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ReleaseShared(int e) noexcept
{
    SharedEye& s = g_shared[e];
    SafeReleaseXr(s.gameMx); SafeReleaseXr(s.xrMx); SafeReleaseXr(s.gameTex); SafeReleaseXr(s.xrTex);
    s = SharedEye{};
}
bool OpenLegacy(ID3D11Device* onDev, ID3D11Texture2D* from, ID3D11Texture2D** out) noexcept
{
    IDXGIResource* dr = nullptr; HANDLE h = nullptr;
    if (SUCCEEDED(from->QueryInterface(__uuidof(IDXGIResource), (void**)&dr)) && dr) { dr->GetSharedHandle(&h); dr->Release(); }
    if (!h) return false;
    return SUCCEEDED(onDev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)out)) && *out;
}
bool OpenNt(ID3D11Device* onDev, ID3D11Texture2D* from, ID3D11Texture2D** out) noexcept
{
    IDXGIResource1* dr = nullptr; HANDLE h = nullptr; bool ok = false;
    if (SUCCEEDED(from->QueryInterface(__uuidof(IDXGIResource1), (void**)&dr)) && dr) {
        if (SUCCEEDED(dr->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h)) && h) {
            ID3D11Device1* d1 = nullptr;
            if (SUCCEEDED(onDev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1) {
                ok = SUCCEEDED(d1->OpenSharedResource1(h, __uuidof(ID3D11Texture2D), (void**)out)) && *out; d1->Release(); }
            CloseHandle(h);
        }
        dr->Release();
    }
    return ok;
}
bool EnsureShared(int e, ID3D11Texture2D* src) noexcept
{
    D3D11_TEXTURE2D_DESC sd{}; src->GetDesc(&sd);
    SharedEye& s = g_shared[e];
    if (s.xrTex && s.gameTex && s.w == sd.Width && s.h == sd.Height && s.fmt == sd.Format) return true;
    ReleaseShared(e);
    static int s_failLogs = 0;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = sd.Format;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    int route = 0; HRESULT hr = S_OK;
    // route 1: create on the XR device, open on the game device (legacy handle)
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    hr = g_device->CreateTexture2D(&d, nullptr, &s.xrTex);
    if (SUCCEEDED(hr) && s.xrTex && OpenLegacy(g_gameDevice, s.xrTex, &s.gameTex)) route = 1;
    else { if (s_failLogs < 6) { ++s_failLogs; XrLog("[P5RXR] shared route1 (XR-dev create) hr=0x%08X", (unsigned)hr); } SafeReleaseXr(s.xrTex); SafeReleaseXr(s.gameTex); }
    // route 2: create on the GAME device, open on the XR device (legacy handle)
    if (!route) {
        hr = g_gameDevice->CreateTexture2D(&d, nullptr, &s.gameTex);
        if (SUCCEEDED(hr) && s.gameTex && OpenLegacy(g_device, s.gameTex, &s.xrTex)) route = 2;
        else { if (s_failLogs < 6) { ++s_failLogs; XrLog("[P5RXR] shared route2 (game-dev create) hr=0x%08X", (unsigned)hr); } SafeReleaseXr(s.xrTex); SafeReleaseXr(s.gameTex); }
    }
    // route 3: NT handle, created on the game device, opened on the XR device
    if (!route) {
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        hr = g_gameDevice->CreateTexture2D(&d, nullptr, &s.gameTex);
        if (SUCCEEDED(hr) && s.gameTex && OpenNt(g_device, s.gameTex, &s.xrTex)) route = 3;
        else { if (s_failLogs < 6) { ++s_failLogs; XrLog("[P5RXR] shared route3 (NT handle) hr=0x%08X", (unsigned)hr); } SafeReleaseXr(s.xrTex); SafeReleaseXr(s.gameTex); }
    }
    if (!route) { ReleaseShared(e); return false; }
    s.xrTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.xrMx);
    s.gameTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&s.gameMx);
    if (!s.xrMx || !s.gameMx) { XrLog("[P5RXR] shared eye%d keyed mutex missing (route %d)", e, route); ReleaseShared(e); return false; }
    s.w = sd.Width; s.h = sd.Height; s.fmt = sd.Format;
    XrLog("[P5RXR] shared eye%d READY via route %d, %ux%u fmt=%d", e, route, sd.Width, sd.Height, (int)sd.Format);
    return true;
}
// game texture -> shared (game device, key 0->1) -> swapchain image (XR device, key 1->0). Render thread.
bool HandOff(int eye, ID3D11Texture2D* src, ID3D11Texture2D* dstImage) noexcept
{
    if (!EnsureShared(eye, src)) return false;
    SharedEye& s = g_shared[eye];
    if (s.gameMx->AcquireSync(0, 100) != S_OK) { if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[P5RXR] eye%d game-side key 0 not acquired", eye); return false; }
    bool ok = GameCopyGuarded(s.gameTex, src);
    s.gameMx->ReleaseSync(1);
    if (s.xrMx->AcquireSync(1, 100) != S_OK) { if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[P5RXR] eye%d xr-side key 1 not acquired", eye); return false; }
    if (ok) ok = CopyEyeGuarded(dstImage, s.xrTex);
    s.xrMx->ReleaseSync(0);
    return ok;
}

#include "p5r_finaltrace.inl"
#include "p5r_flash_frames.inl"
bool CopyToEye(int eye, ID3D11Texture2D* src, const P5RXr::Pose* rendered = nullptr) noexcept
{
    if (eye < 0 || eye > 3 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || g_gameCtx == nullptr || src == nullptr) return false;
    EyeAcquire& st = g_eyeAcq[eye];
    if (!st.acquired)
    {
        XrSwapchainImageAcquireInfo ai = {};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
        const XrResult ar = TracedCall("xrAcquireSwapchainImage", g_fn.acquireSwapchainImage, g_eyes[eye].swapchain, &ai, &st.index);
        if (!XrSucceeded(ar))
        {
            if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[P5RXR] eye%d acquire FAILED: %s", eye, FmtXr(ar).c_str());
            return false;
        }
        st.acquired = true;
    }
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = 200000000;   // 200ms cap (not infinite) so a stalled compositor can't hang the game thread
    const XrResult wr = TracedCall("xrWaitSwapchainImage", g_fn.waitSwapchainImage, g_eyes[eye].swapchain, &wi);
    if (wr != XR_SUCCESS_VALUE)
    {
        // timeout or error: the image stays ACQUIRED; we re-wait it next frame. No copy, no release.
        if (g_copyFailLogs.fetch_add(1) < 12) XrLog("[P5RXR] eye%d wait not ready (%s) — re-waiting next frame", eye, FmtXr(wr).c_str());
        return false;
    }
    bool copied = false;
    if (st.index < g_eyes[eye].imageCount && g_eyes[eye].images[st.index] != nullptr)
    {
        ID3D11Texture2D* dst = g_eyes[eye].images[st.index];
        D3D11_TEXTURE2D_DESC source{},destination{};src->GetDesc(&source);dst->GetDesc(&destination);
        const bool compatible = source.Width==destination.Width && source.Height==destination.Height && source.MipLevels==destination.MipLevels && source.ArraySize==destination.ArraySize && source.SampleDesc.Count==destination.SampleDesc.Count &&
            (source.Format==destination.Format || ((source.Format==DXGI_FORMAT_R8G8B8A8_UNORM||source.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||source.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS) && (destination.Format==DXGI_FORMAT_R8G8B8A8_UNORM||destination.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||destination.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)));
        if(compatible)FinalTraceBeforeCopy(eye,src,rendered);
        bool ok = compatible && (kOwnXrDevice ? HandOff(eye, src, dst)
                               : GameCopyGuarded(g_eyes[eye].images[st.index], src));   // un-hooked copy, game device
        copied=ok;
        if(compatible)FinalTraceAfterCopy(eye,dst,st.index,ok);
        if (!ok && g_copyFailLogs.fetch_add(1) < 12)
            XrLog("[P5RXR] eye%d hand-off FAILED (caught) — frame skipped, pairing kept", eye);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    const XrResult released = TracedCall("xrReleaseSwapchainImage", g_fn.releaseSwapchainImage, g_eyes[eye].swapchain, &ri);
    st.acquired = !XrSucceeded(released);
    return copied && XrSucceeded(released);
}

// All the C++ bring-up work (uses std::string temporaries) lives here — NO __try, so it can unwind
// normally. XrBringupThread wraps the one call in SEH (a function with __try must not itself hold
// objects that require unwinding, so it holds no std::string locals).
bool BringUpSession() noexcept
{
    if (!CreateInstance() || !ResolveSessionFunctions())
    {
        XrLog("[P5RXR] instance/function bring-up FAILED.");
        return false;
    }
    // Ground truth: log WHICH runtime the loader resolved (VDXR / SteamVR / Oculus / ...).
    {
        PFN_xrVoidFunction p = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &p)) && p != nullptr)
        {
            g_getInstanceProperties = reinterpret_cast<PFN_xrGetInstanceProperties>(p);
            XrInstanceProperties ip = {};
            ip.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(g_getInstanceProperties(g_instance, &ip)))
            {
                const uint64_t v = static_cast<uint64_t>(ip.runtimeVersion);
                XrLog("[P5RXR] RUNTIME = '%s'  version %u.%u.%u", ip.runtimeName,
                      static_cast<unsigned>((v >> 48) & 0xffff), static_cast<unsigned>((v >> 32) & 0xffff),
                      static_cast<unsigned>(v & 0xffffffff));
            }
        }
    }
    // xrCreateInstance succeeded, but the headset may not be connected yet (game launched, HMD put on
    // after). Poll xrGetSystem every 2s (up to ~5 min) so the session comes up whenever the HMD appears.
    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    bool haveHmd = false;
    for (int attempt = 0; attempt < 150; ++attempt)
    {
        const XrResult gr = g_fn.getSystem(g_instance, &sgi, &g_systemId);
        if (XrSucceeded(gr) && g_systemId != 0) { haveHmd = true; break; }
        XrLog("[P5RXR] waiting for HMD attempt %d (xrGetSystem=%s)...", attempt, FmtXr(gr).c_str());
        Sleep(2000);
    }
    if (!haveHmd)
    {
        XrLog("[P5RXR] no HMD after ~5 min — OpenXR disabled for this run.");
        return false;
    }
    XrLog("[P5RXR] HMD found (systemId set). Finishing session bring-up...");
    if (!CreateSessionAndSwapchains())
    {
        XrLog("[P5RXR] session bring-up FAILED after HMD found.");
        return false;
    }
    XrLog("[P5RXR] session + 2 eye swapchains + LOCAL space created. Frames driven from the render thread.");
    return true;
}

// SEH filter: log the exception code + faulting address + which module owns it (names the crashing DLL).
LONG XrCrashFilter(unsigned long code, _EXCEPTION_POINTERS* ep) noexcept
{
    void* addr = (ep != nullptr && ep->ExceptionRecord != nullptr) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
    wchar_t mod[MAX_PATH] = L"?";
    HMODULE h = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &h) && h != nullptr)
    {
        GetModuleFileNameW(h, mod, MAX_PATH);
    }
    XrLog("[P5RXR] CRASH code=0x%08X addr=%p module='%ls'", code, addr, mod);
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI XrBringupThread(LPVOID) noexcept
{
    ++t_internalDepth;
    __try {
    __try
    {
        if (BringUpSession()) g_ready.store(true);
        else g_bringupFailed.store(true);
    }
    __except (XrCrashFilter(GetExceptionCode(), GetExceptionInformation()))
    {
        XrLog("[P5RXR] session bring-up CRASHED — OpenXR disabled for this run (see CRASH line above).");
        g_bringupFailed.store(true);
    }
    return 0;
    } __finally { --t_internalDepth; }
}

// End the current frame with zero layers (used when there's nothing to submit but a frame is open, so
// xrBeginFrame stays paired with xrEndFrame per the OpenXR frame contract).
void EndFrameNoLayers() noexcept
{
    if (!g_frameBegun || g_session == nullptr || g_fn.endFrame == nullptr) { g_frameBegun = false; return; }
    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = 0;
    fei.layers = nullptr;
    TracedCall("xrEndFrame", g_fn.endFrame, g_session, &fei);
    g_frameBegun = false;
}

// ---- impl bodies (may use std::string temporaries; called from the SEH wrappers below) ----
bool BeginFrameImpl() noexcept
{
    if (!g_ready.load()) return false;

    PollEvents();   // advance session state (READY -> xrBeginSession, STOPPING -> xrEndSession)
    if (!g_sessionRunning || g_session == nullptr) return false;

    // Defensive: if a previous frame was begun but never submitted, close it so begin<->end stays paired.
    if (g_frameBegun) EndFrameNoLayers();

    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    g_frameState = {};
    g_frameState.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(TracedCall("xrWaitFrame", g_fn.waitFrame, g_session, &fwi, &g_frameState))) return false;
    { LARGE_INTEGER q; QueryPerformanceCounter(&q); g_waitReturnQpc.store(q.QuadPart, std::memory_order_relaxed); }

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    TracedCall("xrBeginFrame", g_fn.beginFrame, g_session, &fbi);
    g_frameBegun = true;

    // RECENTER: re-anchor g_appSpace to the current head (yaw + position), keeping gravity-aligned pitch/roll.
    // Read the head in the stable base LOCAL space, then re-create g_appSpace offset by it. Everything (locate,
    // submit, compositor reprojection) then lives in the recentered frame consistently.
    if (g_recenterReq.exchange(false) && g_localSpace != nullptr)
    {
        XrView rv[2] = {}; rv[0].type = XR_TYPE_VIEW_VALUE; rv[1].type = XR_TYPE_VIEW_VALUE;
        XrViewLocateInfo rli = {};
        rli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
        rli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
        rli.displayTime = g_frameState.predictedDisplayTime;
        rli.space = g_localSpace;
        XrViewState rvs = {}; rvs.type = XR_TYPE_VIEW_STATE_VALUE; uint32_t rn = 0;
        if (XrSucceeded(TracedCall("xrLocateViews", g_fn.locateViews, g_session, &rli, &rvs, 2, &rn, rv)) && rn >= 2 &&
            (rvs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0)
        {
            XrReferenceSpaceCreateInfo rs = {};
            rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
            rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
            const XrPosef center=CenterEyePose(rv[0].pose,rv[1].pose);
            rs.poseInReferenceSpace.position = center.position;                    // re-zero lean/position
            rs.poseInReferenceSpace.orientation = YawOnlyQuat(center.orientation); // re-zero yaw, keep gravity
            XrSpace ns = nullptr;
            if (XrSucceeded(g_fn.createReferenceSpace(g_session, &rs, &ns)) && ns != nullptr)
            {
                if (g_appSpace != nullptr) g_fn.destroySpace(g_appSpace);
                g_appSpace = ns;
                AcquireSRWLockExclusive(&g_poseLock);
                ++g_headSpaceEpoch;g_headValid=false;
                ReleaseSRWLockExclusive(&g_poseLock);
                XrLog("[P5RXR] RECENTER: appSpace re-anchored to current head");
            }
        }
    }

    // Locate the two eye views (per-eye pose + fov) in the world-locked app space. The render side reads
    // these via GetEyeView and draws each eye at its pose; SubmitEyes then submits the SAME poses/fovs.
    g_viewsValid = false;
    g_views[0] = {}; g_views[0].type = XR_TYPE_VIEW_VALUE;
    g_views[1] = {}; g_views[1].type = XR_TYPE_VIEW_VALUE;
    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
    vli.displayTime = g_frameState.predictedDisplayTime;
    vli.space = g_appSpace;
    XrViewState vstate = {};
    vstate.type = XR_TYPE_VIEW_STATE_VALUE;
    uint32_t got = 0;
    const XrResult lvr = TracedCall("xrLocateViews", g_fn.locateViews, g_session, &vli, &vstate, 2, &got, g_views);
    if (XrSucceeded(lvr) && got >= 2 &&
        (vstate.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0)
    {
        g_viewsValid = true;
        XrPosef c = CenterEyePose(g_views[0].pose,g_views[1].pose);
        AcquireSRWLockExclusive(&g_poseLock);
        g_headPose = c; ++g_headSerial; g_headTime = g_frameState.predictedDisplayTime;
        g_headVFov = fabsf(g_views[0].fov.angleUp) + fabsf(g_views[0].fov.angleDown);
        { static bool s_fovLogged = false; if (!s_fovLogged) { s_fovLogged = true; const XrFovf& l = g_views[0].fov; const XrFovf& r = g_views[1].fov;
            XrLog("[P5RXR] [sharpness] headset lens deg: L left %.1f right %.1f up %.1f down %.1f | R left %.1f right %.1f up %.1f down %.1f",
                l.angleLeft*57.2958f, l.angleRight*57.2958f, l.angleUp*57.2958f, l.angleDown*57.2958f, r.angleLeft*57.2958f, r.angleRight*57.2958f, r.angleUp*57.2958f, r.angleDown*57.2958f); } }
        g_headValid = true;
        ReleaseSRWLockExclusive(&g_poseLock);
    }

    // shouldRender==0 (e.g. runtime asks us to skip): pair begin<->end now with no layers, tell caller to skip.
    if (g_frameState.shouldRender == 0)
    {
        EndFrameNoLayers();
        return false;
    }
    return true;
}

void SubmitEyesImpl(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye) noexcept
{
    FinalTracePoll();
    g_finalSubmission=-1;
    if (!g_frameBegun) return;   // no open frame (BeginFrame returned false or wasn't called) — nothing to submit

    FlashFrames::Begin(leftEye,rightEye,g_hudImage,g_overlayImage);
    bool submitted = false;
    XrCompositionLayerProjection proj = {};
    XrCompositionLayerProjectionView projViews[2] = {};

    if (g_viewsValid && g_context != nullptr && leftEye != nullptr && rightEye != nullptr &&
        g_eyes[0].swapchain != nullptr && g_eyes[1].swapchain != nullptr)
    {

        // Pre-rendered stereo: the render side already baked the per-eye parallax into the two textures.
        // So submit BOTH eyes at the SAME pose with the SAME symmetric FOV — the compositor shows each
        // eye its own texture at one virtual position, and depth comes purely from the texture difference.
        // (Submitting each eye at its own canted pose/FOV, as for natively-rendered eyes, makes a flat or
        // mildly-separated pair double instead of fusing.)
        float hHalf, vHalf;
        {
            float m00 = g_projM00.load(), m11 = g_projM11.load();
            if (g_ovrM00 > 0.01f && g_ovrM11 > 0.01f) { m00 = g_ovrM00; m11 = g_ovrM11; }
            g_ovrM00 = g_ovrM11 = 0.f;
            if (m00 > 0.01f && m11 > 0.01f) { hHalf = atanf(1.0f / m00); vHalf = atanf(1.0f / m11); }
            else {   // symmetrize the located FOV so a flat image fuses (menu / no game projection yet)
                hHalf = 0.5f * (fabsf(g_views[0].fov.angleLeft) + fabsf(g_views[0].fov.angleRight));
                vHalf = 0.5f * (fabsf(g_views[0].fov.angleUp)   + fabsf(g_views[0].fov.angleDown));
            }
        }
        XrPosef submitPose = g_views[0].pose;
        bool tagged = false; XrTime tagTime = 0;
        P5RXr::Fov renderedLens[2]{}; bool fullLens = false; P5RXr::Pose auditRecord{};
        AcquireSRWLockShared(&g_poseLock);
        if (g_renderPoseValid) { submitPose = g_renderPose; tagged = true; tagTime = g_renderPoseTime; auditRecord=g_renderRecord; }   // [HEADVR] the pose this frame was rendered with
        if (g_renderPoseValid && g_renderRecord.renderedFovValid) {
            fullLens = true; memcpy(renderedLens, g_renderRecord.renderedFov, sizeof(renderedLens));
        }
        ReleaseSRWLockShared(&g_poseLock);
        if(tagged&&auditRecord.renderedViewValid&&auditRecord.renderedFovValid) {
            const uint32_t sourceSerial=auditRecord.serial;bool delayed=false;
            auditRecord=g_finalImagePose.Apply(auditRecord,delayed);
            submitPose.orientation={auditRecord.orientation[0],auditRecord.orientation[1],auditRecord.orientation[2],auditRecord.orientation[3]};
            submitPose.position={auditRecord.position[0],auditRecord.position[1],auditRecord.position[2]};tagTime=auditRecord.time;
            memcpy(renderedLens,auditRecord.renderedFov,sizeof(renderedLens));fullLens=true;
            static unsigned imageFrames=0;if(++imageFrames==1||imageFrames%300==0)
                XrLog("[IMAGEPOSE] ACTIVE: final-pixel association source camera serial=%u -> submitted image serial=%u delayed=%d (measured one-frame image lag)",sourceSerial,auditRecord.serial,delayed);
        } else g_finalImagePose.Reset();
        if (tagged && tagTime > 0) {   // [LATENCY]
            const XrPosef now = CenterEyePose(g_views[0].pose, g_views[1].pose);
            const XrQuaternionf& a = submitPose.orientation; const XrQuaternionf& b = now.orientation;
            float d = fabsf(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w); if (d > 1.f) d = 1.f;
            const double deg = 2.0 * acos((double)d) * 57.29578, lat = (double)(g_frameState.predictedDisplayTime - tagTime) / 1e6;
            double margin = 1e9;
            for (int e = 0; e < 2; ++e) {
                const XrFovf& f = g_views[e].fov;
                const double m[4] = { hHalf - fabs(f.angleLeft), hHalf - fabs(f.angleRight), vHalf - fabs(f.angleUp), vHalf - fabs(f.angleDown) };
                for (double x : m) if (x * 57.29578 < margin) margin = x * 57.29578;
            }
            std::lock_guard<std::mutex> lk(g_statMx);
            ++g_stN; g_stLatSum += lat; if (lat > g_stLatMax) g_stLatMax = lat; g_stDegSum += deg; if (deg > g_stDegMax) g_stDegMax = deg;
            if (margin < g_stMargin) g_stMargin = margin; if (deg > margin) ++g_stOver;
        }
        for (int eye = 0; eye < 2; ++eye)
        {
            projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
            projViews[eye].pose = submitPose;        // same pose both eyes
            projViews[eye].fov.angleLeft  = -hHalf; projViews[eye].fov.angleRight = hHalf;
            projViews[eye].fov.angleUp    =  vHalf; projViews[eye].fov.angleDown  = -vHalf;
            if (fullLens) {
                const auto& f = renderedLens[eye];
                projViews[eye].fov = {f.angleLeft, f.angleRight, f.angleUp, f.angleDown};
            }
            projViews[eye].subImage.swapchain = g_eyes[eye].swapchain;
            projViews[eye].subImage.imageArrayIndex = 0;
            projViews[eye].subImage.imageRect.offset.x = 0;
            projViews[eye].subImage.imageRect.offset.y = 0;
            projViews[eye].subImage.imageRect.extent.width = static_cast<int32_t>(g_eyes[eye].width);
            projViews[eye].subImage.imageRect.extent.height = static_cast<int32_t>(g_eyes[eye].height);
        }
        proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
        proj.layerFlags = 0;
        proj.space = g_appSpace;
        proj.viewCount = 2;
        proj.views = projViews;
        const bool copiedLeft=CopyToEye(0,leftEye,tagged?&auditRecord:nullptr);
        const bool copiedRight=CopyToEye(1,rightEye,tagged?&auditRecord:nullptr);
        submitted = copiedLeft && copiedRight;
        if(!submitted)XrLog("[COPYOWNERSHIP] stereo pair withheld: copied/released L=%d R=%d; refusing fresh pose on an old/partial XR image",copiedLeft,copiedRight);
        static unsigned lensFrames = 0;
        if (fullLens && (++lensFrames == 1 || lensFrames % 300 == 0))
            XrLog("[P5RXR] [fulllens] ACTIVE: exact rendered per-eye FOV; lens centre biases L=(%.5f,%.5f) R=(%.5f,%.5f) deg",
                .5f*(renderedLens[0].angleLeft+renderedLens[0].angleRight)*57.29578f,
                .5f*(renderedLens[0].angleUp+renderedLens[0].angleDown)*57.29578f,
                .5f*(renderedLens[1].angleLeft+renderedLens[1].angleRight)*57.29578f,
                .5f*(renderedLens[1].angleUp+renderedLens[1].angleDown)*57.29578f);
    }

    // [HUD] the game's HUD layer as per-region quads (premultiplied alpha) over the projection, then
    // [OVERLAY] the second UI image with the same layout on top. One image per quad = the same pixels
    // in both eyes: no eye separation, convergence or eye swap ever applies to the UI.
    XrCompositionLayerQuad hud[24] = {};
    int hudCount = 0;
    auto ensureSize = [](int idx, ID3D11Texture2D* img, uint32_t& swW, uint32_t& swH) {
        if (img == nullptr || g_eyes[idx].swapchain == nullptr) return;
        D3D11_TEXTURE2D_DESC hd = {}; img->GetDesc(&hd);
        if (hd.Width == g_eyes[idx].width && hd.Height == g_eyes[idx].height) return;
        // run 7: CopyResource needs identical sizes; a mismatch made every UI copy fail silently
        if (g_fn.destroySwapchain != nullptr) g_fn.destroySwapchain(g_eyes[idx].swapchain);
        g_eyes[idx] = Eye{}; g_eyeAcq[idx] = EyeAcquire{};
        swW = hd.Width; swH = hd.Height;
        CreateSwapchainAt(idx, hd.Width, hd.Height);
    };
    if (submitted && g_viewSpace != nullptr)
    {
        ensureSize(2, g_hudImage, g_hudSwW, g_hudSwH);
        ensureSize(3, g_overlayImage, g_ovSwW, g_ovSwH);
    }
    const P5RXr::HudLayout& L = g_hudLayout;
    const bool hudOk = submitted && g_hudImage != nullptr && g_viewSpace != nullptr && g_eyes[2].swapchain != nullptr && L.count > 0;
    const bool ovOk = submitted && g_overlayImage != nullptr && g_viewSpace != nullptr && g_eyes[3].swapchain != nullptr && L.count > 0;
    if (hudOk || ovOk)
    {
        if ((L.redock || !g_hudAnchored) && g_viewsValid)
        {
            const XrPosef head = CenterEyePose(g_views[0].pose, g_views[1].pose);
            g_hudAnchor.orientation = YawOnlyQuat(head.orientation);
            g_hudAnchor.position = head.position;
            g_hudAnchored = true;
            XrLog("[P5RXR] HUD docked in front of the head (%.2fm wide, %.2fm away, height %+.2fm)", L.width, L.distance, L.height);
        }
        // one panel size for both images (the HUD image's aspect when present)
        const int base = hudOk ? 2 : 3;
        const float panelW = L.width, panelH = L.width * static_cast<float>(g_eyes[base].height) / static_cast<float>(g_eyes[base].width);
        const float yaw = 2.0f * std::atan2(g_hudAnchor.orientation.y, g_hudAnchor.orientation.w);
        const float cy = std::cos(yaw), sy = std::sin(yaw);
        float ipd = 0.064f;
        if (g_viewsValid)
        {
            const float dx = g_views[1].pose.position.x - g_views[0].pose.position.x, dy = g_views[1].pose.position.y - g_views[0].pose.position.y, dz = g_views[1].pose.position.z - g_views[0].pose.position.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d > 0.04f && d < 0.09f) ipd = d;
        }
        for (int pass = 0; pass < 2; ++pass)
        {
            const int idx = pass == 0 ? 2 : 3;
            if ((pass == 0 && !hudOk) || (pass == 1 && !ovOk)) continue;
            CopyToEye(idx, pass == 0 ? g_hudImage : g_overlayImage);
            const float W = static_cast<float>(g_eyes[idx].width), H = static_cast<float>(g_eyes[idx].height);
            // all regions in one mode -> one full-image quad (fewer layers, no seams)
            P5RXr::HudRegion merged[8]; int mergedCount = 0;
            bool same = L.count > 0;
            for (int i = 0; i < L.count && i < 8; ++i) if (L.region[i].mode != L.region[0].mode || L.region[i].oy != 0.f) same = false;
            if (same) { merged[0] = { 0.0f, 0.0f, 1.0f, 1.0f, L.region[0].mode }; mergedCount = 1; }
            else { for (int i = 0; i < L.count && i < 8; ++i) merged[mergedCount++] = L.region[i]; }
            for (int i = 0; i < mergedCount; ++i)
            {
                const P5RXr::HudRegion& r = merged[i];
                if (r.mode == 2 || (r.mode == 1 && !g_hudAnchored)) continue;
                const int x0 = static_cast<int>(r.u0 * W + 0.5f), x1 = static_cast<int>(r.u1 * W + 0.5f);
                const int y0 = static_cast<int>(r.v0 * H + 0.5f), y1 = static_cast<int>(r.v1 * H + 0.5f);
                if (x1 <= x0 || y1 <= y0) continue;
                // region centre on the panel (panel centred on the view axis, raised by height)
                const float lx = ((r.u0 + r.u1) * 0.5f - 0.5f) * panelW;
                const float ly = (0.5f - (r.v0 + r.v1) * 0.5f) * panelH + L.height + r.oy;   // [CMMREGIONS] per-region height
                const float lz = -L.distance;
                // Run 17: the UI sits 100 m away, scaled to the same apparent size, so the two eyes see
                // it at the same angle (no disparity) without relying on per-eye layer visibility.
                const float farScale = 100.0f / L.distance;
                {
                    XrCompositionLayerQuad& q = hud[hudCount];
                    q.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
                    // [MENUSOLID] a game menu's alpha has whole rectangles at 0 under drawn art (they
                    // vanished) and see-through backgrounds (IM phone); menus are drawn solid instead
                    q.layerFlags = (pass == 0 && L.opaque) ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT_VALUE;
                    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
                    q.subImage.swapchain = g_eyes[idx].swapchain;
                    q.subImage.imageRect.offset = { x0, y0 };
                    q.subImage.imageRect.extent = { x1 - x0, y1 - y0 };
                    q.size = { (r.u1 - r.u0) * panelW * farScale, (r.v1 - r.v0) * panelH * farScale };
                    const float x = lx * farScale, y = ly * farScale, z = lz * farScale;
                    if (r.mode == 0)
                    {
                        q.space = g_viewSpace;
                        q.pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
                        q.pose.position = { x, y, z };
                    }
                    else
                    {
                        q.space = g_appSpace;
                        q.pose.orientation = g_hudAnchor.orientation;
                        q.pose.position = { g_hudAnchor.position.x + cy * x + sy * z,
                                            g_hudAnchor.position.y + y,
                                            g_hudAnchor.position.z - sy * x + cy * z };
                    }
                    ++hudCount;
                }
            }
        }
    }
    g_hudImage = nullptr;
    g_overlayImage = nullptr;
    g_hudLayout.redock = false;
    const XrCompositionLayerBaseHeader* layers[25] = {};
    uint32_t layerCount = 0;
    if (submitted)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    for (int i = 0; i < hudCount; ++i)
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud[i]);

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = layerCount > 0 ? layers : nullptr;
    const XrResult ended=TracedCall("xrEndFrame", g_fn.endFrame, g_session, &fei);
    FinalTraceEndSubmission(submitted,ended,fei.displayTime);
    FlashFrames::End(submitted,ended,fei.displayTime);
    g_frameBegun = false;
}

// [SCREEN] Game menus: one flat image on a world-locked quad in front of the head. The quad is
// anchored (yaw + position) when screen mode begins, so it stays put while the head moves.
bool    g_screenAnchored = false;
XrPosef g_screenPose = {};
void SubmitScreenImpl(ID3D11Texture2D* image, float widthMeters, float distanceMeters, bool reanchor) noexcept
{
    g_finalImagePose.Reset();
    g_hudImage = nullptr;   // [HUD] menus are shown whole on the screen quad
    g_overlayImage = nullptr;
    if (!g_frameBegun) return;
    if (reanchor || !g_screenAnchored)
    {
        g_screenAnchored = false;
        if (g_viewsValid)
        {
            const XrPosef head = CenterEyePose(g_views[0].pose, g_views[1].pose);
            const XrQuaternionf yaw = YawOnlyQuat(head.orientation);
            const float angle = 2.0f * std::atan2(yaw.y, yaw.w);
            g_screenPose.orientation = yaw;
            g_screenPose.position = { head.position.x - distanceMeters * std::sin(angle),
                                      head.position.y,
                                      head.position.z - distanceMeters * std::cos(angle) };
            g_screenAnchored = true;
            XrLog("[P5RXR] SCREEN anchored %.2fm ahead, %.2fm wide (yaw %.1f deg)", distanceMeters, widthMeters, angle * 57.2958f);
        }
    }
    XrCompositionLayerQuad quad = {};
    bool submitted = false;
    if (g_screenAnchored && g_context != nullptr && image != nullptr && g_eyes[0].swapchain != nullptr)
    {
        CopyToEye(0, image);
        const float aspect = g_eyes[0].height > 0 ? static_cast<float>(g_eyes[0].width) / static_cast<float>(g_eyes[0].height) : 16.0f / 9.0f;
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
        quad.layerFlags = 0;
        quad.space = g_appSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
        quad.subImage.swapchain = g_eyes[0].swapchain;
        quad.subImage.imageArrayIndex = 0;
        quad.subImage.imageRect.extent.width = static_cast<int32_t>(g_eyes[0].width);
        quad.subImage.imageRect.extent.height = static_cast<int32_t>(g_eyes[0].height);
        quad.pose = g_screenPose;
        quad.size = { widthMeters, widthMeters / aspect };
        submitted = true;
    }
    const XrCompositionLayerBaseHeader* layers[1] = {};
    if (submitted) layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = g_frameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = submitted ? 1u : 0u;
    fei.layers = submitted ? layers : nullptr;
    TracedCall("xrEndFrame", g_fn.endFrame, g_session, &fei);
    g_frameBegun = false;
}

bool InitImpl(ID3D11Device* device) noexcept
{
    EnsureSelfModule();
    if (device == nullptr) return false;

    // Idempotent: bring-up runs once. Subsequent calls report progress (true = still coming up / ready).
    bool expected = false;
    if (!g_bringupStarted.compare_exchange_strong(expected, true))
        return !g_bringupFailed.load();

    // Fast "no runtime" answer: if the loader isn't installed, there's no OpenXR at all — fail now,
    // no thread. (CreateInstance re-loads + pins it on the bring-up thread.)
    HMODULE probe = LoadLibraryW(L"openxr_loader.dll");
    if (probe == nullptr)
    {
        XrLog("[P5RXR] openxr_loader.dll not present — no OpenXR runtime; VR disabled.");
        g_bringupStarted.store(false);
        return false;
    }
    FreeLibrary(probe);

    g_gameDevice = device;
    g_gameDevice->AddRef();
    g_gameDevice->GetImmediateContext(&g_gameCtx);
    IDXGIAdapter* adapter = nullptr;
    {
        IDXGIDevice* dd = nullptr;
        if (SUCCEEDED(g_gameDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) { dd->GetAdapter(&adapter); dd->Release(); }
    }
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT dhr = S_OK;
    if (!kOwnXrDevice) {   // session + swapchains on the game's device (proven path)
        g_device = g_gameDevice; g_device->AddRef();
        g_context = g_gameCtx; if (g_context) g_context->AddRef();
        got = g_device->GetFeatureLevel();
    } else dhr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 2, D3D11_SDK_VERSION, &g_device, &got, &g_context);
    if (adapter) adapter->Release();
    if (FAILED(dhr) || g_device == nullptr || g_context == nullptr || g_gameCtx == nullptr)
    {
        XrLog("[P5RXR] own XR device create FAILED hr=0x%08X", (unsigned)dhr);
        SafeReleaseXr(g_context); SafeReleaseXr(g_device); SafeReleaseXr(g_gameCtx); SafeReleaseXr(g_gameDevice);
        g_bringupStarted.store(false);
        return false;
    }
    XrLog("[P5RXR] Init: game device=%p (source only) | OWN XR device=%p ctx=%p fl=0x%X — bringing up OpenXR (async).",
          (void*)g_gameDevice, (void*)g_device, (void*)g_context, (unsigned)got);

    HANDLE th = CreateThread(nullptr, 0, &XrBringupThread, nullptr, 0, nullptr);
    if (th != nullptr) { CloseHandle(th); return true; }

    XrLog("[P5RXR] CreateThread(bring-up) FAILED err=%lu", GetLastError());
    g_bringupFailed.store(true);
    return false;
}

void ReleaseDeviceResourcesImpl() noexcept
{
    FlashFrames::Release();
    g_finalImagePose.Reset();
    FinalTraceRelease();
    // A prediction is now kept open across the game render. Close it BEFORE handing
    // the shared device back for resize; no prepared frame may survive that boundary.
    if (g_frameBegun) EndFrameNoLayers();
    for (int eye = 0; eye < 4; ++eye)
    {
        if (g_eyes[eye].swapchain != nullptr && g_fn.destroySwapchain != nullptr)
            g_fn.destroySwapchain(g_eyes[eye].swapchain);
        g_eyes[eye].swapchain = nullptr;
        g_eyes[eye].imageCount = 0;
        for (uint32_t i = 0; i < 8; ++i) g_eyes[eye].images[i] = nullptr;
        g_eyeAcq[eye] = EyeAcquire{};
    }
    XrLog("[P5RXR] eye swapchains RELEASED off the game device (resize window). Session stays alive; frames submit 0 layers.");
}

bool RecreateDeviceResourcesImpl() noexcept
{
    if (g_session == nullptr) return false;
    if (g_eyes[0].swapchain != nullptr && g_eyes[1].swapchain != nullptr && g_eyes[2].swapchain != nullptr && g_eyes[3].swapchain != nullptr) return true;
    ReleaseDeviceResourcesImpl();   // never leak a partial set
    const bool ok = CreateEyeSwapchains();
    XrLog("[P5RXR] eye swapchains REBUILT after resize: %s", ok ? "ok" : "FAILED");
    return ok;
}

void ShutdownImpl() noexcept
{
    FlashFrames::Release();
    FlashFrames::writer.Stop();
    FinalTraceRelease();
    g_ready.store(false);
    g_sessionRunning = false;

    if (g_frameBegun) EndFrameNoLayers();

    for (int eye = 0; eye < 4; ++eye)
    {
        if (g_eyes[eye].swapchain != nullptr && g_fn.destroySwapchain != nullptr)
            g_fn.destroySwapchain(g_eyes[eye].swapchain);
        g_eyes[eye] = Eye{};
        g_eyeAcq[eye] = EyeAcquire{};
    }
    if (g_appSpace != nullptr && g_fn.destroySpace != nullptr) g_fn.destroySpace(g_appSpace);
    g_appSpace = nullptr;
    if (g_localSpace != nullptr && g_fn.destroySpace != nullptr) g_fn.destroySpace(g_localSpace);
    g_localSpace = nullptr;
    if (g_viewSpace != nullptr && g_fn.destroySpace != nullptr) g_fn.destroySpace(g_viewSpace);
    g_viewSpace = nullptr;
    if (g_session != nullptr && g_fn.destroySession != nullptr) g_fn.destroySession(g_session);
    g_session = nullptr;
    if (g_instance != nullptr && g_fn.destroyInstance != nullptr) g_fn.destroyInstance(g_instance);
    g_instance = nullptr;

    ReleaseShared(0); ReleaseShared(1);
    SafeReleaseXr(g_context);
    SafeReleaseXr(g_device);
    SafeReleaseXr(g_gameCtx);
    SafeReleaseXr(g_gameDevice);
    g_viewsValid = false;
    XrLog("[P5RXR] Shutdown complete.");
}
}  // namespace

// ---- public interface (thin SEH wrappers so an XR hiccup can never fault into the game) ----
namespace P5RXr
{
bool Init(ID3D11Device* device) noexcept
{
    ++t_internalDepth;
    __try {
    __try { return InitImpl(device); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    } __finally { --t_internalDepth; }
}

bool IsRunning() noexcept { return g_sessionRunning; }
bool HasRenderFrame() noexcept { return g_frameBegun; }
bool PrepareRenderFrame() noexcept
{
    if (g_frameBegun) return true;
    return BeginFrame();
}
bool IsInternalCall() noexcept { return t_internalDepth != 0; }
const char* ActiveCall() noexcept { return g_activeCall.load(std::memory_order_acquire); }

bool BeginFrame() noexcept
{
    ++t_internalDepth;
    __try {
    __try { return BeginFrameImpl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_frameBegun = false; return false; }
    } __finally { --t_internalDepth; }
}

bool GetEyeView(int eye, Pose* outPose, Fov* outFov) noexcept
{
    if (eye < 0 || eye > 1 || !g_viewsValid) return false;
    const XrView& v = g_views[eye];
    if (outPose != nullptr)
    {
        outPose->orientation[0] = v.pose.orientation.x;
        outPose->orientation[1] = v.pose.orientation.y;
        outPose->orientation[2] = v.pose.orientation.z;
        outPose->orientation[3] = v.pose.orientation.w;
        outPose->position[0] = v.pose.position.x;
        outPose->position[1] = v.pose.position.y;
        outPose->position[2] = v.pose.position.z;
    }
    if (outFov != nullptr)
    {
        outFov->angleLeft = v.fov.angleLeft;
        outFov->angleRight = v.fov.angleRight;
        outFov->angleUp = v.fov.angleUp;
        outFov->angleDown = v.fov.angleDown;
    }
    return true;
}

uint32_t EyeWidth() noexcept { return g_eyeWidth; }
uint32_t EyeHeight() noexcept { return g_eyeHeight; }

void SubmitEyes(ID3D11Texture2D* leftEye, ID3D11Texture2D* rightEye) noexcept
{
    ++t_internalDepth;
    __try {
    __try { SubmitEyesImpl(leftEye, rightEye); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_frameBegun = false; }
    } __finally { --t_internalDepth; }
}

void SetOverlay(ID3D11Texture2D* image) noexcept { g_overlayImage = image; }
uint32_t LastHazeFrame() noexcept { return FlashFrames::lastHaze.load(); }
uint32_t LastEyeSpikeFrame() noexcept { return FlashFrames::lastEyeSpike.load(); }
void SetFlashRecording(bool enabled, uint64_t frame) noexcept { FlashFrames::enabled=enabled;FlashFrames::frame=frame; }
void SetHud(ID3D11Texture2D* image, const HudLayout* layout) noexcept
{
    g_hudImage = image;
    if (layout != nullptr)
    {
        const bool redock = g_hudLayout.redock || layout->redock;   // keep a pending request until submitted
        g_hudLayout = *layout;
        g_hudLayout.redock = redock;
    }
}
void SubmitScreen(ID3D11Texture2D* image, float widthMeters, float distanceMeters, bool reanchor) noexcept
{
    ++t_internalDepth;
    __try {
    __try { SubmitScreenImpl(image, widthMeters, distanceMeters, reanchor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_frameBegun = false; }
    } __finally { --t_internalDepth; }
}

// Stored for the render side (render-fov match). This layer just holds the values.
void SetGameProjection(float m00, float m11) noexcept { g_projM00.store(m00); g_projM11.store(m11); }
void GetGameProjection(float* m00, float* m11) noexcept { if (m00) *m00 = g_projM00.load(); if (m11) *m11 = g_projM11.load(); }
void OverrideSubmitProjection(float m00, float m11) noexcept { g_ovrM00 = m00; g_ovrM11 = m11; }
bool GetRenderPose(Pose* out) noexcept
{
    AcquireSRWLockShared(&g_poseLock);
    const bool ok = g_renderPoseValid;
    if (ok && out) *out = g_renderRecord;
    ReleaseSRWLockShared(&g_poseLock);
    if (!ok || !out) return false;
    return true;
}
void SetEyeResolution(uint32_t w, uint32_t h) noexcept { g_forceEyeW.store(w); g_forceEyeH.store(h); }

bool GetHeadPose(Pose* outPose, float* outVFovRad) noexcept
{
    AcquireSRWLockShared(&g_poseLock);
    bool ok = g_headValid;
    XrPosef p = g_headPose; float vf = g_headVFov; const uint32_t serial = g_headSerial, epoch = g_headSpaceEpoch; const XrTime t = g_headTime;
    ReleaseSRWLockShared(&g_poseLock);
    if (!ok) return false;
    if (outPose) {
        outPose->renderedFovValid = false;
        outPose->serial = serial; outPose->time = (long long)t;
        outPose->spaceEpoch = epoch;
        outPose->orientation[0] = p.orientation.x; outPose->orientation[1] = p.orientation.y;
        outPose->orientation[2] = p.orientation.z; outPose->orientation[3] = p.orientation.w;
        outPose->position[0] = p.position.x; outPose->position[1] = p.position.y; outPose->position[2] = p.position.z;
    }
    if (outVFovRad) *outVFovRad = vf;
    return true;
}

void SetRenderPose(const Pose* pose) noexcept
{
    AcquireSRWLockExclusive(&g_poseLock);
    if (pose) {
        g_renderRecord = *pose;
        g_renderPose.orientation = { pose->orientation[0], pose->orientation[1], pose->orientation[2], pose->orientation[3] };
        g_renderPose.position = { pose->position[0], pose->position[1], pose->position[2] };
        g_renderPoseTime = (XrTime)pose->time;
        g_renderPoseValid = true;
    } else g_renderPoseValid = false;
    ReleaseSRWLockExclusive(&g_poseLock);
}

SubmitStats TakeSubmitStats() noexcept {
    std::lock_guard<std::mutex> lk(g_statMx);
    SubmitStats s{ g_stN, g_stN ? g_stLatSum / g_stN : 0, g_stLatMax, g_stN ? g_stDegSum / g_stN : 0, g_stDegMax, g_stMargin < 1e8 ? g_stMargin : 0, (double)g_stOver };
    g_stN = g_stOver = 0; g_stLatSum = g_stLatMax = g_stDegSum = g_stDegMax = 0; g_stMargin = 1e9;
    return s;
}
long long LastWaitReturnQpc() noexcept { return g_waitReturnQpc.load(std::memory_order_relaxed); }
uint32_t HeadSerial() noexcept { AcquireSRWLockShared(&g_poseLock); const uint32_t s = g_headSerial; ReleaseSRWLockShared(&g_poseLock); return s; }
double DisplayPeriodMs() noexcept { const XrDuration p = g_frameState.predictedDisplayPeriod; return p > 0 ? (double)p / 1e6 : 0.0; }
void Recenter() noexcept { g_recenterReq.store(true); g_hudAnchored = false; }
void SetGameCopyFn(void* fn) noexcept { g_gameRawCopy = (PFN_RawCopy)fn; }   // re-anchor "forward" to the current head (next frame)

void ReleaseDeviceResources() noexcept
{
    ++t_internalDepth;
    __try {
    __try { ReleaseDeviceResourcesImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    } __finally { --t_internalDepth; }
}
bool RecreateDeviceResources() noexcept
{
    ++t_internalDepth;
    __try {
    __try { return RecreateDeviceResourcesImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    } __finally { --t_internalDepth; }
}
void Shutdown() noexcept
{
    ++t_internalDepth;
    __try {
    __try { ShutdownImpl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    } __finally { --t_internalDepth; }
}
}
