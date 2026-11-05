// p5r_probe.cpp — Persona 5 Royal (GFD engine, x64 DX11) CAMERA-MATRIX FINDER (v2).
//
// v1 (log-proven 2026-07-14, title + gameplay): 12 hooks live past Denuvo alongside
// ReShade (which wraps device/context/swapchain — hooks on the wrapper vtable catch all)
// + Reloaded-II. Swapchain 3840x2160 RGBA8 FLIP_DISCARD. Gameplay = 1.4-2k DrawIndexed,
// ZERO instanced draws, 36-91 Dispatch. CB architecture: fixed pool of 7168-byte CBs on
// every VS/PS slot b0-b13, streamed 100% via UpdateSubresource (Map never touches a CB).
// PS slots rebind ~17/frame (per-frame constants); VS b1 ~300/frame (per-draw data).
//
// v2 goal: FIND the view/projection matrices. Because every CB upload passes through
// UpdateSubresource with a CPU-side source pointer, we classify 4x4 blocks IN THE SOURCE
// PAYLOAD (no GPU readback):
//   - census (sampled): (size, offset, class) histogram — which offsets ALWAYS hold
//     perspective/affine matrices across many frames
//   - forensic capture (one frame, F7 or auto-armed on first busy frame): every CB upload's
//     matrix map + draw/dispatch markers + render-target changes -> pass structure
// Camera fingerprint protocol: stand still ~5s, then rotate camera, F7 during motion.
// Projection = PERSPECTIVE-class, static. View = AFFINE/rot-class, changes with camera.
//
// This build does NOT poke/render/stereo. Instrument first (project rule). Read-only.
//
// Keys:  F7 = arm one-frame forensic capture   F8 = status/histograms
//        F10 = per-frame line toggle           F4 = reset histograms
//        (F5/F9 avoided — common quicksave/load binds)
// Log: p5rprobe_log.txt next to this .asi (game root).

#define WIN32_LEAN_AND_MEAN
#include "p5r_culling.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>   // [BATCH] constant-buffer offsets (VS/PSGetConstantBuffers1)
#include <tlhelp32.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cmath>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <psapi.h>
#include "MinHook.h"
#include <d3dcompiler.h>
#include "p5r_xr.h"
#include "p5r_gfdcam.h"
#include "p5r_camera_controls.h"
#include "p5r_menu.h"
#include "p5r_input.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ------------------------------------------------------------------ logging
static FILE*      g_log = nullptr;
static std::mutex g_logMx;
static void OpenLog() {
    char mod[MAX_PATH]{}; GetModuleFileNameA((HMODULE)&__ImageBase, mod, MAX_PATH);
    char* slash = strrchr(mod, '\\'); if (slash) *(slash + 1) = 0;
    char path[MAX_PATH]; wsprintfA(path, "%sp5rprobe_log.txt", mod);
    g_log = fopen(path, "w");}
static void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
}
#include "p5r_resspoof.inl"
static std::atomic<uint32_t> g_fpAnchorUses{0}, g_fpManualUses{0}, g_evFpUses{0};   // [FPANCHOR] camera uploads using Joker's eye / the old offset
static char g_dumpLabel[64] = "";   // [UIDUMP] non-empty: automatic labelled dump (file names carry it)
static constexpr bool kD3DDebugLayer = false;  // [D3DDBG] off (with VR live it killed the game silently at mirror start)
static void DebugLayerDrain(const char* when);
static ID3D11InfoQueue* g_infoQ = nullptr;
static void DebugLayerAttach(ID3D11Device* dev);
static void ModOf(const void* addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod);
    char name[MAX_PATH] = "?";
    if (mod) { GetModuleFileNameA(mod, name, MAX_PATH); char* s = strrchr(name, '\\'); if (s) memmove(name, s + 1, strlen(s)); }
    snprintf(out, cap, "%s+0x%zX", name, (size_t)((uintptr_t)addr - (uintptr_t)mod));
}
// FULL module path variant — two modules can share a basename (game-dir proxy vs System32), so the
// stall diagnostics must print the whole path to tell them apart.
static void ModPathOf(const void* addr, char* out, size_t cap) {
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    snprintf(out, cap, "%s+0x%zX", name, (size_t)((uintptr_t)addr - (uintptr_t)mod));
}

// ------------------------------------------------------------------ vtable indices (verified in MGS2 lane)
enum : int {
    VT_SC_Present        = 8,
    VT_SC_ResizeBuffers  = 13,
    VT_SC_Present1       = 22,
    VT_DEV_CreateDeferredContext = 27,
    VT_CTX_VSSetCB       = 7,
    VT_CTX_DrawIndexed   = 12,
    VT_CTX_Draw          = 13,
    VT_CTX_Map           = 14,
    VT_CTX_PSSetCB       = 16,
    VT_CTX_DrawIdxInst   = 20,
    VT_CTX_DrawInst      = 21,
    VT_CTX_OMSetRTs      = 33,
    VT_CTX_Dispatch      = 41,
    VT_CTX_UpdateSubres  = 48,
};

// ------------------------------------------------------------------ original fn pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeferredContext)(ID3D11Device*, UINT, ID3D11DeviceContext**);
typedef void  (STDMETHODCALLTYPE* PFN_DrawIndexed)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void  (STDMETHODCALLTYPE* PFN_Draw)(ID3D11DeviceContext*, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_DrawIdxInst)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_DrawInst)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_Dispatch)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_SetCBs)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Map)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void  (STDMETHODCALLTYPE* PFN_UpdateSubres)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef void  (STDMETHODCALLTYPE* PFN_OMSetRTs)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);

static PFN_Present       g_origPresent = nullptr;
static PFN_Present1      g_origPresent1 = nullptr;
static PFN_ResizeBuffers g_origResizeBuffers = nullptr;
static PFN_CreateDeferredContext g_origCreateDeferredContext = nullptr;
static PFN_DrawIndexed   g_origDrawIndexed = nullptr;
static PFN_Present       g_origPresentLate = nullptr;
static PFN_Present1      g_origPresent1Late = nullptr;
static void*             g_initialPresentTarget = nullptr;
static void*             g_initialPresent1Target = nullptr;
static std::atomic<bool> g_latePresentHooked{false};
static std::atomic<bool> g_latePresent1Hooked{false};
// v7 title-stall forensics: snapshot the first 16 bytes of each MinHook'd DXGI code entry right
// after hooking. At stall time, compare — proves/disproves "someone rewrote our detour" without
// guessing. (The old ReShade-rewrite theory is untestable now that ReShade is gone; overlays like
// GameOverlayRenderer64 re-hook Present around fullscreen transitions the same way.)
static void*   g_resizeCodeTarget = nullptr;
static uint8_t g_presentBytesBoot[16] = {}, g_present1BytesBoot[16] = {}, g_resizeBytesBoot[16] = {};
static bool    g_bootBytesValid = false;
static PFN_Draw          g_origDraw = nullptr;
static PFN_DrawIdxInst   g_origDrawIdxInst = nullptr;
static PFN_DrawInst      g_origDrawInst = nullptr;
static PFN_Dispatch      g_origDispatch = nullptr;
static PFN_SetCBs        g_origVSSetCB = nullptr, g_origPSSetCB = nullptr;
static PFN_Map           g_origMap = nullptr;
static PFN_UpdateSubres  g_origUpdateSubres = nullptr;
static PFN_OMSetRTs      g_origOMSetRTs = nullptr;

static PFN_DrawIndexed   g_origDefDrawIndexed = nullptr;
static PFN_Draw          g_origDefDraw = nullptr;
static PFN_DrawIdxInst   g_origDefDrawIdxInst = nullptr;
static PFN_DrawInst      g_origDefDrawInst = nullptr;
static PFN_Dispatch      g_origDefDispatch = nullptr;
static PFN_SetCBs        g_origDefVSSetCB = nullptr, g_origDefPSSetCB = nullptr;
static PFN_Map           g_origDefMap = nullptr;
static PFN_UpdateSubres  g_origDefUpdateSubres = nullptr;
static PFN_OMSetRTs      g_origDefOMSetRTs = nullptr;
static std::array<std::atomic<ID3D11DeviceContext*>, 8> g_deferredContexts{};
static std::atomic<uint32_t> g_deferredContextCount{0};
// live game device + immediate context (AddRef'd in HookLiveContext; declared here because the
// context-classification path below needs g_ctxImm as its immediate-context fast path)
static ID3D11Device*        g_dev = nullptr;
static ID3D11DeviceContext* g_ctxImm = nullptr;

// ------------------------------------------------------------------ counters
static std::atomic<uint64_t> g_frame{0};
static std::atomic<uint32_t> g_cDrawIdx{0}, g_cDraw{0}, g_cDrawIdxInst{0}, g_cDrawInst{0}, g_cDispatch{0};
static std::atomic<uint32_t> g_cMapBuf{0}, g_cMapDiscard{0}, g_cMapOther{0}, g_cUpdBuf{0}, g_cUpdOther{0};
static std::atomic<uint32_t> g_cVSSet{0}, g_cPSSet{0}, g_cRTSet{0}, g_cUpdSeq{0};
static uint32_t g_lDrawIdx=0, g_lDraw=0, g_lDrawIdxInst=0, g_lDrawInst=0, g_lDispatch=0;
static uint32_t g_lMapBuf=0, g_lMapDiscard=0, g_lMapOther=0, g_lUpdBuf=0, g_lUpdOther=0, g_lVSSet=0, g_lPSSet=0, g_lRTSet=0;
static volatile bool g_perFrameLine = false;
static IDXGISwapChain* volatile g_lastSc = nullptr;   // for GetDesc at dump time (present thread only)

// per-slot CB stats (kept from v1)
static constexpr int kSlots = 16;
static volatile bool g_censusFrame = false;
struct StageCB {
    std::atomic<uint32_t> slotSets[kSlots];
    std::mutex mx;
    std::unordered_set<ID3D11Buffer*> distinct[kSlots];
    std::map<UINT, uint32_t> sizeHist[kSlots];
};
static StageCB g_vs, g_ps;
static void CensusCB(StageCB& st, UINT startSlot, UINT num, ID3D11Buffer* const* bufs) {
    for (UINT i = 0; i < num; ++i) {
        UINT slot = startSlot + i;
        if (slot >= kSlots) break;
        st.slotSets[slot].fetch_add(1, std::memory_order_relaxed);
        if (!g_censusFrame || !bufs || !bufs[i]) continue;
        std::lock_guard<std::mutex> lk(st.mx);
        if (st.distinct[slot].size() < 4096 && st.distinct[slot].insert(bufs[i]).second) {
            D3D11_BUFFER_DESC d{}; bufs[i]->GetDesc(&d);
            st.sizeHist[slot][d.ByteWidth]++;
        }
    }
}
static std::mutex g_upMx;
static std::map<UINT, uint32_t> g_updSizeHist;

// ------------------------------------------------------------------ matrix-shape classifier (BF lineage)
// classes: 'Z' zero, 'J' junk, 'I' identity, 'P' perspective-ish, 'A' affine, 'R' rot-ish, 'D' dense
static bool AllZero(const float* m) { for (int i=0;i<16;++i) if (m[i]!=0.f) return false; return true; }
static bool AllFinite(const float* m) { for (int i=0;i<16;++i){ if(!(m[i]==m[i])||fabsf(m[i])>1e12f) return false; } return true; }
static bool RowsOrthonormal(const float* m) {
    for (int r = 0; r < 3; ++r) { float l = m[r*4]*m[r*4]+m[r*4+1]*m[r*4+1]+m[r*4+2]*m[r*4+2]; if (fabsf(l-1.f)>0.05f) return false; }
    if (fabsf(m[0]*m[4]+m[1]*m[5]+m[2]*m[6])>0.05f) return false;
    if (fabsf(m[0]*m[8]+m[1]*m[9]+m[2]*m[10])>0.05f) return false;
    return true;
}
static bool ColsOrthonormal(const float* m) {
    for (int c = 0; c < 3; ++c) { float l = m[c]*m[c]+m[c+4]*m[c+4]+m[c+8]*m[c+8]; if (fabsf(l-1.f)>0.05f) return false; }
    return fabsf(m[0]*m[1]+m[4]*m[5]+m[8]*m[9]) <= 0.05f;
}
static bool IsIdentity(const float* m) { return m[0]==1.f&&m[5]==1.f&&m[10]==1.f&&m[15]==1.f&&m[1]==0.f&&m[4]==0.f&&m[12]==0.f&&m[13]==0.f&&m[14]==0.f; }
static bool LooksPerspective(const float* m) {
    bool rm = (fabsf(fabsf(m[11])-1.f)<0.01f || fabsf(m[11])>0.5f) && m[15]==0.f;
    bool cm = (fabsf(fabsf(m[14])-1.f)<0.01f || fabsf(m[14])>0.5f) && m[15]==0.f;
    return rm || cm;
}
static char ClassifyBlock(const float* m) {
    if (AllZero(m))         return 'Z';
    if (!AllFinite(m))      return 'J';
    if (IsIdentity(m))      return 'I';
    if (LooksPerspective(m))return 'P';
    bool ro=RowsOrthonormal(m), co=ColsOrthonormal(m);
    if (ro && m[12]==0.f&&m[13]==0.f&&m[14]==0.f) return 'A';   // row-major, translation col4
    if (co && m[3]==0.f&&m[7]==0.f&&m[11]==0.f)   return 'A';   // col-major, translation row4
    if (ro || co)           return 'R';
    return 'D';
}

// ------------------------------------------------------------------ 4x4 matrix math (row-major, M*p)
// Layout matches the decomposition that cross-checked: M[row][col] = m[row*4+col], translation in
// the 4th column (m3/m7/m11), bottom row (m12..15). Consistent M*p ops => convention-agnostic.
static void Mat4Mul(const float* A, const float* B, float* C) {
    for (int i=0;i<4;++i) for (int j=0;j<4;++j) {
        float s=0.f; for (int k=0;k<4;++k) s += A[i*4+k]*B[k*4+j]; C[i*4+j]=s;
    }
}
static bool Mat4Inverse(const float* M, float* out) {
    double a[4][8];
    for (int i=0;i<4;++i){ for(int j=0;j<4;++j) a[i][j]=M[i*4+j]; for(int j=0;j<4;++j) a[i][4+j]=(i==j)?1.0:0.0; }
    for (int col=0; col<4; ++col) {
        int piv=col; double best=fabs(a[col][col]);
        for (int r=col+1;r<4;++r){ double v=fabs(a[r][col]); if(v>best){best=v;piv=r;} }
        if (best < 1e-9) return false;
        if (piv!=col) for(int j=0;j<8;++j){ double t=a[col][j]; a[col][j]=a[piv][j]; a[piv][j]=t; }
        double f=a[col][col];
        for (int j=0;j<8;++j) a[col][j]/=f;
        for (int r=0;r<4;++r) if(r!=col){ double ff=a[r][col]; for(int j=0;j<8;++j) a[r][j]-=ff*a[col][j]; }
    }
    for (int i=0;i<4;++i) for(int j=0;j<4;++j) out[i*4+j]=(float)a[i][4+j];
    return true;
}

// ------------------------------------------------------------------ census matrix histogram (sampled)
// key: (uploadSize, byteOffset, class) -> count. Which offsets ALWAYS hold matrices?
struct MatKey { UINT size; UINT off; char cls; bool operator<(const MatKey& o) const {
    if (size!=o.size) return size<o.size; if (off!=o.off) return off<o.off; return cls<o.cls; } };
static std::mutex g_matMx;
static std::map<MatKey, uint32_t> g_matHist;
static void ScanPayloadCensus(UINT size, const float* f) {
    UINT nBlocks = size / 64;
    std::lock_guard<std::mutex> lk(g_matMx);
    for (UINT b = 0; b < nBlocks; ++b) {
        char c = ClassifyBlock(f + b*16);
        if (c=='Z' || c=='J') continue;
        g_matHist[MatKey{size, b*64, c}]++;
    }
}

// ------------------------------------------------------------------ forensic capture (one frame)
struct CapMat { uint16_t off64; char cls; float r0[4]; float r3[4]; };
struct CapEvt {
    uint8_t type;               // 0=cbUpload 1=drawIdx 2=dispatch 3=rtChange
    void*   a;                  // upload: dst buffer | rt: rtv0
    void*   b;                  // rt: dsv
    UINT    size;               // upload: byte size | draw: indexCount | rt: width
    UINT    aux;                // rt: height | upload: total P/A/R found
    DXGI_FORMAT fmt;            // rt only
    uint8_t nMat; CapMat mats[4];
};
// full dump of a camera-signature CB (first 8 blocks = 512 bytes = 128 floats)
struct CamDump { void* dst; UINT size; float f[128]; };
static std::vector<CamDump> g_camDumps;
static const size_t kCamDumpMax = 6;   // per frame — camera CB is pooled, a few samples suffice

// camera CB fingerprint: an affine block with a large (world-space) translation in its 4th column,
// at block 0 or block 1. Translation lives in m[3],m[7],m[11] (row-major, seen in the v2 logs).
static bool IsCameraSig(const float* f, UINT size) {
    if (size < 128) return false;
    for (int blk = 0; blk <= 1; ++blk) {
        const float* m = f + blk*16;
        if (ClassifyBlock(m) != 'A') continue;
        if (fabsf(m[3]) > 500.f || fabsf(m[7]) > 500.f || fabsf(m[11]) > 500.f) return true;
    }
    return false;
}

// ------------------------------------------------------------------ camera INJECTION (stereo step 1)
// Validate the actual VP = projection * view relationship. A fixed camera may be
// axis-aligned or near the origin: neither a dense matrix nor large translation is required.
static bool IsMainCameraCB(const float* f, UINT size) {
    if (!f || size < 128 || !AllFinite(f) || !AllFinite(f + 16)) return false;
    const float* v = f + 16;
    if (fabsf(v[12]) > 0.001f || fabsf(v[13]) > 0.001f || fabsf(v[14]) > 0.001f || fabsf(v[15]-1.f) > 0.001f)
        return false;
    if (!RowsOrthonormal(v)) return false;
    // P5R's perspective projection has clip.w = -view.z. Orthographic/UI matrices fail here.
    for (int i = 0; i < 4; ++i) if (fabsf(f[12+i] + v[8+i]) > 0.05f) return false;
    float inverse[16], projection[16];
    if (!Mat4Inverse(v, inverse)) return false;
    Mat4Mul(f, inverse, projection);
    if (!AllFinite(projection) || projection[0] < 0.01f || projection[5] < 0.01f ||
        projection[0] > 1000.f || projection[5] > 1000.f || fabsf(projection[11]) < 0.0001f) return false;
    const float epsilon = 0.002f * (1.f + fabsf(projection[0]) + fabsf(projection[5]));
    // Off-center perspective terms P02/P12 are allowed; skew and non-perspective rows are not.
    for (int i : {1,4,8,9,12,13}) if (fabsf(projection[i]) > epsilon) return false;
    return fabsf(projection[14] + 1.f) < 0.01f && fabsf(projection[15]) < 0.1f;
}
// Shift the eye by d world-units along the camera-right axis. Moving the camera right by d in world
// == subtracting d from the View translation.x (view-space). Rebuild ViewProj = Proj*View' with the
// game's own Proj (= ViewProj*View^-1), so geometry, and any TAA jitter baked into Proj, stay exact.
// Also shifts camPos@0x80 (if present) so view-dependent effects (specular/fog) track the new eye.
static std::atomic<float> g_capM00{0.0f}, g_capM11{0.0f};   // game projection cot(hHalf)/cot(vHalf), for the XR submit FOV
// v19: headset fill. 1.0 = the game's own (very narrow) FOV; larger renders wider. NUMPAD+/- tune it live.
// [HEADVR] default 1.0: the headset fill below replaces the old fixed 2x widening.
static std::atomic<float> g_fovScale{1.0f};
// [HEADVR] head tracking + presence. The game camera becomes the player's "body": its view is the
// reference frame, the HMD pose is applied on top in view space (GFD view space = OpenXR view space:
// right-handed, X right, Y up, -Z forward — proj is w=-z, m11>0). Both eyes get the SAME head pose;
// that pose is tagged onto the submitted frame (pose-tagging), so the image is world-locked.
static std::atomic<bool>  g_headOn{true};   // restored after the headset-tested isolation run      // F11: HMD rotation drives the view
static std::atomic<bool>  g_headPosOn{false}; // Optional HMD lean; rotation-only default.
static std::atomic<bool>  g_hmdFill{true};   // BISECT A     // END: render (and declare) the headset's full FOV
// [SCREEN] a game menu is open: the camera stays the game's own flat view and the frame is shown
// on a world-locked screen quad (set per frame from GfdCam::ScreenMode()).
static std::atomic<bool>  g_screenView{false};
static std::atomic<bool>  g_srcFovOn{false};   // OFF: writing the live GFD camera = crash suspect #1 (engine-object write)
static std::atomic<int>   g_dolly{0};          // PGUP/PGDN: move the game camera along its forward axis (world units)
static constexpr float    kUnitsPerMeter = 1000.f;   // v19 estimate: near plane ~200u ~ 0.2 m
static std::atomic<float> g_fillTanH{0.f}, g_fillTanV{0.f};
static std::atomic<float> g_cbVFov{0.f};       // vertical FOV the GAME rendered (pre-fill), radians
static std::atomic<float> g_hmdVFov{0.f};      // headset vertical FOV, radians
static std::atomic<uint32_t> g_headApplied{0};
struct HeadXf { bool rot, pos; float RT[9]; float t[3]; };
// [FAKEHMD] crash bisect: exercise the head/FOV/GFD-camera paths with no headset connected.
static constexpr bool kFakeHmd = false;   // test-only; ON broke XR bring-up ordering
static std::atomic<bool> g_tagRealPos{true};   // [TAGPOS] Numpad *: tag keeps the real head position (A/B: zeroed, old)
#include "p5r_head_filter.h"
static bool BuildHeadXf(HeadXf& hx, P5RXr::Pose& used) {
    hx.rot = hx.pos = false;
    float vf = 0.f;
    // [FRAMEPOSE] 10-16: the game uploads its camera twice per frame and each upload used to fetch the newest head pose
    // separately, so one frame's renders could carry two different head poses (the one-frame tilted world). Every
    // upload in the same game frame now uses the SAME located pose; the camera trace logs serials to prove it.
    static SRWLOCK s_fpLock = SRWLOCK_INIT; static uint64_t s_fpFrame = ~0ull; static P5RXr::Pose s_fpPose{}; static float s_fpVf = 0.f; static bool s_fpOk = false;
    static std::atomic<uint32_t> s_fpReused{0}, s_fpFresh{0};
    const uint64_t fpNow = g_frame.load(std::memory_order_relaxed);
    bool fpHave = false;
    AcquireSRWLockShared(&s_fpLock); if (s_fpFrame == fpNow && s_fpOk) { used = s_fpPose; vf = s_fpVf; fpHave = true; } ReleaseSRWLockShared(&s_fpLock);
    if (fpHave) s_fpReused.fetch_add(1, std::memory_order_relaxed);
    else if (P5RXr::GetHeadPose(&used, &vf)) {
        AcquireSRWLockExclusive(&s_fpLock);
        if (s_fpFrame == fpNow && s_fpOk) { used = s_fpPose; vf = s_fpVf; } else { s_fpFrame = fpNow; s_fpPose = used; s_fpVf = vf; s_fpOk = true; }
        ReleaseSRWLockExclusive(&s_fpLock);
        fpHave = true; s_fpFresh.fetch_add(1, std::memory_order_relaxed);
    }
    if ((fpNow % 600) == 0 && fpHave) { static uint64_t s_fpLog = ~0ull; if (s_fpLog != fpNow) { s_fpLog = fpNow; Log("[framepose] f%llu one head pose per frame: fresh=%u reused=%u", (unsigned long long)fpNow, s_fpFresh.exchange(0), s_fpReused.exchange(0)); } }
    if (!fpHave) {
        if (!kFakeHmd) return false;
        // [FAKEHMD] test without a headset: identity head, 99 deg vertical FOV
        used = P5RXr::Pose{}; used.orientation[3] = 1.f; vf = 99.f * 3.14159265f / 180.f;
    }
    if (vf > 0.1f) g_hmdVFov.store(vf, std::memory_order_relaxed);
    static std::atomic<bool> rawLogged{false};
    if (!rawLogged.exchange(true)) Log("[headfilter] OFF: rejected smoothing candidate; using located quaternion unchanged");
    if (g_headOn.load(std::memory_order_relaxed)) {
        float x=used.orientation[0], y=used.orientation[1], z=used.orientation[2], w=used.orientation[3];
        float R[9] = { 1-2*(y*y+z*z), 2*(x*y-z*w),   2*(x*z+y*w),
                       2*(x*y+z*w),   1-2*(x*x+z*z), 2*(y*z-x*w),
                       2*(x*z-y*w),   2*(y*z+x*w),   1-2*(x*x+y*y) };
        for (int r=0;r<3;++r) for (int c=0;c<3;++c) hx.RT[r*3+c] = R[c*3+r];   // view-space rotation = R^T
        hx.rot = true;
    } else { used.orientation[0]=used.orientation[1]=used.orientation[2]=0.f; used.orientation[3]=1.f; }
    if (g_headPosOn.load(std::memory_order_relaxed)) {
        for (int i=0;i<3;++i) hx.t[i] = used.position[i] * CameraControls::UnitsPerMeter(CameraControls::Get());
        hx.pos = true;
    } else if (!g_tagRealPos.load(std::memory_order_relaxed)) { used.position[0]=used.position[1]=used.position[2]=0.f; }
    // [TAGPOS] rotation-only render: the camera ignores head translation, but the TAG keeps the real
    // located position. Zeroing it told the compositor "rendered from the origin" while the eyes swing
    // ~5-10 cm on the neck arc during every head turn, so it slid the image by that much: the world
    // shifted left/right as the head turned. With the real position the compositor corrects only the
    // movement within one frame (3DOF image, correctly tagged).
    return hx.rot || hx.pos;
}
// [CAMTRACE] 10-16 LOG ONLY: every main-camera upload (frame, path, thread, head pose serial+quat, original and final
// view rows). When the recorder reports a one-frame eye spike, the uploads around it are dumped to the log.
struct CamTrace { uint64_t f; uint32_t tid, serial; uint8_t path, head, deferred; float q[4]; float o[12], v[12]; };
static CamTrace g_ct[4096]; static std::atomic<uint32_t> g_ctN{0};
static bool IsDeferredContext(ID3D11DeviceContext* c);
static void CamTraceNote(ID3D11DeviceContext* c, int path, bool head, const P5RXr::Pose& used, const float* origV, const float* finalV) {
    CamTrace& t = g_ct[g_ctN.fetch_add(1, std::memory_order_relaxed) & 4095];
    t.f = g_frame.load(std::memory_order_relaxed); t.tid = GetCurrentThreadId(); t.serial = used.serial;
    t.path = (uint8_t)path; t.head = (uint8_t)head; t.deferred = (uint8_t)IsDeferredContext(c);
    memcpy(t.q, used.orientation, 16); memcpy(t.o, origV, 48); memcpy(t.v, finalV, 48);
}
// [CBCHANGE] 10-16 LOG ONLY (events): per constant buffer, when its contents last changed and the change before that.
// At a fog switch the dump lists buffers that were stable for 20+ frames and changed right at the switch, with their
// first 16 floats before/after: the fog input, whatever form it takes.
struct CbSlot { std::atomic<void*> buf{nullptr}; uint32_t hash = 0, size = 0; uint64_t last = 0, prev = 0; float before[16]{}, after[16]{}; };
static CbSlot g_cbs[8192];
static void CbChangeNote(void* buf, const void* data, UINT size) {
    if (!buf || !data || size < 16) return;
    uint32_t h = 2166136261u; const uint8_t* b = (const uint8_t*)data; const UINT n = size < 1024 ? size : 1024;
    for (UINT i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    uint32_t i = (uint32_t)(((uintptr_t)buf >> 4) * 2654435761u) & 8191; CbSlot* s = nullptr;
    for (int p = 0; p < 16 && !s; ++p, i = (i + 1) & 8191) {
        void* cur = g_cbs[i].buf.load(std::memory_order_relaxed);
        if (cur == buf) s = &g_cbs[i];
        else if (!cur) { void* e = nullptr; if (g_cbs[i].buf.compare_exchange_strong(e, buf) || e == buf) s = &g_cbs[i]; }
    }
    if (!s) return;
    if (s->hash != h) {
        const UINT c = size < 64 ? size : 64;
        memcpy(s->before, s->after, 64); memset(s->after, 0, 64); memcpy(s->after, data, c);
        s->prev = s->last; s->last = g_frame.load(std::memory_order_relaxed); s->hash = h; s->size = size;
    }
}
static void CbChangeDump(uint32_t at) {
    unsigned lines = 0;
    for (auto& s : g_cbs) {
        if (!s.buf.load(std::memory_order_relaxed) || lines >= 30) continue;
        if (s.last + 3 < at || s.last > (uint64_t)at + 1 || s.prev + 20 > s.last) continue;
        ++lines; const float* a = s.before; const float* z = s.after;
        Log("[cbchange] haze=%u buf=%p size=%u changed f%llu (stable since f%llu) before=(%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g) after=(%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g|%.3g %.3g %.3g %.3g)",
            at, s.buf.load(), s.size, (unsigned long long)s.last, (unsigned long long)s.prev,
            a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8],a[9],a[10],a[11],a[12],a[13],a[14],a[15],
            z[0],z[1],z[2],z[3],z[4],z[5],z[6],z[7],z[8],z[9],z[10],z[11],z[12],z[13],z[14],z[15]);
    }
    if (!lines) Log("[cbchange] haze=%u: no constant buffer that was stable for 20+ frames changed at the switch", at);
}
// [SHOTHOLD] 10-16 candidate, ON: cutscenes give each game camera shot its own look settings (glow/exposure/fog)
// in the main camera buffer. In VR the view does not cut, so those switches read as fog/glow flicker. During an event,
// a non-matrix field that was steady 20+ frames and changes exactly at a game camera cut keeps its pre-cut value;
// a field that also changes between cuts is animated and is released. Every hold/release is logged.
static void ShotHold(const float* orig, float* L, float* R, UINT bytes) {
    constexpr UINT kMax = 4096, kFirst = 52;   // 0..51 = VP, V, camera position, history VP (already handled)
    static float last[kMax], hold[kMax]; static uint64_t since[kMax]; static bool held[kMax];
    static float lastV[12]; static float lastDelta = 0.f; static bool have = false; static UINT haveN = 0;
    static unsigned logs = 0; static std::atomic<uint32_t> heldNow{0};
    const UINT n = (std::min)(bytes / 4, kMax);
    const uint64_t f = g_frame.load(std::memory_order_relaxed);
    if (!GfdCam::EventRunning() || n <= kFirst) { if (have && logs < 400) { ++logs; Log("[shothold] event ended: %u held fields released", heldNow.load()); } have = false; heldNow = 0; return; }
    if (!have || haveN != n) {
        memcpy(last, orig, n * 4); for (UINT i = 0; i < n; ++i) { since[i] = f; held[i] = false; }
        memcpy(lastV, orig + 16, 48); lastDelta = 0.f; have = true; haveN = n; heldNow = 0; return;
    }
    float delta = 0.f; for (int i = 0; i < 12; ++i) delta = (std::max)(delta, fabsf(orig[16+i] - lastV[i]));
    const bool cut = delta > 0.05f && delta > 5.f * lastDelta;
    lastDelta = cut ? 0.f : delta; memcpy(lastV, orig + 16, 48);
    for (UINT i = kFirst; i < n; ++i) {
        const float v = orig[i];
        if (v != last[i]) {
            if (cut && !held[i] && since[i] + 20 <= f) {
                held[i] = true; hold[i] = last[i]; heldNow.fetch_add(1);
                if (logs < 400) { ++logs; Log("[shothold] f%llu game camera cut: holding field #%u (byte %u) at %g (shot wanted %g)", (unsigned long long)f, i, i * 4, hold[i], v); }
            } else if (!cut && held[i]) {
                held[i] = false; heldNow.fetch_sub(1);
                if (logs < 400) { ++logs; Log("[shothold] f%llu field #%u changed between cuts (animated): released, now %g", (unsigned long long)f, i, v); }
            }
            last[i] = v; since[i] = f;
        }
        if (held[i]) { L[i] = hold[i]; R[i] = hold[i]; }
    }
}
static void CamTraceDump(uint32_t spike) {
    const uint32_t n = g_ctN.load(std::memory_order_relaxed); const uint32_t from = n > 4096 ? n - 4096 : 0; unsigned lines = 0;
    for (uint32_t i = from; i < n && lines < 60; ++i) {
        const CamTrace& t = g_ct[i & 4095];
        if (t.f + 8 < spike || t.f > (uint64_t)spike + 2) continue; ++lines;
        Log("[camtrace] spike=%u f=%llu #%u tid=%u %s path=%d head=%d serial=%u q=(%.4f,%.4f,%.4f,%.4f) O=(%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f) V=(%.3f,%.3f,%.3f|%.3f,%.3f,%.3f|%.3f,%.3f,%.3f)",
            spike, (unsigned long long)t.f, i, t.tid, t.deferred ? "def" : "imm", t.path, t.head, t.serial, t.q[0], t.q[1], t.q[2], t.q[3],
            t.o[0], t.o[1], t.o[2], t.o[4], t.o[5], t.o[6], t.o[8], t.o[9], t.o[10], t.v[0], t.v[1], t.v[2], t.v[4], t.v[5], t.v[6], t.v[8], t.v[9], t.v[10]);
    }
}
static void InjectEyeShift(float* f, float d, const HeadXf* hx = nullptr, P5RXr::Fov* lens = nullptr) {
    float* VP = f;          // 0x00 ViewProjection
    float* V  = f + 16;     // 0x40 View
    float Vinv[16], Proj[16], Vp[16], VPp[16];
    if (!Mat4Inverse(V, Vinv)) return;
    Mat4Mul(VP, Vinv, Proj);
    // [HEADVR] what the GAME rendered (before our widening) — feeds the source-camera validator
    if (Proj[5] > 0.01f) g_cbVFov.store(2.f * atanf(1.f / Proj[5]), std::memory_order_relaxed);
    if (g_screenView.load(std::memory_order_relaxed)) return;   // [SCREEN] both eyes = the game's own camera
    // Cover both eyes' full asymmetric bounds; keep render and submitted FOV matched.
    if (g_hmdFill.load(std::memory_order_relaxed)) {
        if (CameraControls::exactFill.load(std::memory_order_relaxed)) CameraControls::FillProjectionExact(Proj, g_fillTanH.load(), g_fillTanV.load());
        else CameraControls::FillProjection(Proj, g_fillTanH.load(), g_fillTanV.load());
    }
    // [HEADVR] camera dolly + head pose, applied to the view BEFORE the eye shift:
    //   V' = R_head^T * T(-headPos) * T(dolly fwd) * V
    {
        const bool fieldView=GfdCam::AllowManual();
        float dl = (float)g_dolly.load(std::memory_order_relaxed);
        if (dl != 0.f) {
            V[11] += dl;                                     // view-space: camera moved forward = points +z
            float* cp = f + 32;
            if (fabsf(cp[3]-1.f) < 0.001f && (fabsf(cp[0])>1.f || fabsf(cp[2])>1.f)) {
                cp[0] -= dl*V[8]; cp[1] -= dl*V[9]; cp[2] -= dl*V[10];   // forward(world) = -row2
            }
        }
        // Offset both eyes in the game camera's local axes, then apply HMD pose.
        const auto manual = CameraControls::Get();
        float fpEye[3], evEye[3], evAt[3]; bool evHasAt = false;
        const bool evWanted = GfdCam::ConfidantRunning() ? manual.eventFirstPerson : manual.cutsceneFirstPerson;
        if (evWanted && GfdCam::EventEye(evEye, evAt, evHasAt) && !evHasAt) {
            // [CUTFP] no one to face and Joker's facing not learned yet: the cutscene camera's direction, levelled
            float fw[3] = { -V[8], 0.f, -V[10] }; const float fl = sqrtf(fw[0]*fw[0] + fw[2]*fw[2]);
            if (fl > 1e-3f) { fw[0] /= fl; fw[2] /= fl;
                const float rt[3] = { -fw[2], 0.f, fw[0] }; const float up[3] = { 0.f, 1.f, 0.f };
                for (int c = 0; c < 3; ++c) { V[c] = rt[c]; V[4+c] = up[c]; V[8+c] = -fw[c]; } }
            for (int j = 0; j < 3; ++j) evEye[j] += 12.f * (j == 1 ? 0.f : fw[j]);
            for (int r = 0; r < 3; ++r) V[r*4+3] = -(V[r*4]*evEye[0] + V[r*4+1]*evEye[1] + V[r*4+2]*evEye[2]);
            float* cp = f + 32;
            if (fabsf(cp[3]-1.f) < 0.001f) { cp[0] = evEye[0]; cp[1] = evEye[1]; cp[2] = evEye[2]; }
            g_evFpUses.fetch_add(1, std::memory_order_relaxed);
        } else if (evWanted && evHasAt) {
            // [EVENTFP] view from Joker's eye toward the person he is talking to (head tracking adds on top).
            // P5R view rows: right, up, back (-forward); right-handed, Y up.
            float fw[3] = { evAt[0] - evEye[0], evAt[1] - evEye[1], evAt[2] - evEye[2] };
            float fl = sqrtf(fw[0]*fw[0] + fw[1]*fw[1] + fw[2]*fw[2]); for (float& v : fw) v /= fl;
            float rt[3] = { -fw[2], 0.f, fw[0] }; float rl = sqrtf(rt[0]*rt[0] + rt[2]*rt[2]); rt[0] /= rl; rt[2] /= rl;   // forward x worldUp
            float up[3] = { rt[1]*fw[2] - rt[2]*fw[1], rt[2]*fw[0] - rt[0]*fw[2], rt[0]*fw[1] - rt[1]*fw[0] };          // right x forward
            const float* rows[3] = { rt, up, fw };
            for (int r = 0; r < 3; ++r) { const float sgn = r == 2 ? -1.f : 1.f;
                for (int c = 0; c < 3; ++c) V[r*4+c] = sgn * rows[r][c];
                V[r*4+3] = -(V[r*4]*evEye[0] + V[r*4+1]*evEye[1] + V[r*4+2]*evEye[2]); }
            float* cp = f + 32;
            if (fabsf(cp[3]-1.f) < 0.001f) { cp[0] = evEye[0]; cp[1] = evEye[1]; cp[2] = evEye[2]; }
            g_evFpUses.fetch_add(1, std::memory_order_relaxed);
        } else if (CameraControls::UseManual(manual) && fieldView && GfdCam::FirstPersonEye(fpEye)) {
            // [FPANCHOR] eye at Joker; the game camera supplies only the direction (right stick turns
            // the view on the spot). Saved Up is a height tweak; Forward/Right are the old orbit
            // calibration and do not apply here. Yaw/pitch tweaks still rotate.
            auto rotOnly = manual; rotOnly.right = 0; rotOnly.forward = 0; rotOnly.up = 0;
            CameraControls::Apply(V, rotOnly);
            const float P[3] = { fpEye[0], fpEye[1] + manual.up, fpEye[2] };
            for (int r = 0; r < 3; ++r) V[r*4+3] = -(V[r*4]*P[0] + V[r*4+1]*P[1] + V[r*4+2]*P[2]);
            float* cp = f + 32;
            if (fabsf(cp[3]-1.f) < 0.001f) { cp[0] = P[0]; cp[1] = P[1]; cp[2] = P[2]; }
            g_fpAnchorUses.fetch_add(1, std::memory_order_relaxed);
        } else if (CameraControls::UseManual(manual) && fieldView) {
            float* cp = f + 32;
            if (fabsf(cp[3]-1.f) < 0.001f) {
                for (int j=0;j<3;++j)
                    cp[j] += manual.right*V[j] + manual.up*V[4+j] - manual.forward*V[8+j];
            }
            CameraControls::Apply(V, manual);
            g_fpManualUses.fetch_add(1, std::memory_order_relaxed);
        }
        if (hx && hx->pos) { V[3] -= hx->t[0]; V[7] -= hx->t[1]; V[11] -= hx->t[2]; }
        if (hx && hx->rot) {
            float Vr[12];
            for (int r=0;r<3;++r) for (int c=0;c<4;++c)
                Vr[r*4+c] = hx->RT[r*3+0]*V[0*4+c] + hx->RT[r*3+1]*V[1*4+c] + hx->RT[r*3+2]*V[2*4+c];
            memcpy(V, Vr, sizeof(Vr));
        }
    }
    // v19 FOV: P5R renders a telephoto view — the recovered projection is m00~6.4 (cot(hHalf)),
    // i.e. roughly 18 degrees horizontal, so in a ~100-degree headset it is a small floating
    // window. Dividing m00/m11 by s widens the ACTUAL rendered frustum by ~s, and the same
    // widened values are handed to OpenXR below, so the declared FOV matches what was drawn
    // (no angular stretch, geometry stays correct). Cost: the game culls to its own narrower
    // frustum, so very large values can pop objects at the edges — hence it is tunable live.
    {
        float s = g_fovScale.load(std::memory_order_relaxed);
        if (s > 1.001f || s < 0.999f) { Proj[0] /= s; Proj[5] /= s; }
    }
    g_capM00.store(Proj[0], std::memory_order_relaxed);   // row-major m00=cot(hHalf), m11=cot(vHalf)
    g_capM11.store(Proj[5], std::memory_order_relaxed);
    CameraControls::Converge(Proj, d, CameraControls::Get());
    // Full lens, including the game's off-axis/jitter terms and per-eye convergence.
    // clip.w=-z: at NDC +/-1 the view ray is (P02 +/-1)/P00 (and similarly for y).
    if (lens) *lens = {atanf((Proj[2]-1.f)/Proj[0]), atanf((Proj[2]+1.f)/Proj[0]),
                       atanf((Proj[6]+1.f)/Proj[5]), atanf((Proj[6]-1.f)/Proj[5])};
    memcpy(Vp, V, 16*sizeof(float));
    Vp[3] -= d;                              // View': translation.x -= d
    Mat4Mul(Proj, Vp, VPp);                  // ViewProj' = Proj * View'
    {   // [PRECISECULL] publish this eye's rendered frustum, widened for head-motion latency
        float wide[16], cull[16]; memcpy(wide, Proj, sizeof(wide));
        wide[0] /= P5RCulling::kMargin; wide[5] /= P5RCulling::kMargin;
        Mat4Mul(wide, Vp, cull);
        float eye[3]; for (int j = 0; j < 3; ++j) eye[j] = -(Vp[j]*Vp[3] + Vp[4+j]*Vp[7] + Vp[8+j]*Vp[11]);   // -R^T t
        P5RCulling::StoreEyeVP(d < 0 ? 0 : (d > 0 ? 1 : 2), cull, eye);
    }
    memcpy(VP, VPp, 16*sizeof(float));
    memcpy(V,  Vp,  16*sizeof(float));
    // Keep shader camera position consistent with the final per-eye view.
    // For rigid V=[R,t], camera position is -transpose(R)*t. This includes
    // manual offsets, HMD lean/rotation and eye separation, even at world origin.
    float* cp = f + 32;
    if (fabsf(cp[3]-1.f) < 0.001f) {
        for(int j=0;j<3;++j)cp[j]=-(V[j]*V[3]+V[4+j]*V[7]+V[8+j]*V[11]);
    }
}
static std::atomic<bool> g_injectOn{false};
static std::atomic<bool> g_swayOn{false};
static std::atomic<int>  g_shiftUnits{100};   // sway-test eye-shift magnitude in world units (F3/F4)
// v19: P5R's world is ~1000 units/metre (recovered near plane ~200 units ~ 0.2 m), so the old
// default of 3 was ~3 mm of eye separation - effectively mono, which is why the depth read as
// "really weak". A human IPD (~63 mm) is ~63 units, i.e. ~32 per eye. F3/F4 now scale by 1.4x
// per press so a couple of presses sweep the whole useful range instead of crawling by 1.
static std::atomic<int>  g_stereoSep{32};     // per-eye stereo half-separation in world units (F3/F4 when dup on)
// v17: CROSS-EYED disparity was reported in the first live-headset session (2026-08-31), so the
// default submits the twin as the LEFT view and the backbuffer as the RIGHT. F7 toggles at runtime
// so one in-headset press settles the correct order for good — the log records the winner.
static std::atomic<bool> g_swapEyes{false};   // [ECL2] true left/right eyes now; F7 still flips
static std::atomic<uint32_t> g_cInject{0};    // main-camera CB rewrites this frame (feedback it's hitting)
static uint32_t g_lInject = 0;

static std::atomic<bool> g_capturing{false};
static volatile bool     g_captureRequest = false;
static std::mutex        g_capMx;
static std::vector<CapEvt> g_capture;
static const size_t      kCaptureMax = 30000;
static bool g_autoCaptureDone = false;
static bool g_autoPending = false;

static void CapPush(const CapEvt& e) {
    std::lock_guard<std::mutex> lk(g_capMx);
    if (g_capture.size() < kCaptureMax) g_capture.push_back(e);
}
static void CapDraw(UINT idxCount) { CapEvt e{}; e.type=1; e.size=idxCount; CapPush(e); }
static void CapDispatch()          { CapEvt e{}; e.type=2; CapPush(e); }
static void CapUpload(void* dst, UINT size, const float* f) {
    CapEvt e{}; e.type=0; e.a=dst; e.size=size;
    UINT nBlocks = size / 64; UINT found=0;
    for (UINT b = 0; b < nBlocks; ++b) {
        char c = ClassifyBlock(f + b*16);
        if (c=='Z'||c=='J'||c=='I'||c=='D') continue;
        ++found;
        if (e.nMat < 4) { CapMat& m = e.mats[e.nMat++]; m.off64=(uint16_t)b; m.cls=c;
            memcpy(m.r0, f+b*16, 16); memcpy(m.r3, f+b*16+12, 16); }
    }
    e.aux = found;
    CapPush(e);
    // if this upload looks like the camera CB, keep a full dump of its first 8 blocks
    if (IsCameraSig(f, size)) {
        std::lock_guard<std::mutex> lk(g_capMx);
        if (g_camDumps.size() < kCamDumpMax) {
            CamDump d{}; d.dst=dst; d.size=size;
            UINT n = size/4; if (n > 128) n = 128;
            memcpy(d.f, f, n*4);
            g_camDumps.push_back(d);
        }
    }
}
static void CapRT(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv) {
    CapEvt e{}; e.type=3; e.a=rtv; e.b=dsv; e.fmt=DXGI_FORMAT_UNKNOWN;
    if (rtv) {
        ID3D11Resource* res=nullptr; rtv->GetResource(&res);
        if (res) { ID3D11Texture2D* tex=nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&tex)) && tex) {
                D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
                e.size=td.Width; e.aux=td.Height; e.fmt=td.Format; tex->Release();
            }
            res->Release();
        }
    }
    CapPush(e);
}

// ------------------------------------------------------------------ v4: main-camera correlation
// The camera CB packs MULTIPLE cameras' matrices (proven: viewproj@0x00 and view@0x40 had
// different forward axes + the eye didn't project to w~0). To recover the MAIN camera's matched
// view+viewproj (and thus projection/FOV), tie the CB to the main scene render target: the
// 2400x1350 (125%-scale) HDR buffer is the only RTV wider than the 1920 backbuffer, so
// "bound RTV width >= 2000" == main scene pass. During a capture frame we cache CB payloads by
// pointer and record which CBs are bound while that pass is active, then dump their cached bytes.
static std::atomic<bool> g_inMainPass{false};
static std::atomic<uint32_t> g_mainPassW{0}, g_mainPassH{0};

// ---- render-driver stack finder (F6) -------------------------------------------------------
// One-shot: capture the call chain at the start of the main scene pass (RT bind) and at its first
// DrawIndexed. The P5R.exe return addresses in those chains are GFD's per-view render driver — the
// function we hook and invoke twice for same-frame stereo. Symbolized as module+offset.
extern "C" __declspec(dllimport) USHORT __stdcall RtlCaptureStackBackTrace(ULONG, ULONG, PVOID*, PULONG);
static std::atomic<bool> g_stackArm{false};
// three one-shot correlation points per arm: the main-camera-CB upload, the scene RT bind, and the
// first scene draw. The function ENTRY common to all three = GFD's per-view render fn (wraps camera
// setup + RT + draws) — the hookable target we invoke twice for same-frame stereo.
static volatile bool g_didCamStack = false, g_didRtStack = false, g_didDrawStack = false;
static void LogStack(const char* where) {
    void* frames[32] = {};
    USHORT nn = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    Log("---- RENDER-DRIVER STACK @ %s (%u frames)  [ret -> fn-entry] ----", where, (unsigned)nn);
    for (USHORT i = 0; i < nn; ++i) {
        char rm[192]; ModOf(frames[i], rm, sizeof(rm));
        DWORD64 imgBase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)frames[i], &imgBase, nullptr);
        if (rf && imgBase) { char fm[192]; ModOf((void*)(imgBase + rf->BeginAddress), fm, sizeof(fm));
            Log("   [%2u] %-22s -> %s", i, rm, fm); }
        else Log("   [%2u] %-22s -> ?", i, rm);
    }
    Log("---- END STACK ----");
}

// ---- OT-flush instrumentation (NUMPAD7) -----------------------------------------------------
// SAFE read-only hook of GFD's render-list executor (+0x1F57E0, from the F6 chain) and its caller
// (+0x1E13CD): bracket the draw count per call to learn the frame's render structure (how many
// flushes/frame, draws each) BEFORE we invoke it twice for stereo. 8-arg forward = correct for any
// MS x64 ABI fn with <=8 args; forwarding extra args a callee ignores is harmless (too-few is not).
typedef uint64_t (*PFN_GameFn)(void*,void*,void*,void*,void*,void*,void*,void*);
static std::atomic<bool> g_installGameHooks{false};
static bool g_gameHooksInstalled = false;
static PFN_GameFn g_origOTFlush=nullptr, g_origOTFlushParent=nullptr, g_origOTFlush2=nullptr;   // +0x1F57E0 / +0x1E13CD / +0x1F4B32
static std::atomic<uint32_t> g_otCalls{0}, g_otParentCalls{0}, g_otFlush2Calls{0};
// always-on main-scene-RT bind counter: rtv width>=2000 == the 2400x1350 scene target. Cache the
// scene RTV pointers so after warmup it's a pointer-compare (GetDesc only for an unseen RTV).
static std::atomic<uint32_t> g_cSceneRTBind{0};
static std::mutex g_srtMx;
static std::unordered_set<void*> g_sceneRTVset;
static std::atomic<int> g_otSampleE13{0}, g_otSampleF4b{0};   // >0 = log next N per-call detail lines
static uint64_t HookOTFlush(void* a0,void* a1,void* a2,void* a3,void* a4,void* a5,void* a6,void* a7) {   // +0x1F57E0 per-command
    uint64_t r = g_origOTFlush(a0,a1,a2,a3,a4,a5,a6,a7);
    g_otCalls.fetch_add(1, std::memory_order_relaxed);
    return r;
}
static uint64_t HookOTFlushParent(void* a0,void* a1,void* a2,void* a3,void* a4,void* a5,void* a6,void* a7) {   // +0x1E13CD render-list (2x/frame)
    uint32_t d0=g_cDrawIdx.load(std::memory_order_relaxed);
    uint64_t r = g_origOTFlushParent(a0,a1,a2,a3,a4,a5,a6,a7);
    uint32_t dd=g_cDrawIdx.load(std::memory_order_relaxed)-d0;
    g_otParentCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_otSampleE13.load(std::memory_order_relaxed) > 0) { g_otSampleE13.fetch_sub(1, std::memory_order_relaxed);
        Log("   [e13 +0x1E13CD] draws=%u", dd); }   // draw count per call -> scene call = the big one (~1400)
    return r;
}
static uint64_t HookOTFlush2(void* a0,void* a1,void* a2,void* a3,void* a4,void* a5,void* a6,void* a7) {   // +0x1F4B32 (parent of +0x1E13CD)
    uint32_t d0=g_cDrawIdx.load(std::memory_order_relaxed), s0=g_cSceneRTBind.load(std::memory_order_relaxed);
    uint64_t r = g_origOTFlush2(a0,a1,a2,a3,a4,a5,a6,a7);
    uint32_t dd=g_cDrawIdx.load(std::memory_order_relaxed)-d0, ds=g_cSceneRTBind.load(std::memory_order_relaxed)-s0;
    g_otFlush2Calls.fetch_add(1, std::memory_order_relaxed);
    if (g_otSampleF4b.load(std::memory_order_relaxed) > 0) { g_otSampleF4b.fetch_sub(1, std::memory_order_relaxed);
        Log("   [f4b +0x1F4B32] draws=%u sceneRTbinds=%u", dd, ds); }
    return r;
}
static std::mutex g_cbcMx;
static std::unordered_map<void*, std::array<float,64>> g_cbCache;      // buf ptr -> first 256 bytes
struct BoundCB { void* buf; uint8_t stage; uint8_t slot; };            // stage 0=VS 1=PS
static std::vector<BoundCB> g_mainBound;
static std::unordered_set<void*> g_mainBoundSeen;
static const size_t kMainBoundMax = 64;

static void CacheCB(void* buf, UINT size, const float* f) {
    if (!g_capturing.load(std::memory_order_relaxed)) return;
    UINT n = size/4; if (n > 64) n = 64;
    std::lock_guard<std::mutex> lk(g_cbcMx);
    auto& arr = g_cbCache[buf];
    memcpy(arr.data(), f, n*4);
}
static void RecordBound(UINT startSlot, UINT num, ID3D11Buffer* const* bufs, uint8_t stage) {
    if (!g_capturing.load(std::memory_order_relaxed) || !g_inMainPass.load(std::memory_order_relaxed)) return;
    if (!bufs) return;
    std::lock_guard<std::mutex> lk(g_cbcMx);
    for (UINT i=0;i<num;++i) {
        if (!bufs[i]) continue;
        if (g_mainBound.size() >= kMainBoundMax) return;
        if (g_mainBoundSeen.insert(bufs[i]).second)
            g_mainBound.push_back(BoundCB{bufs[i], stage, (uint8_t)(startSlot+i)});
    }
}
// v5: the camera CB is bound ONCE before the pass and left bound, so RecordBound (re-binds only)
// misses it. On the first draw inside the main pass, read the CURRENTLY-bound VS+PS CBs directly.
static std::atomic<bool> g_mainSnapDone{false};
static void SnapshotMainCam(ID3D11DeviceContext* c) {
    if (!g_capturing.load(std::memory_order_relaxed) || !g_inMainPass.load(std::memory_order_relaxed)) return;
    if (g_mainSnapDone.exchange(true)) return;               // once per capture frame
    ID3D11Buffer* vs[14]={}; ID3D11Buffer* ps[14]={};
    c->VSGetConstantBuffers(0,14,vs);
    c->PSGetConstantBuffers(0,14,ps);
    { std::lock_guard<std::mutex> lk(g_cbcMx);
      for (int i=0;i<14;++i) if (vs[i] && g_mainBound.size()<kMainBoundMax && g_mainBoundSeen.insert(vs[i]).second)
          g_mainBound.push_back(BoundCB{vs[i],0,(uint8_t)i});
      for (int i=0;i<14;++i) if (ps[i] && g_mainBound.size()<kMainBoundMax && g_mainBoundSeen.insert(ps[i]).second)
          g_mainBound.push_back(BoundCB{ps[i],1,(uint8_t)i}); }
    for (int i=0;i<14;++i){ if(vs[i]) vs[i]->Release(); if(ps[i]) ps[i]->Release(); }
}

static size_t DumpForensicCapture() {
    std::vector<CapEvt> snap;
    { std::lock_guard<std::mutex> lk(g_capMx); snap.swap(g_capture); }
    Log("==== FORENSIC CAPTURE: %zu events this frame ====", snap.size());
    if (snap.empty()) { Log("==== END FORENSIC (empty) ===="); return 0; }

    // pass structure: segments between RT changes
    Log("-- pass structure (RT segments) --");
    int seg=0; UINT segDraws=0, segDisp=0, segUps=0; const CapEvt* segRt=nullptr;
    auto flushSeg = [&](void) {
        if (!segRt && !segDraws && !segUps && !segDisp) return;
        if (segRt)
            Log("[seg %02d] rtv=%p dsv=%p %ux%u fmt=%d | draws=%u disp=%u cbUploads=%u",
                seg, segRt->a, segRt->b, segRt->size, segRt->aux, (int)segRt->fmt, segDraws, segDisp, segUps);
        else
            Log("[seg %02d] (pre-first-RT) draws=%u disp=%u cbUploads=%u", seg, segDraws, segDisp, segUps);
        ++seg;
    };
    for (const CapEvt& e : snap) {
        if (e.type==3) { flushSeg(); segRt=&e; segDraws=segDisp=segUps=0; }
        else if (e.type==1) ++segDraws;
        else if (e.type==2) ++segDisp;
        else if (e.type==0) ++segUps;
    }
    flushSeg();

    // matrix map: aggregate uploads by (size, off, cls)
    Log("-- matrix map (this frame) --");
    struct Agg { uint32_t count; CapMat sample; void* sampleDst; };
    std::map<MatKey, Agg> agg;
    uint32_t totUploads=0;
    for (const CapEvt& e : snap) {
        if (e.type!=0) continue;
        ++totUploads;
        for (int i=0;i<e.nMat;++i) {
            MatKey k{e.size, (UINT)e.mats[i].off64*64, e.mats[i].cls};
            auto it = agg.find(k);
            if (it==agg.end()) agg[k] = Agg{1, e.mats[i], e.a};
            else it->second.count++;
        }
    }
    Log("[mat] cb-uploads=%u  distinct (size,off,class) keys=%zu", totUploads, agg.size());
    // perspectives first (the projection / viewproj candidates), then affine/rot by count
    for (int pass=0; pass<2; ++pass) {
        std::vector<std::pair<uint32_t,MatKey>> order;
        for (auto& kv : agg) {
            bool isP = kv.first.cls=='P';
            if ((pass==0) != isP) continue;
            order.push_back({kv.second.count, kv.first});
        }
        std::sort(order.begin(), order.end(), [](auto&a, auto&b){ return a.first>b.first; });
        int shown=0;
        for (auto& o : order) {
            if (shown++ >= (pass==0?20:25)) { Log("      ... (%zu more)", order.size()-(size_t)shown+1); break; }
            const Agg& a = agg[o.second];
            Log("[%c] size=%5u off=0x%04X n=%u dst=%p r0=% .4f % .4f % .4f % .4f | r3=% .4f % .4f % .4f % .4f",
                o.second.cls, o.second.size, o.second.off, o.first, a.sampleDst,
                a.sample.r0[0],a.sample.r0[1],a.sample.r0[2],a.sample.r0[3],
                a.sample.r3[0],a.sample.r3[1],a.sample.r3[2],a.sample.r3[3]);
        }
    }
    // full camera-CB dumps: every block 0x000..0x1C0 with all 16 floats, classified.
    // This is where the projection / view-projection (dense) blocks finally print in full.
    std::vector<CamDump> cams;
    { std::lock_guard<std::mutex> lk(g_capMx); cams.swap(g_camDumps); }
    Log("-- full camera-CB dumps: %zu --", cams.size());
    for (size_t ci=0; ci<cams.size(); ++ci) {
        const CamDump& d = cams[ci];
        Log("[camCB %zu] dst=%p size=%u", ci, d.dst, d.size);
        for (int blk=0; blk<8; ++blk) {
            const float* m = d.f + blk*16;
            char c = ClassifyBlock(m);
            Log("  off=0x%04X [%c] % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f",
                blk*64, c, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
        }
    }
    // v4: main-camera CBs — the constant buffers bound while the 2400x1350 scene RT was active.
    // Their cached payloads hold the MATCHED main-camera matrices (same camera => projection
    // recovers cleanly as viewproj*view^-1). Dump first 4 blocks of each, classified.
    std::vector<BoundCB> bound; std::unordered_map<void*, std::array<float,64>> cache;
    { std::lock_guard<std::mutex> lk(g_cbcMx); bound = g_mainBound; cache = g_cbCache; }
    Log("-- main-camera CBs: %zu bound during scene pass (%ux%u) --",
        bound.size(), g_mainPassW.load(), g_mainPassH.load());
    for (const BoundCB& bc : bound) {
        auto it = cache.find(bc.buf);
        if (it == cache.end()) { Log("[mainCB %s b%u] buf=%p (no cached upload this frame)", bc.stage?"PS":"VS", bc.slot, bc.buf); continue; }
        const float* f = it->second.data();
        // only bother printing buffers that contain at least one non-trivial matrix block
        bool interesting=false; for (int blk=0;blk<4;++blk){ char c=ClassifyBlock(f+blk*16); if(c!='Z'&&c!='J'&&c!='I'){interesting=true;break;} }
        if (!interesting) continue;
        bool camSig = IsCameraSig(f, 256);   // affine + large translation in block 0/1 == THE camera CB
        Log("[mainCB %s b%u] buf=%p%s", bc.stage?"PS":"VS", bc.slot, bc.buf, camSig ? "   <<< CAMERA SIGNATURE" : "");
        for (int blk=0; blk<4; ++blk) {
            const float* m = f + blk*16; char c = ClassifyBlock(m);
            Log("  off=0x%04X [%c] % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f / % .4f % .4f % .4f % .4f",
                blk*64, c, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
        }
    }
    Log("==== END FORENSIC ====");
    return snap.size();
}

// ------------------------------------------------------------------ context hooks
// stereo draw-duplication state used across several hooks; resource creation + duplication bodies are
// defined below (before HookLiveContext). Declared here so the earlier hooks can reference them.
// Title-freeze discriminator: keep OpenXR/session/swapchains live, but never create or use the
// duplicated right-eye resources. If the title transition still faults, the persistent XR
// resources on the game device are sufficient; if it survives, the dup twin/bind state is causal.
// Deliberately compile-time and loud in the log so this one-variable test cannot be misread.
static constexpr bool kTitleFreezeXrOnlyProbe = false;
static std::atomic<bool> g_titleTransitionComplete{false};
static std::atomic<uint64_t> g_titleResizeFirstFrame{0};
static std::atomic<uint64_t> g_stereoArmAfter{~0ull};
static std::atomic<bool> g_dupOn{false};        // duplicate scene draws to the right eye
static std::atomic<bool> g_inScenePass{false};  // a >=2000-wide (scene) RT is currently bound (ANY context)
// v18 [CTXSCENE]: "is the scene RT bound" is PER-CONTEXT state, never global. P5R records through
// several deferred contexts on worker threads at once; with one global flag, worker B's UI/movie
// draws were duplicated (and its RT/CB stage save-restored) because worker A had the scene RT
// bound. That writes our bindings into an unrelated command list. Same class of bug as ME3's
// [CTXRTV] — the third lane to pay for a cross-thread bound-RT global.
struct CtxScene { std::atomic<ID3D11DeviceContext*> ctx{nullptr}; std::atomic<bool> inScene{false};
                  std::atomic<bool> inUI{false};   // v19: backbuffer-sized pass = final composite + UI
                  std::atomic<int> rtSlot{-1};     // v22: census slot of this context's bound RT
                  std::atomic<uint32_t> w{0}, h{0};
                  std::atomic<uint32_t> rtW{0}, rtH{0}, rtFmt{0}; std::atomic<bool> rtDsv{false}; std::atomic<void*> rtTex{nullptr}; };   // [HUD] bound RT0 + depth
static CtxScene g_ctxScene[16];
// v18: the scene pass renders at 2400x1350 while the twin is backbuffer-sized (1920x1080) for the
// XR CopyResource. Recording the scene size lets the dup path set a matching viewport (see
// DuplicateDrawIndexed) instead of inheriting the game's oversized one.
static std::atomic<uint32_t> g_scenePassW{0}, g_scenePassH{0};
static std::atomic<bool> g_twinWanted{false};   // render thread builds the twin; hooks never allocate
static std::atomic<uint32_t> g_camCBWanted{0};  // ditto for the right-eye camera constant buffer
static std::atomic<uint64_t> g_xrRebuildAfter{0};  // v21: frame at which to rebuild XR eye swapchains
// v22 [RTCENSUS]: find P5R's real UI target. The backbuffer-sized pass turned out to be a single
// 4-index composite quad, so the interface is drawn somewhere else. This census records, for every
// render target bound during a frame, its size/format and how many draws landed in it, in bind
// order — so a frame with the HUD up names the UI's own target outright. Counters are approximate
// by design (worker threads, relaxed atomics); we need the SHAPE, not exact totals.
struct RtStat {
    std::atomic<void*> rtv{nullptr};
    std::atomic<uint32_t> w{0}, h{0}, fmt{0}, draws{0}, seq{0};
};
static RtStat g_rtStat[48];
static std::atomic<uint32_t> g_rtSeq{0};
static std::atomic<bool> g_rtCensusOn{false};   // perf: diagnostic, off (table saturated, data unused)
static int RtStatSlot(void* rtv, uint32_t w, uint32_t h, uint32_t fmt) {
    for (int i = 0; i < 48; ++i) if (g_rtStat[i].rtv.load(std::memory_order_acquire) == rtv) return i;
    for (int i = 0; i < 48; ++i) {
        void* expect = nullptr;
        if (g_rtStat[i].rtv.compare_exchange_strong(expect, rtv, std::memory_order_acq_rel)) {
            g_rtStat[i].w.store(w, std::memory_order_relaxed); g_rtStat[i].h.store(h, std::memory_order_relaxed);
            g_rtStat[i].fmt.store(fmt, std::memory_order_relaxed);
            g_rtStat[i].seq.store(g_rtSeq.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
            return i;
        }
    }
    return -1;
}
// v20: the render thread can release the twin (resize/load teardown) while a worker thread is
// mid-duplication — a use-after-free window that is wide open at exactly the scene transitions
// where every crash so far has landed. Workers hold this shared; teardown takes it exclusive.
static SRWLOCK g_dupLock = SRWLOCK_INIT;
static UINT g_bbW=0, g_bbH=0;                  // backbuffer (= twin, = eye) size, known at first frame
static DXGI_FORMAT g_bbFmt=DXGI_FORMAT_UNKNOWN; // v19: identifies the UI/composite pass by target
static CtxScene* CtxSceneSlot(ID3D11DeviceContext* c) {
    if (!c) return nullptr;
    for (auto& s : g_ctxScene) if (s.ctx.load(std::memory_order_acquire) == c) return &s;
    for (auto& s : g_ctxScene) {
        ID3D11DeviceContext* expect = nullptr;
        if (s.ctx.compare_exchange_strong(expect, c, std::memory_order_acq_rel)) return &s;
    }
    return nullptr;   // >16 contexts: this one never duplicates (correct default, no corruption)
}
static bool CtxInScene(ID3D11DeviceContext* c) {
    CtxScene* s = CtxSceneSlot(c);
    return s && s->inScene.load(std::memory_order_relaxed);
}
static bool CtxInUI(ID3D11DeviceContext* c) {
    CtxScene* s = CtxSceneSlot(c);
    return s && s->inUI.load(std::memory_order_relaxed);
}
// v19: the twin holds the raw scene only, so whichever eye it feeds had NO interface (symptom:
// "left eye has no UI but right eye does" — the left eye is the twin under the v17 eye swap).
// P5R draws its UI into the backbuffer-sized target after the scene, so those draws are
// re-issued into the twin as well, with the game's own constant buffers left alone (UI must land
// at the same screen position in both eyes). Only DrawIndexed is duplicated, and the fullscreen
// composite/tonemap passes are non-indexed Draw calls, so the scene image is not overwritten.
// v20: DEFAULT OFF. v19's own counter said uiDup = exactly 1 draw/frame — the UI has far more
// elements than that, so what we were duplicating is almost certainly P5R's fullscreen COMPOSITE
// blit. Copying that into the twin paints the left eye's finished image over the right eye's scene
// and flattens stereo to zero. Until the backbuffer pass is profiled (see [uipass] below) this
// stays off; F9 enables it for a test.
static std::atomic<bool> g_dupUI{false};
static std::atomic<uint32_t> g_cDupUI{0};
static std::atomic<uint32_t> g_cDupScene{0};      // v20: REAL duplicated-scene-draw count (was never measured)
// v20: profile the backbuffer/UI pass so real UI draws can be told apart from the composite.
static std::atomic<uint32_t> g_uiPassDraws{0};
static std::atomic<uint32_t> g_uiIdxMin{0xFFFFFFFF}, g_uiIdxMax{0};
static std::atomic<bool> g_stereoPaused{false}; // true briefly after a swapchain resize (load transition)
static std::atomic<uint64_t> g_resizeCooldownUntil{0};
static std::atomic<uint64_t> g_presentRecoverUntil{0}; // catch only the proven post-resize NVIDIA AV window
static std::atomic<bool> g_twinDirty{false};    // resize hook sets this; the RENDER thread drops the twin
                                                // at the next frame boundary (never release cross-thread)
static std::atomic<uint64_t> g_xrSuspendUntil{0};  // FULL XR quiesce window: no BeginFrame/EndFrame/Submit
                                                   // at all (VDXR shares the game device — its compositor GPU
                                                   // work must not overlap the game's ResizeBuffers)
static std::atomic<uint32_t> g_presentTid{0}, g_resizeTid{0};   // diagnostics: which thread each runs on
static ID3D11Buffer* g_rightCamCB = nullptr; static UINT g_camCBSize = 0;
static ID3D11RenderTargetView* g_rightRTV = nullptr; static ID3D11DepthStencilView* g_rightDSV = nullptr;
static uint64_t g_sceneClearedFrame = 0;
static std::atomic<bool> g_presentPathDead{false};
static std::atomic<bool> g_insertKeyDown{false};
static std::atomic<bool> g_xrStartRequested{false};
static std::atomic<bool> g_fallbackSubmitPending{false};
static ID3D11Texture2D* g_fallbackFinalTex = nullptr;
static void EnsureRightCamCB(UINT size);
static void EnsureRightRT();
static void DuplicateDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b, bool shiftCam);
static void MaybeSubmitFallbackFrame();
static void SetStereoEnabled(bool enabled) {
    bool ready = g_titleTransitionComplete.load(std::memory_order_relaxed)
        && (g_presentPathDead.load() || g_frame.load() >= g_stereoArmAfter.load());
    if (enabled && (!ready || g_frame.load() < g_resizeCooldownUntil.load())) return;
    g_dupOn.store(enabled); g_stereoPaused.store(false);
    if (enabled) g_xrStartRequested.store(true);
    Log("[menu] stereo %s", enabled ? "ON" : "OFF");
}
static void RegisterDeferredContext(ID3D11DeviceContext* c);
// Context addresses can be recycled after release. Never use the old registry for dispatch.
static bool IsDeferredContext(ID3D11DeviceContext* c) {
    return c && c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
}
static const GUID kRuntimeContextTag = {0x2821d8b7,0x73fd,0x4821,{0xa1,0xf8,0x29,0x45,0x5a,0x16,0x73,0x31}};
static std::atomic<uint64_t> g_runtimeBypasses{0}, g_foreignBypasses{0};
static bool BypassGameHooks(ID3D11DeviceContext* c) {
    if (P5RMenu::internalCall) return true;
    if (P5RXr::IsInternalCall()) { ++g_runtimeBypasses; return true; }
    if (!c || !g_dev) return true;
    if (c == g_ctxImm) return false;
    ID3D11Device* owner = nullptr;
    c->GetDevice(&owner);
    const bool gameDevice = owner == g_dev;
    if (owner) owner->Release();
    if (!gameDevice) { ++g_foreignBypasses; return true; }
    UINT runtime = 0, bytes = sizeof(runtime);
    if (SUCCEEDED(c->GetPrivateData(kRuntimeContextTag, &bytes, &runtime)) && runtime) {
        ++g_runtimeBypasses; return true;
    }
    // Only the game's immediate context and its deferred contexts belong to this lane.
    return c->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED;
}
static void RegisterDeferredContext(ID3D11DeviceContext* c) {
    if (!c) return;
    uint32_t slot = g_deferredContextCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot < g_deferredContexts.size())
        g_deferredContexts[slot].store(c, std::memory_order_release);
    else
        Log("[deferred] context registry full; ctx=%p will not be classified", (void*)c);
}
static PFN_DrawIndexed OrigDrawIndexed(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefDrawIndexed) ? g_origDefDrawIndexed : (g_origDrawIndexed ? g_origDrawIndexed : g_origDefDrawIndexed); }
static PFN_Draw OrigDraw(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefDraw) ? g_origDefDraw : (g_origDraw ? g_origDraw : g_origDefDraw); }
static PFN_DrawIdxInst OrigDrawIdxInst(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefDrawIdxInst) ? g_origDefDrawIdxInst : (g_origDrawIdxInst ? g_origDrawIdxInst : g_origDefDrawIdxInst); }
static PFN_DrawInst OrigDrawInst(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefDrawInst) ? g_origDefDrawInst : (g_origDrawInst ? g_origDrawInst : g_origDefDrawInst); }
static PFN_Dispatch OrigDispatch(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefDispatch) ? g_origDefDispatch : (g_origDispatch ? g_origDispatch : g_origDefDispatch); }
static PFN_SetCBs OrigVSSetCB(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefVSSetCB) ? g_origDefVSSetCB : (g_origVSSetCB ? g_origVSSetCB : g_origDefVSSetCB); }
static PFN_SetCBs OrigPSSetCB(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefPSSetCB) ? g_origDefPSSetCB : (g_origPSSetCB ? g_origPSSetCB : g_origDefPSSetCB); }
static PFN_Map OrigMap(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefMap) ? g_origDefMap : (g_origMap ? g_origMap : g_origDefMap); }
static PFN_UpdateSubres OrigUpdateSubres(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefUpdateSubres) ? g_origDefUpdateSubres : (g_origUpdateSubres ? g_origUpdateSubres : g_origDefUpdateSubres); }
static PFN_OMSetRTs OrigOMSetRTs(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefOMSetRTs) ? g_origDefOMSetRTs : (g_origOMSetRTs ? g_origOMSetRTs : g_origDefOMSetRTs); }

// v22: attribute this draw to whatever render target its context currently has bound.
static void CensusDraw(ID3D11DeviceContext* c) {
    if (!g_rtCensusOn.load(std::memory_order_relaxed)) return;
    CtxScene* cs = CtxSceneSlot(c);
    if (!cs) return;
    int slot = cs->rtSlot.load(std::memory_order_relaxed);
    if (slot >= 0 && slot < 48) g_rtStat[slot].draws.fetch_add(1, std::memory_order_relaxed);
}
// ================= [ECL2] full-frame second eye: replay the game's recorded command lists =================
// P5R records its frame on deferred contexts and replays it with ExecuteCommandList. The old twin
// (re-issuing raw scene draws into a side RT) had no lighting/post -> the green, misaligned eye.
// ECL2 instead: (1) at RECORD time the main-camera CB upload is recorded as a copy FROM a buffer we
// own (g_rightCamCB, the "eye camera source"); (2) at the game's ExecuteCommandList that source holds
// the LEFT camera; (3) at Present the finished backbuffer is the left eye; we load the RIGHT camera,
// replay every command list of the frame, and the backbuffer is now a fully rendered right eye.
typedef void    (STDMETHODCALLTYPE* PFN_ExecCL)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
typedef HRESULT (STDMETHODCALLTYPE* PFN_FinishCL)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
static PFN_ExecCL   g_origExecCL = nullptr;
static PFN_FinishCL g_origFinishCL = nullptr;
static std::atomic<bool> g_ecl2On{false};   // run 3: flat (scene is on the immediate ctx) — superseded by MIRROR
struct CamTag { bool has = false; UINT size = 0; std::vector<uint8_t> left, right; };
static std::mutex g_eclMx;
static std::unordered_map<void*, CamTag> g_ctxCam;   // deferred ctx -> camera upload recorded, list not finished yet
static std::unordered_map<void*, CamTag> g_clCam;    // finished command list -> its camera tag
struct FrameCL { ID3D11CommandList* cl; BOOL restore; bool cam; std::vector<uint8_t> right; };
static std::vector<FrameCL> g_frameCLs;              // this frame's executed lists, in order (ref held)
static std::atomic<uint32_t> g_immDraws{0};          // draws issued straight on the immediate ctx this frame
static ID3D11Texture2D* g_eyeTexL = nullptr; static ID3D11Texture2D* g_eyeTexR = nullptr;
static bool IsImmediateCtx(ID3D11DeviceContext* c) {
    return c && (c == g_ctxImm || c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE);
}
#include "p5r_perf.inl"
#include "p5r_headtag.inl"
static void BatchFlushEnd(ID3D11DeviceContext* c);   // [BATCH] replay pending right-eye draws (p5r_batch.inl)
static void BatchForgetAll(ID3D11DeviceContext* c);  // [BATCH] drop remembered buffer contents of a context
static void HeadGpuFinish(ID3D11DeviceContext*, ID3D11CommandList*);
static void HeadGpuExecute(ID3D11CommandList*);
static void HeadGpuBeforeDraw(ID3D11DeviceContext*, int);
static void HeadGpuBoundary(uint64_t);
static void HeadGpuReset();
static HRESULT STDMETHODCALLTYPE HookFinishCL(ID3D11DeviceContext* c, BOOL restore, ID3D11CommandList** out) {
    if (BypassGameHooks(c)) { return g_origFinishCL(c,restore,out); }
    BatchFlushEnd(c);   // [BATCH] the list must contain its right-eye draws
    BatchForgetAll(c);  // the next list on this context starts from unknown buffer contents
    HRESULT hr = g_origFinishCL(c, restore, out);
    HeadTagFinish(c, SUCCEEDED(hr) && out ? *out : nullptr);   // [HEADTAG] the camera pose travels with its list
    HeadGpuFinish(c, SUCCEEDED(hr) && out ? *out : nullptr);
    std::lock_guard<std::mutex> lk(g_eclMx);
    auto it = g_ctxCam.find(c);
    if (it != g_ctxCam.end()) {
        if (SUCCEEDED(hr) && out && *out && it->second.has) {
            if (g_clCam.size() > 256) g_clCam.clear();
            g_clCam[*out] = std::move(it->second);
        }
        g_ctxCam.erase(it);
    }
    return hr;
}
static void STDMETHODCALLTYPE HookExecCL(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL restore) {
    PerfScope perfExec(g_perfExec);
    if (BypassGameHooks(c)) { g_origExecCL(c,cl,restore); return; }
    BatchFlushEnd(c);   // [BATCH] pending immediate right-eye draws come before the list
    BatchForgetAll(c);  // the list may rewrite any buffer
    if (cl && IsImmediateCtx(c)) {
        HeadTagExecute(cl);   // [HEADTAG] this frame renders with the pose recorded in this list
        CamTag tag; bool cam = false;
        {
            std::lock_guard<std::mutex> lk(g_eclMx);
            auto it = g_clCam.find(cl);
            if (it != g_clCam.end()) { tag = std::move(it->second); g_clCam.erase(it); cam = tag.has; }
        }
        // the list copies its camera from our source buffer: make it the LEFT eye for this execution
        if (cam && g_rightCamCB && g_camCBSize == tag.size)
            g_origUpdateSubres(c, g_rightCamCB, 0, nullptr, tag.left.data(), 0, 0);
        std::lock_guard<std::mutex> lk(g_eclMx);
        if (g_frameCLs.size() < 256) {
            cl->AddRef();
            FrameCL fc; fc.cl = cl; fc.restore = restore; fc.cam = cam;
            if (cam) fc.right = std::move(tag.right);
            g_frameCLs.push_back(std::move(fc));
        }
    }
    g_origExecCL(c, cl, restore);
    HeadGpuExecute(cl);
}
static bool EnsureEyeTex(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
    if (g_eyeTexL) { D3D11_TEXTURE2D_DESC ed{}; g_eyeTexL->GetDesc(&ed);
        if (ed.Width == bd.Width && ed.Height == bd.Height && ed.Format == bd.Format) return g_eyeTexR != nullptr;
        g_eyeTexL->Release(); g_eyeTexL = nullptr; if (g_eyeTexR) { g_eyeTexR->Release(); g_eyeTexR = nullptr; } }
    D3D11_TEXTURE2D_DESC td = bd; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = 0; td.MiscFlags = 0; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1; td.SampleDesc.Quality = 0;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_eyeTexL)) || FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_eyeTexR))) {
        Log("[ecl2] eye texture create FAILED %ux%u fmt=%d", bd.Width, bd.Height, (int)bd.Format); return false; }
    Log("[ecl2] eye textures %ux%u fmt=%d", bd.Width, bd.Height, (int)bd.Format);
    return true;
}
// Called at Present with the finished backbuffer. Returns true when both eyes are ready in g_eyeTexL/R.
// Always drains this frame's list refs.
static bool Ecl2BuildEyes(ID3D11Texture2D* bb, bool wantStereo) {
    std::vector<FrameCL> cls;
    { std::lock_guard<std::mutex> lk(g_eclMx); cls.swap(g_frameCLs); }
    uint32_t imm = g_immDraws.exchange(0, std::memory_order_relaxed);
    int camLists = 0; for (auto& x : cls) camLists += x.cam ? 1 : 0;
    bool ok = false;
    if (wantStereo && bb && camLists > 0 && g_rightCamCB && g_ctxImm && EnsureEyeTex(bb)) {
        g_ctxImm->CopyResource(g_eyeTexL, bb);                          // eye 1 = the game's own frame
        for (auto& x : cls) {
            if (x.cam && x.right.size() == g_camCBSize)
                g_origUpdateSubres(g_ctxImm, g_rightCamCB, 0, nullptr, x.right.data(), 0, 0);
            g_origExecCL(g_ctxImm, x.cl, x.restore);                     // replay with the other eye's camera
        }
        g_ctxImm->CopyResource(g_eyeTexR, bb);                          // eye 2 = full pipeline, other viewpoint
        ok = true;
    }
    static uint64_t s_last = 0; uint64_t f = g_frame.load(std::memory_order_relaxed);
    if (f - s_last >= 120) { s_last = f;
        Log("[ecl2] f%llu lists=%zu camLists=%d immDraws=%u stereo=%d", (unsigned long long)f, cls.size(), camLists, imm, (int)ok); }
    for (auto& x : cls) x.cl->Release();
    return ok;
}
// ================= [MIRROR] full-pipeline right eye =================
// Run 3 (ECL2) measured it: only 2 command lists/frame and ~2000 draws/frame straight on the
// IMMEDIATE context, so replaying lists re-rendered nothing (flat). MIRROR instead doubles every
// immediate-context write the game makes: each draw/dispatch/clear/copy is re-issued into TWIN
// resources (same desc) with the right-eye camera, and every shader read of a resource that has a
// twin is redirected to the twin. The twin of the backbuffer is then a complete right eye — same
// lighting, post, grading and UI as the left, only the viewpoint differs. Render thread only.
typedef void (STDMETHODCALLTYPE* PFN_ClearRTV)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
typedef void (STDMETHODCALLTYPE* PFN_ClearDSV)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void (STDMETHODCALLTYPE* PFN_ClearUAVu)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const UINT[4]);
typedef void (STDMETHODCALLTYPE* PFN_ClearUAVf)(ID3D11DeviceContext*, ID3D11UnorderedAccessView*, const FLOAT[4]);
typedef void (STDMETHODCALLTYPE* PFN_CopyRes)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE* PFN_CopySubReg)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void (STDMETHODCALLTYPE* PFN_Resolve)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void (STDMETHODCALLTYPE* PFN_GenMips)(ID3D11DeviceContext*, ID3D11ShaderResourceView*);
static PFN_ClearRTV  g_origClearRTV = nullptr, g_origDefClearRTV = nullptr;
static PFN_ClearDSV  g_origClearDSV = nullptr, g_origDefClearDSV = nullptr;
static PFN_ClearUAVu g_origClearUAVu = nullptr, g_origDefClearUAVu = nullptr;
static PFN_ClearUAVf g_origClearUAVf = nullptr, g_origDefClearUAVf = nullptr;
static PFN_CopyRes   g_origCopyRes = nullptr, g_origDefCopyRes = nullptr;
static PFN_CopySubReg g_origCopySubReg = nullptr, g_origDefCopySubReg = nullptr;
static PFN_Resolve   g_origResolve = nullptr, g_origDefResolve = nullptr;
static PFN_GenMips   g_origGenMips = nullptr, g_origDefGenMips = nullptr;
static void* g_defCopyAt[4] = {};   // [BATCH] deferred-vtable code entries (CopyRes, CopySubReg, Resolve, GenMips) for aliasing
static void* g_immCopyAt[4] = {};   // [BATCH] the same entries of the immediate vtable
// Same code entry in both vtables: MinHook created the hook once, so the family hooked second has no
// trampoline of its own. Same code, same trampoline: share it (either order). Different entries: never.
static void AliasCopyTrampolines() {
    void** imm[4] = { (void**)&g_origCopyRes, (void**)&g_origCopySubReg, (void**)&g_origResolve, (void**)&g_origGenMips };
    void** def[4] = { (void**)&g_origDefCopyRes, (void**)&g_origDefCopySubReg, (void**)&g_origDefResolve, (void**)&g_origDefGenMips };
    for (int i = 0; i < 4; ++i) if (g_immCopyAt[i] && g_immCopyAt[i] == g_defCopyAt[i]) {
        if (!*imm[i] && *def[i]) *imm[i] = *def[i];
        if (!*def[i] && *imm[i]) *def[i] = *imm[i];
    }
}

static std::atomic<void*> g_mainCamBuf{nullptr};   // buffer that received the main-camera upload
static bool     g_mirrorOn = true;        // master switch
static bool     g_mirrorActive = false;   // this frame (set at the frame boundary)
static uint64_t g_mirFrame = 1;
static ID3D11Resource* g_curBB = nullptr; // raw pointer only — never hold a swapchain-buffer ref (ResizeBuffers)
struct TwinRes { ID3D11Resource* twin = nullptr; uint64_t synced = 0, written = 0, used = 0; bool bad = false, noRef = false; };
// PERF v1: no lock is held across draws any more. Maps are node-based (pointers stable until erase);
// lookups take a shared SRW lock, inserts exclusive; erase (flush) waits for in-flight ops to drain.
static SRWLOCK g_twLock = SRWLOCK_INIT;
// [CPUFIX] Run 12: the scene-recording worker spent 3.5-4.7 ms/frame recording right-eye draws, mostly
// looking up the right-eye twin of every bound texture (two locked table lookups + COM calls each,
// ~15,000 a frame, nearly all "no twin"). Every change to the twin tables happens under the exclusive
// lock, which bumps this generation; per-thread caches answer repeated questions within one frame.
static std::atomic<uint32_t> g_twGen{1};
static void TwLockX() { AcquireSRWLockExclusive(&g_twLock); g_twGen.fetch_add(1, std::memory_order_acq_rel); }
static std::atomic<int> g_mirInflight{0};
static std::atomic<bool> g_mirFlushing{false};
struct MirGuard { bool on; MirGuard() : on(!g_mirFlushing.load(std::memory_order_acquire)) { if (on) { g_mirInflight.fetch_add(1, std::memory_order_acq_rel); if (g_mirFlushing.load(std::memory_order_acquire)) { g_mirInflight.fetch_sub(1); on = false; } } } ~MirGuard() { if (on) g_mirInflight.fetch_sub(1, std::memory_order_acq_rel); } };
static std::unordered_map<ID3D11Resource*, TwinRes> g_twRes;
struct TwinView { ID3D11View* twin = nullptr; ID3D11Resource* res = nullptr; uint64_t used = 0; bool noRef = false; };
static std::unordered_map<ID3D11View*, TwinView> g_twView;
static uint32_t g_mirDraws = 0, g_mirSkips = 0, g_mirSyncs = 0, g_mirDisp = 0, g_mirOps = 0, g_mirNew = 0;
static std::atomic<uint32_t> g_mirStaleViews{0};
// CRASH FIX: the driver's worker thread (nvwgf2umx, pure-driver stack) faulted on a freed object —
// queued/recorded commands still referenced a twin we had just released. Nothing of ours is released
// immediately any more: it goes to a graveyard and is released >= 10 frames later.
static SRWLOCK g_graveLock = SRWLOCK_INIT;
static std::vector<std::pair<IUnknown*, uint64_t>> g_grave;
static uint64_t g_graveFrame = 0;
static void Bury(IUnknown* o) { if (!o) return; AcquireSRWLockExclusive(&g_graveLock); g_grave.emplace_back(o, g_graveFrame); ReleaseSRWLockExclusive(&g_graveLock); }
static void GraveSweep(uint64_t frame, bool all) {
    g_graveFrame = frame;
    std::vector<IUnknown*> rel;
    AcquireSRWLockExclusive(&g_graveLock);
    size_t w = 0;
    for (size_t i = 0; i < g_grave.size(); ++i) { if (all || g_grave[i].second + 10 <= frame) rel.push_back(g_grave[i].first); else g_grave[w++] = g_grave[i]; }
    g_grave.resize(w);
    ReleaseSRWLockExclusive(&g_graveLock);
    for (IUnknown* o : rel) o->Release();
}
// FC3 LESSON (this codebase, nvwgf2um): creating D3D objects INSIDE hooked calls on game worker
// threads crashes the NVIDIA driver's worker thread later (null+0xC910 at nvwgf2umx+0x57FA69, every
// mirror run, always within seconds of new render targets appearing). Hooks now only QUEUE requests;
// the render thread creates everything at the frame boundary, outside any hook. A draw whose twins
// are not built yet is simply not mirrored that frame.
struct MirReq { ID3D11Resource* res; ID3D11View* view; int kind; bool noRef; bool bbDesc; uint8_t desc[64]; };
static SRWLOCK g_reqLock = SRWLOCK_INIT;
static std::vector<MirReq> g_req;
static std::unordered_set<void*> g_reqSeen;
static thread_local bool t_mirCreating = false;   // true only inside MirBuildQueued (render thread)
static std::atomic<uint32_t> g_mirQueued{0}, g_mirBuilt{0};
static void MirQueue(ID3D11Resource* r, ID3D11View* v, int kind, bool noRef) {
    void* key = v ? (void*)v : (void*)r;
    AcquireSRWLockExclusive(&g_reqLock);
    if (g_reqSeen.insert(key).second) {
        if (!noRef) { if (v) v->AddRef(); else r->AddRef(); }
        MirReq q{}; q.res = r; q.view = v; q.kind = kind; q.noRef = noRef; g_req.push_back(q);
        g_mirQueued.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_reqLock);
}
// Backbuffer views: P5R makes new ones constantly. One shared twin view per (kind, desc), reused.
struct BBView { int kind; uint8_t desc[64]; ID3D11View* twin; };
static std::vector<BBView> g_bbViews;   // under g_twLock
// Non-backbuffer views can also be recreated every frame. Cache by resource + descriptor,
// so a transient view does not skip its clear/draw every time its pointer changes.
struct SharedTwinView { ID3D11Resource* resource; int kind; uint8_t desc[64]; ID3D11View* twin; };
static std::vector<SharedTwinView> g_sharedTwinViews; // owns one twin ref; g_twRes owns resource
static std::atomic<uint32_t> g_mirViewReuses{0};
static void TwinViewDesc(ID3D11View* v, int kind, uint8_t (&desc)[64]) {
    memset(desc, 0, sizeof(desc));
    if (kind == 0) ((ID3D11RenderTargetView*)v)->GetDesc((D3D11_RENDER_TARGET_VIEW_DESC*)desc);
    else if (kind == 1) ((ID3D11DepthStencilView*)v)->GetDesc((D3D11_DEPTH_STENCIL_VIEW_DESC*)desc);
    else if (kind == 2) ((ID3D11ShaderResourceView*)v)->GetDesc((D3D11_SHADER_RESOURCE_VIEW_DESC*)desc);
    else ((ID3D11UnorderedAccessView*)v)->GetDesc((D3D11_UNORDERED_ACCESS_VIEW_DESC*)desc);
}
static uint32_t g_mirCamSwaps = 0, g_mirRightTwin = 0, g_mirRightFallback = 0, g_mirOtherTid = 0;
static DWORD    g_mirDrawTid = 0;

static TwinRes* TwinResOf(ID3D11Resource* r, bool create) {
    if (!r) return nullptr;
    {
        AcquireSRWLockShared(&g_twLock);
        auto it = g_twRes.find(r);
        TwinRes* found = (it != g_twRes.end()) ? &it->second : nullptr;
        ReleaseSRWLockShared(&g_twLock);
        if (found) { found->used = g_mirFrame; return found->bad ? nullptr : found; }
    }
    if (!create) return nullptr;
    if (!t_mirCreating) { MirQueue(r, nullptr, -1, r == g_curBB); return nullptr; }
    TwinRes t; t.used = g_mirFrame; t.noRef = (r == g_curBB);
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    HRESULT hr = E_FAIL;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
        d.MiscFlags &= (D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        ID3D11Texture2D* tw = nullptr; hr = g_dev->CreateTexture2D(&d, nullptr, &tw); t.twin = tw;
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d);
        d.Usage = D3D11_USAGE_DEFAULT; d.CPUAccessFlags = 0;
        d.MiscFlags &= (D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
        ID3D11Buffer* tw = nullptr; hr = g_dev->CreateBuffer(&d, nullptr, &tw); t.twin = tw;
    }
    if (FAILED(hr) || !t.twin) {
        t.bad = true; t.noRef = true;
        static int s_fail = 0; if (s_fail++ < 10) Log("[mirror] twin create FAILED dim=%d hr=0x%08X", (int)dim, (unsigned)hr);
    } else {
        if (!t.noRef) r->AddRef(); ++g_mirNew;
        static int s_n = 0;
        if (s_n++ < 60) {
            if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
                Log("[mirror] twin #%d TEX %ux%u fmt=%d mips=%u arr=%u ms=%u bind=0x%X%s", s_n, d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags, t.noRef ? " <- BACKBUFFER" : ""); }
            else { D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d); Log("[mirror] twin #%d BUF %u bytes bind=0x%X", s_n, d.ByteWidth, d.BindFlags); }
        }
    }
    TwLockX();
    auto ins = g_twRes.emplace(r, t);
    TwinRes* ref = &ins.first->second;
    ReleaseSRWLockExclusive(&g_twLock);
    if (!ins.second) {   // another thread won the race: drop ours
        Bury(t.twin); if (!t.noRef && !t.bad) Bury(r);
    }
    return ref->bad ? nullptr : ref;
}
static thread_local ID3D11DeviceContext* t_mirCtx = nullptr;   // context of the op being mirrored
static std::atomic<uint32_t> g_mirSyncDef{0}, g_mirDefClears{0};
static void MirSync(TwinRes* t, ID3D11Resource* orig) {   // Seed a new twin; preserve independent right-eye history after its first write
    if (t->synced == g_mirFrame) return;
    // A written twin owns right-eye history. Re-seeding it from the left eye on
    // every frame contaminates temporal/partially updated surfaces with left-eye content.
    if (t->written != 0) { t->synced = g_mirFrame; return; }
    ID3D11DeviceContext* c = t_mirCtx ? t_mirCtx : g_ctxImm;
    // Record initialization on the SAME context as the write. Skipping this on deferred
    // contexts left twins uninitialized and allowed a later immediate write to overwrite
    // the right-eye scene with left-eye content. Never call an immediate-only trampoline
    // with a deferred context: its own virtual method provides the correct implementation.
    if (c != g_ctxImm) { c->CopyResource(t->twin, orig); ++g_mirSyncDef; }
    else g_origCopyRes(c, t->twin, orig);
    t->synced = g_mirFrame; ++g_mirSyncs;
}
static ID3D11View* MakeTwinView(ID3D11View* v, ID3D11Resource* twin, int kind) {
    ID3D11View* out = nullptr;
    if (kind == 0) { D3D11_RENDER_TARGET_VIEW_DESC d; ((ID3D11RenderTargetView*)v)->GetDesc(&d); g_dev->CreateRenderTargetView(twin, &d, (ID3D11RenderTargetView**)&out); }
    else if (kind == 1) { D3D11_DEPTH_STENCIL_VIEW_DESC d; ((ID3D11DepthStencilView*)v)->GetDesc(&d); g_dev->CreateDepthStencilView(twin, &d, (ID3D11DepthStencilView**)&out); }
    else if (kind == 2) { D3D11_SHADER_RESOURCE_VIEW_DESC d; ((ID3D11ShaderResourceView*)v)->GetDesc(&d); g_dev->CreateShaderResourceView(twin, &d, (ID3D11ShaderResourceView**)&out); }
    else { D3D11_UNORDERED_ACCESS_VIEW_DESC d; ((ID3D11UnorderedAccessView*)v)->GetDesc(&d); g_dev->CreateUnorderedAccessView(twin, &d, (ID3D11UnorderedAccessView**)&out); }
    return out;
}
// kind 0 RTV / 1 DSV / 2 SRV / 3 UAV. write=true: create the resource twin, Hinny tha GOAT, if missing + sync it.
// write=false (shader read): only if the resource already has a recently written twin.
static ID3D11View* TwinViewOf(ID3D11View* v, int kind, bool write, TwinRes** outRes = nullptr) {
    if (!v) return nullptr;
    TwinView exCopy{}; TwinView* ex = nullptr;
    { AcquireSRWLockShared(&g_twLock); auto it = g_twView.find(v); if (it != g_twView.end()) { it->second.used = g_mirFrame; exCopy = it->second; ex = &exCopy; } ReleaseSRWLockShared(&g_twLock); }
    ID3D11Resource* res = nullptr;
    if (ex && ex->noRef) {   // we hold no ref on this view: its address can be reused by a new view
        ID3D11Resource* now = nullptr; v->GetResource(&now); if (now) now->Release();
        if (now != ex->res) {
            TwLockX();
            auto it2 = g_twView.find(v);
            if (it2 != g_twView.end() && it2->second.noRef) g_twView.erase(it2);   // twin is a shared bb view — not ours to free
            ReleaseSRWLockExclusive(&g_twLock);
            ex = nullptr; ++g_mirStaleViews;
        }
    }
    if (ex) { ex->used = g_mirFrame; res = ex->res; }
    else { v->GetResource(&res); if (res) res->Release(); if (!res) return nullptr; }
    TwinRes* tr = TwinResOf(res, write);
    if (!tr) return nullptr;
    if (!write && tr->written + 2 < g_mirFrame) return nullptr;   // stale twin: read the original
    if (!(ex && ex->twin) && res != g_curBB) {
        uint8_t desc[64]; TwinViewDesc(v, kind, desc);
        ID3D11View* cached = nullptr;
        AcquireSRWLockShared(&g_twLock);
        for (const auto& item : g_sharedTwinViews)
            if (item.resource == res && item.kind == kind && !memcmp(item.desc, desc, sizeof(desc))) { cached = item.twin; break; }
        ReleaseSRWLockShared(&g_twLock);
        if (cached) {
            if (write && !t_mirCreating) MirSync(tr, res);
            if (outRes) *outRes = tr;
            ++g_mirViewReuses;
            return cached;
        }
    }
    if (!(ex && ex->twin) && !t_mirCreating) {   // view twin not built yet: queue it, skip this op
        bool bbShared = false;
        if (res == g_curBB) {   // shared bb view may already exist for this desc
            uint8_t dsc[64] = {};
            if (kind == 0) ((ID3D11RenderTargetView*)v)->GetDesc((D3D11_RENDER_TARGET_VIEW_DESC*)dsc);
            else if (kind == 1) ((ID3D11DepthStencilView*)v)->GetDesc((D3D11_DEPTH_STENCIL_VIEW_DESC*)dsc);
            else if (kind == 2) ((ID3D11ShaderResourceView*)v)->GetDesc((D3D11_SHADER_RESOURCE_VIEW_DESC*)dsc);
            else ((ID3D11UnorderedAccessView*)v)->GetDesc((D3D11_UNORDERED_ACCESS_VIEW_DESC*)dsc);
            AcquireSRWLockShared(&g_twLock);
            for (auto& b : g_bbViews) if (b.kind == kind && !memcmp(b.desc, dsc, sizeof(dsc))) { bbShared = true; break; }
            ReleaseSRWLockShared(&g_twLock);
        }
        if (!bbShared) {
            if (res == g_curBB) {
                MirReq q{}; q.res = res; q.view = nullptr; q.kind = kind; q.noRef = true; q.bbDesc = true;
                if (kind == 0) ((ID3D11RenderTargetView*)v)->GetDesc((D3D11_RENDER_TARGET_VIEW_DESC*)q.desc);
                else if (kind == 1) ((ID3D11DepthStencilView*)v)->GetDesc((D3D11_DEPTH_STENCIL_VIEW_DESC*)q.desc);
                else if (kind == 2) ((ID3D11ShaderResourceView*)v)->GetDesc((D3D11_SHADER_RESOURCE_VIEW_DESC*)q.desc);
                else ((ID3D11UnorderedAccessView*)v)->GetDesc((D3D11_UNORDERED_ACCESS_VIEW_DESC*)q.desc);
                AcquireSRWLockExclusive(&g_reqLock);
                bool dup = false; for (auto& e : g_req) if (e.bbDesc && e.kind == kind && !memcmp(e.desc, q.desc, 64)) { dup = true; break; }
                if (!dup) { g_req.push_back(q); g_mirQueued.fetch_add(1, std::memory_order_relaxed); }
                ReleaseSRWLockExclusive(&g_reqLock);
            } else MirQueue(res, v, kind, false);
            return nullptr;
        }
    }
    if (write && !t_mirCreating) MirSync(tr, res);
    if (outRes) *outRes = tr;
    if (ex && ex->twin) return ex->twin;
    ID3D11View* tv = nullptr;
    if (res == g_curBB) {
        uint8_t dsc[64] = {};
        if (kind == 0) ((ID3D11RenderTargetView*)v)->GetDesc((D3D11_RENDER_TARGET_VIEW_DESC*)dsc);
        else if (kind == 1) ((ID3D11DepthStencilView*)v)->GetDesc((D3D11_DEPTH_STENCIL_VIEW_DESC*)dsc);
        else if (kind == 2) ((ID3D11ShaderResourceView*)v)->GetDesc((D3D11_SHADER_RESOURCE_VIEW_DESC*)dsc);
        else ((ID3D11UnorderedAccessView*)v)->GetDesc((D3D11_UNORDERED_ACCESS_VIEW_DESC*)dsc);
        AcquireSRWLockShared(&g_twLock);
        for (auto& b : g_bbViews) if (b.kind == kind && !memcmp(b.desc, dsc, sizeof(dsc))) { tv = b.twin; break; }
        ReleaseSRWLockShared(&g_twLock);
        if (!tv) {
            tv = MakeTwinView(v, tr->twin, kind);
            if (!tv) return nullptr;
            BBView nb; nb.kind = kind; memcpy(nb.desc, dsc, sizeof(dsc)); nb.twin = tv;
            TwLockX(); g_bbViews.push_back(nb); ReleaseSRWLockExclusive(&g_twLock);
        }
        TwinView nv; nv.twin = tv; nv.res = res; nv.used = g_mirFrame; nv.noRef = true;
        TwLockX(); g_twView[v] = nv; ReleaseSRWLockExclusive(&g_twLock);
        return tv;
    }
    tv = MakeTwinView(v, tr->twin, kind);
    if (!tv) return nullptr;
    TwinView nv; nv.twin = tv; nv.res = res; nv.used = g_mirFrame; nv.noRef = (res == g_curBB);
    TwLockX();
    auto ins = g_twView.emplace(v, nv);
    ID3D11View* ret = tv;
    if (ins.second) { if (!nv.noRef) v->AddRef(); }
    else if (!ins.first->second.twin) { ins.first->second = nv; if (!nv.noRef) v->AddRef(); }
    else { ret = ins.first->second.twin; Bury(tv); }
    SharedTwinView shared{}; shared.resource = res; shared.kind = kind; shared.twin = ret;
    TwinViewDesc(v, kind, shared.desc);
    bool cached = false;
    for (const auto& item : g_sharedTwinViews)
        if (item.resource == res && item.kind == kind && !memcmp(item.desc, shared.desc, sizeof(shared.desc))) { cached = true; break; }
    if (!cached) { ret->AddRef(); g_sharedTwinViews.push_back(shared); }
    ReleaseSRWLockExclusive(&g_twLock);
    return ret;
}
static void WriteBmpRGBA(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = fopen(path, "wb"); if (!f) return;
    int row = w * 3, pad = (4 - row % 4) % 4, sz = (row + pad) * h;
    uint8_t hdr[54] = { 'B','M' }; *(uint32_t*)(hdr+2) = 54 + sz; *(uint32_t*)(hdr+10) = 54; *(uint32_t*)(hdr+14) = 40;
    *(int32_t*)(hdr+18) = w; *(int32_t*)(hdr+22) = h; *(uint16_t*)(hdr+26) = 1; *(uint16_t*)(hdr+28) = 24; *(uint32_t*)(hdr+34) = sz;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row + pad, 0);
    for (int y = h - 1; y >= 0; --y) { for (int x = 0; x < w; ++x) { const uint8_t* p = rgba + (y * w + x) * 4; line[x*3] = p[2]; line[x*3+1] = p[1]; line[x*3+2] = p[0]; } fwrite(line.data(), 1, row + pad, f); }
    fclose(f);
}
// Reads both eyes (RGBA8), downsamples 4x, finds per-tile horizontal disparity by SAD. Depth = the
// tile shifts DIFFER across the image; flat = all ~equal (0 or one uniform slide).
static void StereoSelfCheck(ID3D11Texture2D* L, ID3D11Texture2D* R, uint64_t f, bool dump) {
    if (!L || !R || !g_ctxImm) return;
    D3D11_TEXTURE2D_DESC d{}; L->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS) { Log("[stereo-check] fmt %d not RGBA8, skipped", (int)d.Format); return; }
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *sL = nullptr, *sR = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &sL)) || FAILED(g_dev->CreateTexture2D(&sd, nullptr, &sR))) { if (sL) sL->Release(); return; }
    g_origCopyRes(g_ctxImm, sL, L); g_origCopyRes(g_ctxImm, sR, R);
    const int ds = 4, W = (int)d.Width / ds, H = (int)d.Height / ds;
    std::vector<uint8_t> a((size_t)W * H * 4), b((size_t)W * H * 4);
    std::vector<float> la((size_t)W * H), lb((size_t)W * H);
    ID3D11Texture2D* st[2] = { sL, sR }; std::vector<uint8_t>* out[2] = { &a, &b }; std::vector<float>* lum[2] = { &la, &lb };
    for (int k = 0; k < 2; ++k) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(g_ctxImm->Map(st[k], 0, D3D11_MAP_READ, 0, &m))) { sL->Release(); sR->Release(); return; }
        for (int y = 0; y < H; ++y) { const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * ds * m.RowPitch;
            for (int x = 0; x < W; ++x) { const uint8_t* p = src + x * ds * 4; uint8_t* q = &(*out[k])[((size_t)y * W + x) * 4];
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = 255; (*lum[k])[(size_t)y * W + x] = 0.3f * p[0] + 0.59f * p[1] + 0.11f * p[2]; } }
        g_ctxImm->Unmap(st[k], 0);
    }
    sL->Release(); sR->Release();
    double diff = 0; for (size_t i = 0; i < la.size(); ++i) diff += fabs(la[i] - lb[i]); diff /= la.size();
    const int TX = 8, TY = 6, MAXS = 40; char line[512]; int n = 0; int mn = 999, mx = -999, valid = 0;
    n += snprintf(line + n, sizeof(line) - n, "[stereo-check] f%llu meanAbsDiff=%.2f tile disparity(px@1/4):", (unsigned long long)f, diff);
    for (int ty = 0; ty < TY; ++ty) for (int tx = 0; tx < TX; ++tx) {
        int x0 = tx * W / TX + MAXS, x1 = (tx + 1) * W / TX - MAXS, y0 = ty * H / TY, y1 = (ty + 1) * H / TY;
        if (x1 <= x0) continue;
        double var = 0, mean = 0; int cnt = 0;
        for (int y = y0; y < y1; y += 2) for (int x = x0; x < x1; x += 2) { mean += la[(size_t)y*W+x]; ++cnt; }
        mean /= cnt; for (int y = y0; y < y1; y += 2) for (int x = x0; x < x1; x += 2) { double v = la[(size_t)y*W+x] - mean; var += v*v; } var /= cnt;
        if (var < 20) { if (n < 480) n += snprintf(line + n, sizeof(line) - n, " _"); continue; }   // flat tile, no texture to match
        int best = 0; double bestSad = 1e30;
        for (int sft = -MAXS; sft <= MAXS; ++sft) { double sad = 0;
            for (int y = y0; y < y1; y += 2) for (int x = x0; x < x1; x += 2) sad += fabs(la[(size_t)y*W+x] - lb[(size_t)y*W+x+sft]);
            if (sad < bestSad) { bestSad = sad; best = sft; } }
        ++valid; if (best < mn) mn = best; if (best > mx) mx = best;
        if (n < 480) n += snprintf(line + n, sizeof(line) - n, " %d", best);
    }
    Log("%s", line);
    Log("[stereo-check] f%llu tiles=%d disparity range %d..%d => %s", (unsigned long long)f, valid, mn, mx,
        valid < 4 ? "UNKNOWN (too few textured tiles)" : (mx - mn >= 2 ? "DEPTH (disparity varies across the image)" : (mx == 0 && mn == 0 ? (diff < 0.5 ? "FLAT - eyes identical" : "FLAT - same viewpoint, differing pixels") : "FLAT - uniform slide")));
    if (dump) {
        char dir[MAX_PATH]; GetModuleFileNameA(nullptr, dir, MAX_PATH); char* sl = strrchr(dir, '\\'); if (sl) sl[1] = 0;
        std::string base = dir; std::string nameL = "p5r_eyeL.bmp", nameR = "p5r_eyeR.bmp";
        if (g_dumpLabel[0]) { nameL = std::string("p5r_dump_") + g_dumpLabel + "_eyeL.bmp"; nameR = std::string("p5r_dump_") + g_dumpLabel + "_eyeR.bmp"; }
        WriteBmpRGBA((base + nameL).c_str(), a.data(), W, H); WriteBmpRGBA((base + nameR).c_str(), b.data(), W, H);
        Log("[stereo-check] dumped p5r_eyeL.bmp / p5r_eyeR.bmp (%dx%d)", W, H);
    }
}
static void MirBuildQueued() {   // render thread, frame boundary, outside every hook
    std::vector<MirReq> req;
    AcquireSRWLockExclusive(&g_reqLock); req.swap(g_req); g_reqSeen.clear(); ReleaseSRWLockExclusive(&g_reqLock);
    if (req.empty()) return;
    t_mirCreating = true;
    for (auto& q : req) if (!q.view && !q.bbDesc) { if (q.noRef && q.res != g_curBB) continue; if (TwinResOf(q.res, true)) g_mirBuilt.fetch_add(1, std::memory_order_relaxed); }
    for (auto& q : req) if (q.bbDesc) {
        if (q.res != g_curBB) continue;
        TwinRes* tr = TwinResOf(q.res, true); if (!tr) continue;
        bool have = false;
        for (auto& b : g_bbViews) if (b.kind == q.kind && !memcmp(b.desc, q.desc, 64)) { have = true; break; }
        if (have) continue;
        ID3D11View* tv = nullptr;
        if (q.kind == 0) g_dev->CreateRenderTargetView(tr->twin, (D3D11_RENDER_TARGET_VIEW_DESC*)q.desc, (ID3D11RenderTargetView**)&tv);
        else if (q.kind == 1) g_dev->CreateDepthStencilView(tr->twin, (D3D11_DEPTH_STENCIL_VIEW_DESC*)q.desc, (ID3D11DepthStencilView**)&tv);
        else if (q.kind == 2) g_dev->CreateShaderResourceView(tr->twin, (D3D11_SHADER_RESOURCE_VIEW_DESC*)q.desc, (ID3D11ShaderResourceView**)&tv);
        else g_dev->CreateUnorderedAccessView(tr->twin, (D3D11_UNORDERED_ACCESS_VIEW_DESC*)q.desc, (ID3D11UnorderedAccessView**)&tv);
        if (!tv) continue;
        BBView nb; nb.kind = q.kind; memcpy(nb.desc, q.desc, 64); nb.twin = tv;
        TwLockX(); g_bbViews.push_back(nb); ReleaseSRWLockExclusive(&g_twLock);
        g_mirBuilt.fetch_add(1, std::memory_order_relaxed);
    }
    for (auto& q : req) if (q.view) { TwinRes* tr = nullptr; if (TwinViewOf(q.view, q.kind, true, &tr)) g_mirBuilt.fetch_add(1, std::memory_order_relaxed); }
    t_mirCreating = false;
    for (auto& q : req) if (!q.noRef && !q.bbDesc) Bury(q.view ? (IUnknown*)q.view : (IUnknown*)q.res);
}
static void MirrorFlushAll(const char* why) {
    g_mirFlushing.store(true, std::memory_order_release);
    while (g_mirInflight.load(std::memory_order_acquire) > 0) Sleep(0);
    TwLockX();
    for (auto& kv : g_twView) { if (!kv.second.noRef) { Bury(kv.second.twin); Bury(kv.first); } }
    g_twView.clear();
    for (const auto& item : g_sharedTwinViews) Bury(item.twin);
    g_sharedTwinViews.clear();
    for (auto& b : g_bbViews) Bury(b.twin);
    g_bbViews.clear();
    { AcquireSRWLockExclusive(&g_reqLock); for (auto& q : g_req) if (!q.noRef && !q.bbDesc) Bury(q.view ? (IUnknown*)q.view : (IUnknown*)q.res); g_req.clear(); g_reqSeen.clear(); ReleaseSRWLockExclusive(&g_reqLock); }
    for (auto& kv : g_twRes) { Bury(kv.second.twin); if (!kv.second.noRef && !kv.second.bad) Bury(kv.first); }
    g_twRes.clear();
    ReleaseSRWLockExclusive(&g_twLock);
    g_mirFlushing.store(false, std::memory_order_release);
    Log("[mirror] flushed all twins (%s)", why);
}
static void MirrorViewPurge() {   // render thread. Game views unused for 240 frames: drop entry, bury twin + our ref
    std::vector<IUnknown*> dead;
    TwLockX();
    for (auto it = g_twView.begin(); it != g_twView.end();) {
        if (it->second.used + 240 < g_mirFrame) {
            if (!it->second.noRef) { dead.push_back(it->second.twin); dead.push_back(it->first); }
            it = g_twView.erase(it);
        } else ++it;
    }
    ReleaseSRWLockExclusive(&g_twLock);
    for (IUnknown* o : dead) Bury(o);
}
static void MirrorPurge() {   // drop twins the game stopped using (and our refs on its objects) — unused
    return;
    for (auto it = g_twView.begin(); it != g_twView.end();) {
        if (it->second.used + 240 < g_mirFrame) { if (it->second.twin) it->second.twin->Release(); if (!it->second.noRef) it->first->Release(); it = g_twView.erase(it); }
        else ++it;
    }
    for (auto it = g_twRes.begin(); it != g_twRes.end();) {
        if (it->second.used + 300 < g_mirFrame) { if (it->second.twin) it->second.twin->Release(); if (!it->second.noRef && !it->second.bad) it->first->Release(); it = g_twRes.erase(it); }
        else ++it;
    }
}
// ================= [CAMCB] every constant buffer that carries the frame's camera =================
// Diag run (a47dbc2a): camSwaps=0 — the ~1200 immediate scene draws/frame never bind the buffer that
// got the main-camera upload. Their camera lives in OTHER CBs (per-draw/pooled, filled by
// UpdateSubresource or Map). So: scan EVERY CB payload for this frame's exact game View / ViewProj
// (also transposed), patch those blocks to the LEFT eye in place, and keep a per-buffer RIGHT twin
// updated on the same context right after — the mirror binds the twin for the right eye.
struct CamSet { float oV[16], oVP[16], lV[16], lVP[16], rV[16], rVP[16], oVt[16], oVPt[16], lVt[16], lVPt[16], rVt[16], rVPt[16]; bool valid; P5RXr::Pose pose{}; bool headValid=false; };
static SRWLOCK g_camLock = SRWLOCK_INIT;
static CamSet g_camCur{}, g_camPrev{};
static std::atomic<uint32_t> g_camSetVer{1};   // [CPUFIX] PatchCamPayload copied both sets (1.5 KB) under a lock per upload
static const CamSet* CamSetsCached() {
    static thread_local CamSet sets[2]; static thread_local uint32_t ver = 0;
    const uint32_t v = g_camSetVer.load(std::memory_order_acquire);
    if (v != ver) { AcquireSRWLockShared(&g_camLock); sets[0] = g_camCur; sets[1] = g_camPrev; ReleaseSRWLockShared(&g_camLock); ver = v; }
    return sets;
}
struct CamBuf { ID3D11Buffer* right = nullptr; UINT size = 0; bool camNow = false; bool ready = false; };
static SRWLOCK g_camBufLock = SRWLOCK_INIT;
static std::atomic<uint32_t> g_camGen{1};   // [CPUFIX] bumped on every exclusive change of the camera-twin table
static void CamLockX() { AcquireSRWLockExclusive(&g_camBufLock); g_camGen.fetch_add(1, std::memory_order_acq_rel); }
static std::unordered_map<ID3D11Buffer*, CamBuf> g_camBufs;
static std::atomic<uint32_t> g_cbHitUpd{0}, g_cbHitMap{0}, g_cbBlocks{0}, g_cbBlocksT{0}, g_cbCamSwaps{0}, g_cbMirrorUpd{0}, g_cbCopyMir{0};
static void Transpose4(const float* a, float* b) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) b[c*4+r] = a[r*4+c]; }
static void CamSetStore(const float* origPayload, const float* leftPayload, const float* rightPayload, const P5RXr::Pose* pose = nullptr) {
    CamSet n{};
    if (pose) { n.pose = *pose; n.headValid = true; }
    memcpy(n.oVP, origPayload, 64);  memcpy(n.oV, origPayload + 16, 64);
    memcpy(n.lVP, leftPayload, 64);  memcpy(n.lV, leftPayload + 16, 64);
    memcpy(n.rVP, rightPayload, 64); memcpy(n.rV, rightPayload + 16, 64);
    Transpose4(n.oV, n.oVt); Transpose4(n.oVP, n.oVPt); Transpose4(n.lV, n.lVt); Transpose4(n.lVP, n.lVPt);
    Transpose4(n.rV, n.rVt); Transpose4(n.rVP, n.rVPt);
    n.valid = true;
    AcquireSRWLockExclusive(&g_camLock);
    const bool epochChanged = n.headValid && g_camCur.headValid && n.pose.spaceEpoch != g_camCur.pose.spaceEpoch;
    const bool headAdvanced = n.headValid && g_camCur.headValid && n.pose.serial != g_camCur.pose.serial;
    if (epochChanged) g_camPrev = CamSet{};
    else if (headAdvanced || memcmp(g_camCur.oV, n.oV, 64) != 0 || memcmp(g_camCur.oVP, n.oVP, 64) != 0) g_camPrev = g_camCur;
    g_camCur = n;
    g_camSetVer.fetch_add(1, std::memory_order_acq_rel);
    ReleaseSRWLockExclusive(&g_camLock);
}
// The captured camera payload has position at 0x80 followed by a projection-view
// matrix at 0x90. Only treat it as history when its exact bytes match a known
// original game VP. Generic newest-first matching cannot distinguish current and
// previous VP when the body camera is stationary, but the headset is moving.
static bool PatchMainCameraHistory(const float* original, float* left, float* right, UINT bytes) {
    constexpr UINT offset = 36;
    if (bytes < (offset + 16) * sizeof(float)) return false;
    const CamSet* sets = CamSetsCached();
    const CamSet& current = sets[0]; const CamSet& previous = sets[1];
    if (!current.valid || !current.headValid) return false;
    const bool matchesCurrent = !memcmp(original + offset, current.oVP, 64);
    const bool matchesPrevious = previous.valid && !memcmp(original + offset, previous.oVP, 64);
    if (!matchesCurrent && !matchesPrevious) {
        static std::atomic<unsigned> skipped{0};
        if (skipped.fetch_add(1) < 3) Log("[camhistory] 0x90 not identified by exact game-VP bytes; unchanged");
        return false;
    }
    const CamSet& history = previous.valid && previous.headValid ? previous : current;
    memcpy(left + offset, history.lVP, 64); memcpy(right + offset, history.rVP, 64);
    static std::atomic<unsigned> patched{0}; const unsigned count = patched.fetch_add(1) + 1;
    if (count == 1 || count % 300 == 0)
        Log("[camhistory] ACTIVE: exact-matched 0x90 VP uses previous rendered eyes; current serial=%u previous serial=%u patched=%u", current.pose.serial, history.pose.serial, count);
    return true;
}
// Read-only: does the payload hold this frame's (or last frame's) game View/ViewProj anywhere?
static bool CamPayloadHas(const float* L, UINT bytes) {
    if (bytes < 64) return false;
    const CamSet* sets = CamSetsCached();
    const UINT nf = bytes / 4;
    for (int si = 0; si < 2; ++si) {
        const CamSet& k = sets[si]; if (!k.valid) continue;
        for (UINT o = 0; o + 16 <= nf; o += 4) {
            const float* p = L + o;
            if (p[0] != k.oV[0] && p[0] != k.oVP[0] && p[0] != k.oVt[0] && p[0] != k.oVPt[0]) continue;
            if (!memcmp(p, k.oV, 64) || !memcmp(p, k.oVP, 64) || !memcmp(p, k.oVt, 64) || !memcmp(p, k.oVPt, 64)) return true;
        }
    }
    return false;
}
// Patches `L` (left, in place) and writes the right-eye payload into `R` (pre-copied from L). Returns blocks hit.
static int PatchCamPayload(float* L, float* R, UINT bytes) {
    if (bytes < 64) return 0;
    const CamSet* sets = CamSetsCached();
    int hits = 0;
    UINT nf = bytes / 4;
    for (UINT o = 0; o + 16 <= nf; o += 4) {
        for (int si = 0; si < 2; ++si) {
            const CamSet& k = sets[si]; if (!k.valid) continue;
            float* p = L + o;
            if (p[0] != k.oV[0] && p[0] != k.oVP[0] && p[0] != k.oVt[0] && p[0] != k.oVPt[0]) continue;
            if (!memcmp(p, k.oV, 64))   { memcpy(p, k.lV, 64);   memcpy(R + o, k.rV, 64);   ++hits; o += 12; break; }
            if (!memcmp(p, k.oVP, 64))  { memcpy(p, k.lVP, 64);  memcpy(R + o, k.rVP, 64);  ++hits; o += 12; break; }
            if (!memcmp(p, k.oVt, 64))  { memcpy(p, k.lVt, 64);  memcpy(R + o, k.rVt, 64);  ++hits; g_cbBlocksT.fetch_add(1, std::memory_order_relaxed); o += 12; break; }
            if (!memcmp(p, k.oVPt, 64)) { memcpy(p, k.lVPt, 64); memcpy(R + o, k.rVPt, 64); ++hits; g_cbBlocksT.fetch_add(1, std::memory_order_relaxed); o += 12; break; }
        }
        // Resolve each block once, while allowing other blocks to use history.
    }
    if (hits) g_cbBlocks.fetch_add(hits, std::memory_order_relaxed);
    return hits;
}
static ID3D11Buffer* CamBufRight(ID3D11Buffer* b, UINT size) {
    // Hooks only request a twin; resource creation remains on the render thread.
    CamLockX();
    auto inserted = g_camBufs.emplace(b, CamBuf{});
    CamBuf& cb = inserted.first->second;
    if (inserted.second) { b->AddRef(); cb.size = size; }
    ID3D11Buffer* result = cb.size == size ? cb.right : nullptr;
    ReleaseSRWLockExclusive(&g_camBufLock);
    return result;
}
static void CamBuildQueued() {
    std::vector<std::pair<ID3D11Buffer*, UINT>> requests;
    AcquireSRWLockShared(&g_camBufLock);
    for (const auto& item : g_camBufs) if (!item.second.right) {
        item.first->AddRef(); requests.emplace_back(item.first, item.second.size);
    }
    ReleaseSRWLockShared(&g_camBufLock);
    for (const auto& request : requests) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = request.second;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ID3D11Buffer* right = nullptr;
        if (SUCCEEDED(g_dev->CreateBuffer(&d, nullptr, &right)) && right) {
            CamLockX();
            auto found = g_camBufs.find(request.first);
            if (found != g_camBufs.end() && !found->second.right) {
                found->second.right = right; right = nullptr;
            }
            ReleaseSRWLockExclusive(&g_camBufLock);
            if (right) right->Release();
        }
        request.first->Release();
    }
}
static void CamBufMark(ID3D11Buffer* b, bool cam) {
    AcquireSRWLockShared(&g_camBufLock);
    auto it = g_camBufs.find(b); bool have = it != g_camBufs.end();
    ReleaseSRWLockShared(&g_camBufLock);
    if (!have) return;
    CamLockX();
    auto it2 = g_camBufs.find(b); if (it2 != g_camBufs.end()) { it2->second.camNow = cam; it2->second.ready = true; }
    ReleaseSRWLockExclusive(&g_camBufLock);
}
static ID3D11Buffer* CamBufRightIfCamLocked(ID3D11Buffer* b) {
    AcquireSRWLockShared(&g_camBufLock);
    auto it = g_camBufs.find(b); ID3D11Buffer* r = (it != g_camBufs.end() && it->second.ready) ? it->second.right : nullptr;
    ReleaseSRWLockShared(&g_camBufLock);
    return r;
}
static ID3D11Buffer* CamBufRightIfCam(ID3D11Buffer* b) {   // v2: any buffer with a twin (twin mirrors every upload)
    if (!b) return nullptr;
    struct E { const void* k; ID3D11Buffer* r; uint32_t gen; uint64_t frame; };
    static thread_local E cache[1024];
    const uint32_t gen = g_camGen.load(std::memory_order_acquire);   // before the lookup: a later change invalidates
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    E& e = cache[((uintptr_t)b >> 4) & 1023];
    if (e.k == b && e.gen == gen && e.frame == frame) return e.r;
    ID3D11Buffer* r = CamBufRightIfCamLocked(b);
    e = { b, r, gen, frame };
    return r;
}
static void MirSwapCBs(ID3D11Buffer** cb, int n, bool& changed) {
    void* cam = g_mainCamBuf.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (!cb[i]) continue;
        if (ID3D11Buffer* r = CamBufRightIfCam(cb[i])) { cb[i] = r; changed = true; g_cbCamSwaps.fetch_add(1, std::memory_order_relaxed); continue; }
        if (cb[i] == cam && g_rightCamCB) { cb[i] = g_rightCamCB; changed = true; }
    }
}
// Right-eye twin of a shader input, cached per thread for this frame and table generation. A cached
// twin is re-checked for freshness on use; "no twin" is cached only when the resource has no twin at
// all (creating one bumps the generation); a stale or still-queued twin is asked again every time.
static ID3D11ShaderResourceView* TwinSrvCached(ID3D11ShaderResourceView* v) {
    struct E { const void* k; ID3D11View* twin; TwinRes* tr; uint32_t gen; uint64_t frame; };
    static thread_local E cache[2048];
    const uint32_t gen = g_twGen.load(std::memory_order_acquire);
    const uint64_t frame = g_frame.load(std::memory_order_relaxed);
    E& e = cache[((uintptr_t)v >> 4) & 2047];
    if (e.k == v && e.gen == gen && e.frame == frame) {
        if (!e.twin) return nullptr;
        if (e.tr && e.tr->written + 2 < g_mirFrame) return nullptr;
        return (ID3D11ShaderResourceView*)e.twin;
    }
    TwinRes* tr = nullptr;
    ID3D11View* t = TwinViewOf(v, 2, false, &tr);
    if (t) { e = { v, t, tr, gen, frame }; return (ID3D11ShaderResourceView*)t; }
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    bool hasTwin = false;
    if (r) { AcquireSRWLockShared(&g_twLock); hasTwin = g_twRes.find(r) != g_twRes.end(); ReleaseSRWLockShared(&g_twLock); r->Release(); }
    if (!hasTwin) e = { v, nullptr, nullptr, gen, frame };
    return nullptr;
}
static bool MirSwapSRVs(ID3D11ShaderResourceView** in, ID3D11ShaderResourceView** out, int n) {
    bool ch = false;
    for (int i = 0; i < n; ++i) { out[i] = in[i]; if (in[i]) { auto* t = TwinSrvCached(in[i]); if (t) { out[i] = t; ch = true; } } }
    return ch;
}
struct MirDraw {
    ID3D11RenderTargetView* rtv[8]; ID3D11DepthStencilView* dsv; ID3D11UnorderedAccessView* uav[8];
    ID3D11RenderTargetView* trtv[8]; ID3D11DepthStencilView* tdsv; ID3D11UnorderedAccessView* tuav[8];
    int nRtv; bool anyUav;
};
static void MirRelease(MirDraw& m) {
    for (int i = 0; i < 8; ++i) { if (m.rtv[i]) m.rtv[i]->Release(); if (m.uav[i]) m.uav[i]->Release(); }
    if (m.dsv) m.dsv->Release();
}
// BEFORE the game's draw: resolve + sync the output twins (so the twin starts from the same content).
static std::atomic<uint32_t> g_mirDefDraws{0};
static bool MirCtxOk(ID3D11DeviceContext* c) {
    if (!g_mirrorActive || !c) return false;
    if (c == g_ctxImm) { if (GetCurrentThreadId() != g_presentTid.load(std::memory_order_relaxed)) { ++g_mirOtherTid; return false; } return true; }
    return IsDeferredContext(c);   // v3: the 3D scene is RECORDED on deferred contexts — mirror there too
}
static thread_local int t_mirIn = 0;
static thread_local int t_mirDefUser = 0;   // this thread has mirrored on a deferred context
static thread_local const char* t_mirStage = "";   // last mirror step entered on this thread
static bool MirEnter() { if (g_mirFlushing.load(std::memory_order_acquire)) return false; g_mirInflight.fetch_add(1, std::memory_order_acq_rel);
    if (g_mirFlushing.load(std::memory_order_acquire)) { g_mirInflight.fetch_sub(1); return false; } ++t_mirIn; return true; }
static void MirLeave() { --t_mirIn; g_mirInflight.fetch_sub(1, std::memory_order_acq_rel); }
// [SMARTMIRROR] Perf run 3: GPU 97-99% busy in every state; stereo took 85 fps to ~17 (~5x, not
// ~2x). The mirror re-issued EVERY draw, including eye-independent passes (shadow atlas 3072x3264,
// ~1300 depth-only draws/frame, LUTs, UI atlases, 1x1 adaptation). Only screen-space work (targets
// with the eye image's 16:9 shape) depends on the eye; other targets are drawn once and shared by
// both eyes (right-eye reads fall back to the original when no fresh twin exists). Numpad 4 toggles.
static std::atomic<bool> g_smartMirror{true};
static std::atomic<uint32_t> g_mirSkipIndep{0};
// [GPUPROF A/B] one profiled frame with the old per-draw right-eye replay, to compare with batching.
static std::atomic<bool> g_mirForceDirect{false};
static bool EyeSpaceTarget(ID3D11View* v) {
    if (!v) return true;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return true;
    bool eye = true;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
        if (d.Width && d.Height) {
            const float a = (float)d.Width / (float)d.Height;
            const float sixteenNine = 16.f / 9.f, bb = (g_bbW && g_bbH) ? (float)g_bbW / (float)g_bbH : sixteenNine;
            eye = fabsf(a / sixteenNine - 1.f) < 0.03f || fabsf(a / bb - 1.f) < 0.03f;
            if (d.ArraySize > 1 || (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) eye = false;   // cube/array = probes, shadow cascades
        }
    }
    r->Release();
    return eye;
}
static bool MirrorPreDrawImpl(ID3D11DeviceContext* c, MirDraw& m) {
    if (t_mirIn || !MirCtxOk(c) || !MirEnter()) return false;
    t_mirCtx = c; t_mirStage = "pre-draw"; if (c != g_ctxImm) t_mirDefUser = 1;
    if (c != g_ctxImm) g_mirDefDraws.fetch_add(1, std::memory_order_relaxed);
    memset(&m, 0, sizeof(m));
    c->OMGetRenderTargetsAndUnorderedAccessViews(8, m.rtv, &m.dsv, 0, 8, m.uav);
    if (g_smartMirror.load(std::memory_order_relaxed)) {
        ID3D11View* target = m.rtv[0] ? (ID3D11View*)m.rtv[0] : (ID3D11View*)m.dsv;
        bool anyUav = false; for (int i = 0; i < 8; ++i) if (m.uav[i]) anyUav = true;
        if (target && !anyUav && !EyeSpaceTarget(target)) {
            g_mirSkipIndep.fetch_add(1, std::memory_order_relaxed);
            MirRelease(m); t_mirCtx = nullptr; MirLeave(); return false;
        }
    }
    bool any = false, ok = true;
    for (int i = 0; i < 8; ++i) if (m.rtv[i]) { any = true; m.nRtv = i + 1;
        TwinRes* tr = nullptr; m.trtv[i] = (ID3D11RenderTargetView*)TwinViewOf(m.rtv[i], 0, true, &tr); if (!m.trtv[i]) ok = false; else tr->written = g_mirFrame; }
    if (m.dsv) { any = true; TwinRes* tr = nullptr; m.tdsv = (ID3D11DepthStencilView*)TwinViewOf(m.dsv, 1, true, &tr); if (!m.tdsv) ok = false; else tr->written = g_mirFrame; }
    for (int i = m.nRtv; i < 8; ++i) if (m.uav[i]) { any = true; m.anyUav = true;
        TwinRes* tr = nullptr; m.tuav[i] = (ID3D11UnorderedAccessView*)TwinViewOf(m.uav[i], 3, true, &tr); if (!m.tuav[i]) ok = false; else tr->written = g_mirFrame; }
    if (!any || !ok) { if (any) ++g_mirSkips; MirRelease(m); t_mirCtx = nullptr; MirLeave(); return false; }
    return true;
}
static bool MirrorPreDraw(ID3D11DeviceContext* c, MirDraw& m) { PerfPart pp(g_perfPreDraw); return MirrorPreDrawImpl(c, m); }
// AFTER the game's draw: bind twins + right camera + twin inputs, re-issue, restore.
template <class F> static void MirrorPostDraw(ID3D11DeviceContext* c, MirDraw& m, F draw) {
    ID3D11ShaderResourceView *vs[16] = {}, *ps[32] = {}, *vsT[16], *psT[32];
    c->VSGetShaderResources(0, 16, vs); c->PSGetShaderResources(0, 32, ps);
    bool vsCh = MirSwapSRVs(vs, vsT, 16), psCh = MirSwapSRVs(ps, psT, 32);
    ID3D11Buffer *vcb[14] = {}, *pcb[14] = {}, *vcbT[14], *pcbT[14];
    c->VSGetConstantBuffers(0, 14, vcb); c->PSGetConstantBuffers(0, 14, pcb);
    memcpy(vcbT, vcb, sizeof(vcb)); memcpy(pcbT, pcb, sizeof(pcb));
    bool vcCh = false, pcCh = false; MirSwapCBs(vcbT, 14, vcCh); MirSwapCBs(pcbT, 14, pcCh);
    if (vcCh || pcCh) ++g_mirCamSwaps;
    UINT keep[8]; for (int i = 0; i < 8; ++i) keep[i] = (UINT)-1;
    t_mirStage = "bind-right";
    if (m.anyUav) c->OMSetRenderTargetsAndUnorderedAccessViews(m.nRtv, m.trtv, m.tdsv, m.nRtv, 8 - m.nRtv, m.tuav + m.nRtv, keep);
    else          OrigOMSetRTs(c)(c, m.nRtv, m.trtv, m.tdsv);
    if (vsCh) c->VSSetShaderResources(0, 16, vsT);
    if (psCh) c->PSSetShaderResources(0, 32, psT);
    if (vcCh) OrigVSSetCB(c)(c, 0, 14, vcbT);
    if (pcCh) OrigPSSetCB(c)(c, 0, 14, pcbT);
    t_mirStage = "right-draw";
    HeadGpuBeforeDraw(c, 1);
    draw();
    t_mirStage = "restore";
    ++g_mirDraws;
    if (m.anyUav) c->OMSetRenderTargetsAndUnorderedAccessViews(m.nRtv, m.rtv, m.dsv, m.nRtv, 8 - m.nRtv, m.uav + m.nRtv, keep);
    else          OrigOMSetRTs(c)(c, m.nRtv, m.rtv, m.dsv);
    if (vsCh) c->VSSetShaderResources(0, 16, vs);
    if (psCh) c->PSSetShaderResources(0, 32, ps);
    if (vcCh) OrigVSSetCB(c)(c, 0, 14, vcb);
    if (pcCh) OrigPSSetCB(c)(c, 0, 14, pcb);
    for (int i = 0; i < 16; ++i) if (vs[i]) vs[i]->Release();
    for (int i = 0; i < 32; ++i) if (ps[i]) ps[i]->Release();
    for (int i = 0; i < 14; ++i) { if (vcb[i]) vcb[i]->Release(); if (pcb[i]) pcb[i]->Release(); }
    MirRelease(m);
    t_mirCtx = nullptr; t_mirStage = "done"; MirLeave();
}
struct MirDisp { ID3D11UnorderedAccessView* uav[8]; ID3D11UnorderedAccessView* tuav[8]; };
// BISECT 1 (2026-09-26): every crash is gameplay-only; the only mirrored op class absent at the title
// (disp=0 there) is compute. Compute mirroring OFF for this test build — nothing else changed.
static constexpr bool kMirrorCompute = false;
static bool MirrorPreDispatch(ID3D11DeviceContext* c, MirDisp& m) {
    if (!kMirrorCompute) return false;
    if (t_mirIn || !MirCtxOk(c) || !MirEnter()) return false;
    t_mirCtx = c; t_mirStage = "dispatch"; if (c != g_ctxImm) t_mirDefUser = 1;
    memset(&m, 0, sizeof(m));
    c->CSGetUnorderedAccessViews(0, 8, m.uav);
    bool any = false, ok = true;
    for (int i = 0; i < 8; ++i) if (m.uav[i]) { any = true;
        TwinRes* tr = nullptr; m.tuav[i] = (ID3D11UnorderedAccessView*)TwinViewOf(m.uav[i], 3, true, &tr); if (!m.tuav[i]) ok = false; else tr->written = g_mirFrame; }
    if (!any || !ok) { if (any) ++g_mirSkips; for (int i = 0; i < 8; ++i) if (m.uav[i]) m.uav[i]->Release(); t_mirCtx = nullptr; MirLeave(); return false; }
    return true;
}
template <class F> static void MirrorPostDispatch(ID3D11DeviceContext* c, MirDisp& m, F disp) {
    ID3D11ShaderResourceView *cs[32] = {}, *csT[32];
    c->CSGetShaderResources(0, 32, cs);
    bool csCh = MirSwapSRVs(cs, csT, 32);
    ID3D11Buffer *ccb[14] = {}, *ccbT[14]; c->CSGetConstantBuffers(0, 14, ccb); memcpy(ccbT, ccb, sizeof(ccb));
    bool ccCh = false; MirSwapCBs(ccbT, 14, ccCh);
    UINT keep[8]; for (int i = 0; i < 8; ++i) keep[i] = (UINT)-1;
    c->CSSetUnorderedAccessViews(0, 8, m.tuav, keep);
    if (csCh) c->CSSetShaderResources(0, 32, csT);
    if (ccCh) c->CSSetConstantBuffers(0, 14, ccbT);
    disp();
    ++g_mirDisp;
    c->CSSetUnorderedAccessViews(0, 8, m.uav, keep);
    if (csCh) c->CSSetShaderResources(0, 32, cs);
    if (ccCh) c->CSSetConstantBuffers(0, 14, ccb);
    for (int i = 0; i < 32; ++i) if (cs[i]) cs[i]->Release();
    for (int i = 0; i < 14; ++i) if (ccb[i]) ccb[i]->Release();
    for (int i = 0; i < 8; ++i) if (m.uav[i]) m.uav[i]->Release();
    t_mirCtx = nullptr; MirLeave();
}
// ---- mirrored immediate-context operations ----
static bool MirOn(ID3D11DeviceContext* c) { return g_mirrorActive && c == g_ctxImm && GetCurrentThreadId() == g_presentTid.load(std::memory_order_relaxed); }
static bool MirClearOk(ID3D11DeviceContext* c) { return MirCtxOk(c); }   // immediate (render thread) or deferred
// FIX: pick by the context's REAL type. 'not the game immediate' != deferred: VDXR's own immediate context
// (bring-up clears) was routed to the DEFERRED trampoline -> d3d11+0x17D035 null read -> OpenXR dead, black headset.
template <class T> static T PickDef(ID3D11DeviceContext* c, T def, T imm) { bool d = c && c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED; return (d && def) ? def : (imm ? imm : def); }
static bool SingleSub(ID3D11Resource* r) {
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) return true;
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d); return d.MipLevels == 1 && d.ArraySize == 1;
}
static PFN_CopyRes   CopyResFn(ID3D11DeviceContext* c)    { return PickDef(c, g_origDefCopyRes, g_origCopyRes); }
static PFN_CopySubReg CopySubRegFn(ID3D11DeviceContext* c) { return PickDef(c, g_origDefCopySubReg, g_origCopySubReg); }
static PFN_Resolve   ResolveFn(ID3D11DeviceContext* c)    { return PickDef(c, g_origDefResolve, g_origResolve); }
static PFN_GenMips   GenMipsFn(ID3D11DeviceContext* c)    { return PickDef(c, g_origDefGenMips, g_origGenMips); }
#include "p5r_batch.inl"
static void BCopyRes(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (IsDeferredContext(c)) { if (g_origDefCopyRes) g_origDefCopyRes(c, dst, src); else c->CopyResource(dst, src); }   // t_bBusy: hook skips its hazard
    else g_origCopyRes(c, dst, src);
}
// [RENDERSIZE] who creates the engine's widescreen 3D targets (stack -> the size computation)
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateTex2D)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
static PFN_CreateTex2D g_origCreateTex2D = nullptr;
static std::atomic<int> g_tex2DLogged{0};
static HRESULT STDMETHODCALLTYPE HookCreateTex2D(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc, const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
    if (desc && (desc->BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) && !P5RXr::IsInternalCall()
        && desc->Width * 9 == desc->Height * 16 && desc->Width >= 960 && g_tex2DLogged.load() < 12) {
        const int k = g_tex2DLogged.fetch_add(1);
        char where[96]; snprintf(where, sizeof(where), "create %ux%u fmt=%u bind=0x%X (#%d)", desc->Width, desc->Height, (unsigned)desc->Format, desc->BindFlags, k);
        if (k < 4 || desc->Width == 3840) LogStack(where); else Log("[rendersize] %s", where);
    }
    return g_origCreateTex2D(d, desc, init, out);
}
static void STDMETHODCALLTYPE HookClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefClearRTV,g_origClearRTV)(c,v,col); return; }
    BatchFlush(c, BW_CLEAR);
    auto o = PickDef(c, g_origDefClearRTV, g_origClearRTV);
    o(c, v, col);
    if (!MirClearOk(c) || !v) return;
    t_mirCtx = c; struct R { ~R() { t_mirCtx = nullptr; } } rr;
    if (c != g_ctxImm) g_mirDefClears.fetch_add(1, std::memory_order_relaxed);
    MirGuard mg; if (!mg.on) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return; r->Release();
    TwinRes* t = TwinResOf(r, true); if (!t) return;
    if (SingleSub(r)) t->synced = g_mirFrame;           // full clear == fresh content, no copy needed
    auto* tv = (ID3D11RenderTargetView*)TwinViewOf(v, 0, true); if (!tv) return;
    o(c, tv, col); t->written = g_mirFrame; ++g_mirOps;
}
static void STDMETHODCALLTYPE HookClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT fl, FLOAT d, UINT8 st) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefClearDSV,g_origClearDSV)(c,v,fl,d,st); return; }
    BatchFlush(c, BW_CLEAR);
    auto o = PickDef(c, g_origDefClearDSV, g_origClearDSV);
    o(c, v, fl, d, st);
    if (!MirClearOk(c) || !v) return;
    t_mirCtx = c; struct R { ~R() { t_mirCtx = nullptr; } } rr;
    if (c != g_ctxImm) g_mirDefClears.fetch_add(1, std::memory_order_relaxed);
    MirGuard mg; if (!mg.on) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return; r->Release();
    TwinRes* t = TwinResOf(r, true); if (!t) return;
    if (SingleSub(r) && (fl & (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL)) == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL)) t->synced = g_mirFrame;
    auto* tv = (ID3D11DepthStencilView*)TwinViewOf(v, 1, true); if (!tv) return;
    o(c, tv, fl, d, st); t->written = g_mirFrame; ++g_mirOps;
}
static void STDMETHODCALLTYPE HookClearUAVu(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const UINT val[4]) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefClearUAVu,g_origClearUAVu)(c,v,val); return; }
    BatchFlush(c, BW_CLEAR);
    auto o = PickDef(c, g_origDefClearUAVu, g_origClearUAVu);
    o(c, v, val);
    if (!MirClearOk(c) || !v) return;
    t_mirCtx = c; struct R { ~R() { t_mirCtx = nullptr; } } rr;
    MirGuard mg; if (!mg.on) return;
    TwinRes* t = nullptr; auto* tv = (ID3D11UnorderedAccessView*)TwinViewOf(v, 3, true, &t); if (!tv) return;
    o(c, tv, val); t->written = g_mirFrame; ++g_mirOps;
}
static void STDMETHODCALLTYPE HookClearUAVf(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* v, const FLOAT val[4]) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefClearUAVf,g_origClearUAVf)(c,v,val); return; }
    BatchFlush(c, BW_CLEAR);
    auto o = PickDef(c, g_origDefClearUAVf, g_origClearUAVf);
    o(c, v, val);
    if (!MirClearOk(c) || !v) return;
    t_mirCtx = c; struct R { ~R() { t_mirCtx = nullptr; } } rr;
    MirGuard mg; if (!mg.on) return;
    TwinRes* t = nullptr; auto* tv = (ID3D11UnorderedAccessView*)TwinViewOf(v, 3, true, &t); if (!tv) return;
    o(c, tv, val); t->written = g_mirFrame; ++g_mirOps;
}
static void STDMETHODCALLTYPE HookCopyRes(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (BypassGameHooks(c)) { CopyResFn(c)(c,dst,src); return; }
    BatchHazardCopy(c, dst, src);
    CopyResFn(c)(c, dst, src);
    if (dst && g_dupOn.load(std::memory_order_relaxed)) {
        D3D11_RESOURCE_DIMENSION dd; dst->GetType(&dd);
        if (dd == D3D11_RESOURCE_DIMENSION_BUFFER) if (ID3D11Buffer* tw = CamBufRightIfCam((ID3D11Buffer*)dst)) { CopyResFn(c)(c, tw, src); g_cbCopyMir.fetch_add(1, std::memory_order_relaxed); }
    }
    if (!MirOn(c) || !dst || !src) return;
    MirGuard mg; if (!mg.on) return;
    TwinRes* ts = TwinResOf(src, false); if (ts && ts->written + 2 < g_mirFrame) ts = nullptr;
    TwinRes* td = TwinResOf(dst, ts != nullptr); if (!td) return;
    g_origCopyRes(c, td->twin, ts ? ts->twin : src); td->synced = td->written = g_mirFrame; ++g_mirOps;
}
static void STDMETHODCALLTYPE HookCopySubReg(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* b) {
    if (BypassGameHooks(c)) { CopySubRegFn(c)(c,dst,ds,x,y,z,src,ss,b); return; }
    BatchHazardCopy(c, dst, src);
    if (dst && g_dupOn.load(std::memory_order_relaxed)) {
        D3D11_RESOURCE_DIMENSION dd; dst->GetType(&dd);
        if (dd == D3D11_RESOURCE_DIMENSION_BUFFER) if (ID3D11Buffer* tw = CamBufRightIfCam((ID3D11Buffer*)dst)) { CopySubRegFn(c)(c, tw, ds, x, y, z, src, ss, b); g_cbCopyMir.fetch_add(1, std::memory_order_relaxed); }
    }
    if (MirOn(c) && dst && src) do {
        MirGuard mg; if (!mg.on) break;
        TwinRes* ts = TwinResOf(src, false); if (ts && ts->written + 2 < g_mirFrame) ts = nullptr;
        TwinRes* td = TwinResOf(dst, ts != nullptr);
        if (td) { MirSync(td, dst); g_origCopySubReg(c, td->twin, ds, x, y, z, ts ? ts->twin : src, ss, b); td->written = g_mirFrame; ++g_mirOps; }

    } while (0);
    CopySubRegFn(c)(c, dst, ds, x, y, z, src, ss, b);
}
static void STDMETHODCALLTYPE HookResolve(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, ID3D11Resource* src, UINT ss, DXGI_FORMAT f) {
    if (BypassGameHooks(c)) { ResolveFn(c)(c,dst,ds,src,ss,f); return; }
    BatchFlush(c, BW_COPY);
    if (MirOn(c) && dst && src) do {
        MirGuard mg; if (!mg.on) break;
        TwinRes* ts = TwinResOf(src, false); if (ts && ts->written + 2 < g_mirFrame) ts = nullptr;
        TwinRes* td = TwinResOf(dst, ts != nullptr);
        if (td) { MirSync(td, dst); g_origResolve(c, td->twin, ds, ts ? ts->twin : src, ss, f); td->written = g_mirFrame; ++g_mirOps; }

    } while (0);
    ResolveFn(c)(c, dst, ds, src, ss, f);
}
static void STDMETHODCALLTYPE HookGenMips(ID3D11DeviceContext* c, ID3D11ShaderResourceView* v) {
    if (BypassGameHooks(c)) { GenMipsFn(c)(c,v); return; }
    BatchFlush(c, BW_COPY);
    GenMipsFn(c)(c, v);
    if (!MirOn(c) || !v) return;
    MirGuard mg; if (!mg.on) return;
    auto* tv = (ID3D11ShaderResourceView*)TwinViewOf(v, 2, false); if (tv) { g_origGenMips(c, tv); ++g_mirOps; }
}
// [HEADVR v2] at a 4K display every >=2000px target counts as "scene" (shadow atlas 3072x3264, post
// chain, composite) and their draws trashed the twin (black eye). Only duplicate draws that actually
// consume the MAIN camera CB (the buffer that received the main-camera upload) on VS b2.
static std::atomic<uint32_t> g_dupSkipNotCam{0};
static bool DrawUsesMainCam(ID3D11DeviceContext* c) {
    void* want = g_mainCamBuf.load(std::memory_order_relaxed);
    if (!want) return false;
    ID3D11Buffer* b2 = nullptr; c->VSGetConstantBuffers(2, 1, &b2);
    bool ok = (b2 == want);
    if (b2) b2->Release();
    if (!ok) g_dupSkipNotCam.fetch_add(1, std::memory_order_relaxed);
    return ok;
}
#include "p5r_hud_settings.h"
#include "p5r_culling.h"
#include "p5r_hud.inl"
#include "p5r_gpuprof.inl"
static void STDMETHODCALLTYPE HookDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b) {
    if (BypassGameHooks(c)) { OrigDrawIndexed(c)(c,n,s,b); return; }
    PerfScope perfDraw(g_perfDraw);
    if (c == g_ctxImm) g_immDraws.fetch_add(1, std::memory_order_relaxed);
    g_cDrawIdx.fetch_add(1, std::memory_order_relaxed);
    CensusDraw(c);
    if (g_stackArm.load(std::memory_order_relaxed) && !g_didDrawStack && g_didRtStack && g_inMainPass.load(std::memory_order_relaxed)) {
        g_didDrawStack = true; g_stackArm.store(false, std::memory_order_relaxed); LogStack("first-scene-draw"); }
    if (g_capturing.load(std::memory_order_relaxed)) { SnapshotMainCam(c); CapDraw(n); }
    HudState hud; { PerfPart pp(g_perfHud); HudPre(c, true, n, hud); }
    GpuProfOnDraw();
    MirDraw md; bool mir = !hud.redirect && !hud.skip && MirrorPreDraw(c, md);
    BarSave bsv; const bool barT = !hud.skip && BarTestBegin(c, hud, bsv); if (barT) { GameCall gc; OrigDrawIndexed(c)(c,n,s,b); BarTestEnd(c, bsv); }   // [BARPRED]
    if (!hud.skip) { HeadGpuBeforeDraw(c, 0); GameCall gc; OrigDrawIndexed(c)(c,n,s,b); }
    { CovSave cs; if (!hud.skip && HudCoverBegin(c, hud, cs)) { GameCall gc; OrigDrawIndexed(c)(c,n,s,b); HudCoverEnd(c, cs); } }   // [COVREPLAY]
    if (barT) BarPredClear(c);
    if (mir) { PerfScope perfMirror(g_perfMirror); GpuProfMirror(c, true); MirrorDraw(c, md, BCall{0,n,s,0,0,b}, [&]{ OrigDrawIndexed(c)(c,n,s,b); }); GpuProfMirror(c, false); }
    { PerfPart pp(g_perfHud); HudPost(c, hud); }

    if (g_dupOn.load(std::memory_order_relaxed) && !g_stereoPaused.load(std::memory_order_relaxed)) {
        if (!g_mirrorOn && !g_ecl2On.load(std::memory_order_relaxed) && CtxInScene(c) && n > 6 && DrawUsesMainCam(c)) DuplicateDrawIndexed(c,n,s,b,true);
        else if (CtxInUI(c)) {
            g_uiPassDraws.fetch_add(1, std::memory_order_relaxed);
            uint32_t lo=g_uiIdxMin.load(std::memory_order_relaxed); while (n<lo && !g_uiIdxMin.compare_exchange_weak(lo,n)) {}
            uint32_t hi=g_uiIdxMax.load(std::memory_order_relaxed); while (n>hi && !g_uiIdxMax.compare_exchange_weak(hi,n)) {}
            if (g_dupUI.load(std::memory_order_relaxed)) {
                DuplicateDrawIndexed(c,n,s,b,false); g_cDupUI.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    MaybeSubmitFallbackFrame();
}
static void STDMETHODCALLTYPE HookDraw(ID3D11DeviceContext* c, UINT n, UINT s) {
    if (BypassGameHooks(c)) { OrigDraw(c)(c,n,s); return; }
    PerfScope perfDraw(g_perfDraw);
    if (c == g_ctxImm) g_immDraws.fetch_add(1, std::memory_order_relaxed);
    g_cDraw.fetch_add(1, std::memory_order_relaxed);
    CensusDraw(c);

    if (g_capturing.load(std::memory_order_relaxed)) CapDraw(n);
    HudState hud; { PerfPart pp(g_perfHud); HudPre(c, false, n, hud); }
    GpuProfOnDraw();
    MirDraw md; bool mir = !hud.redirect && !hud.skip && MirrorPreDraw(c, md);
    BarSave bsv; const bool barT = !hud.skip && BarTestBegin(c, hud, bsv); if (barT) { GameCall gc; OrigDraw(c)(c,n,s); BarTestEnd(c, bsv); }   // [BARPRED]
    if (!hud.skip) { HeadGpuBeforeDraw(c, 0); GameCall gc; OrigDraw(c)(c,n,s); }
    { CovSave cs; if (!hud.skip && HudCoverBegin(c, hud, cs)) { GameCall gc; OrigDraw(c)(c,n,s); HudCoverEnd(c, cs); } }   // [COVREPLAY]
    if (barT) BarPredClear(c);
    if (mir) { PerfScope perfMirror(g_perfMirror); GpuProfMirror(c, true); MirrorDraw(c, md, BCall{1,n,s,0,0,0}, [&]{ OrigDraw(c)(c,n,s); }); GpuProfMirror(c, false); }
    { PerfPart pp(g_perfHud); HudPost(c, hud); }
    MaybeSubmitFallbackFrame();
}
static void STDMETHODCALLTYPE HookDrawIdxInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
    if (BypassGameHooks(c)) { OrigDrawIdxInst(c)(c,a,b,d,e,f); return; }
    PerfScope perfDraw(g_perfDraw);
    if (c == g_ctxImm) g_immDraws.fetch_add(1, std::memory_order_relaxed);
    g_cDrawIdxInst.fetch_add(1, std::memory_order_relaxed);
    CensusDraw(c);

    if (g_capturing.load(std::memory_order_relaxed)) SnapshotMainCam(c);
    HudState hud; { PerfPart pp(g_perfHud); HudPre(c, true, a, hud); }
    GpuProfOnDraw();
    MirDraw md; bool mir = !hud.redirect && !hud.skip && MirrorPreDraw(c, md);
    BarSave bsv; const bool barT = !hud.skip && BarTestBegin(c, hud, bsv); if (barT) { GameCall gc; OrigDrawIdxInst(c)(c,a,b,d,e,f); BarTestEnd(c, bsv); }   // [BARPRED]
    if (!hud.skip) { HeadGpuBeforeDraw(c, 0); GameCall gc; OrigDrawIdxInst(c)(c,a,b,d,e,f); }
    { CovSave cs; if (!hud.skip && HudCoverBegin(c, hud, cs)) { GameCall gc; OrigDrawIdxInst(c)(c,a,b,d,e,f); HudCoverEnd(c, cs); } }   // [COVREPLAY]
    if (barT) BarPredClear(c);
    if (mir) { PerfScope perfMirror(g_perfMirror); GpuProfMirror(c, true); MirrorDraw(c, md, BCall{2,a,b,d,f,e}, [&]{ OrigDrawIdxInst(c)(c,a,b,d,e,f); }); GpuProfMirror(c, false); }
    { PerfPart pp(g_perfHud); HudPost(c, hud); } }
static void STDMETHODCALLTYPE HookDrawInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    if (BypassGameHooks(c)) { OrigDrawInst(c)(c,a,b,d,e); return; }
    PerfScope perfDraw(g_perfDraw);
    if (c == g_ctxImm) g_immDraws.fetch_add(1, std::memory_order_relaxed);
    g_cDrawInst.fetch_add(1, std::memory_order_relaxed);
    CensusDraw(c);
    if (g_capturing.load(std::memory_order_relaxed)) SnapshotMainCam(c);
    HudState hud; { PerfPart pp(g_perfHud); HudPre(c, false, a, hud); }
    GpuProfOnDraw();
    MirDraw md; bool mir = !hud.redirect && !hud.skip && MirrorPreDraw(c, md);
    BarSave bsv; const bool barT = !hud.skip && BarTestBegin(c, hud, bsv); if (barT) { GameCall gc; OrigDrawInst(c)(c,a,b,d,e); BarTestEnd(c, bsv); }   // [BARPRED]
    if (!hud.skip) { HeadGpuBeforeDraw(c, 0); GameCall gc; OrigDrawInst(c)(c,a,b,d,e); }
    { CovSave cs; if (!hud.skip && HudCoverBegin(c, hud, cs)) { GameCall gc; OrigDrawInst(c)(c,a,b,d,e); HudCoverEnd(c, cs); } }   // [COVREPLAY]
    if (barT) BarPredClear(c);
    if (mir) { PerfScope perfMirror(g_perfMirror); GpuProfMirror(c, true); MirrorDraw(c, md, BCall{3,a,b,d,e,0}, [&]{ OrigDrawInst(c)(c,a,b,d,e); }); GpuProfMirror(c, false); }
    { PerfPart pp(g_perfHud); HudPost(c, hud); } }
typedef void (STDMETHODCALLTYPE* PFN_DrawIndirect)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
static PFN_DrawIndirect g_origDIII = nullptr, g_origDefDIII = nullptr, g_origDII = nullptr, g_origDefDII = nullptr;
static std::atomic<uint32_t> g_cIndirect{0};
static ID3D11Buffer* MirArgs(ID3D11Buffer* a) {   // args written on the GPU for the right eye? use its twin
    TwinRes* t = TwinResOf(a, false);
    return (t && t->written + 2 >= g_mirFrame && t->twin) ? (ID3D11Buffer*)t->twin : a;
}
static void STDMETHODCALLTYPE HookDIII(ID3D11DeviceContext* c, ID3D11Buffer* a, UINT off) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefDIII,g_origDIII)(c,a,off); return; }
    PFN_DrawIndirect o = (IsDeferredContext(c) && g_origDefDIII) ? g_origDefDIII : g_origDIII;
    g_cIndirect.fetch_add(1, std::memory_order_relaxed);
    MirDraw md; bool mir = MirrorPreDraw(c, md);
    o(c, a, off);
    if (mir) { BatchFlush(c, BW_DIRECT); MirrorPostDraw(c, md, [&]{ o(c, MirArgs(a), off); }); }
}
static void STDMETHODCALLTYPE HookDII(ID3D11DeviceContext* c, ID3D11Buffer* a, UINT off) {
    if (BypassGameHooks(c)) { PickDef(c,g_origDefDII,g_origDII)(c,a,off); return; }
    PFN_DrawIndirect o = (IsDeferredContext(c) && g_origDefDII) ? g_origDefDII : g_origDII;
    g_cIndirect.fetch_add(1, std::memory_order_relaxed);
    MirDraw md; bool mir = MirrorPreDraw(c, md);
    o(c, a, off);
    if (mir) { BatchFlush(c, BW_DIRECT); MirrorPostDraw(c, md, [&]{ o(c, MirArgs(a), off); }); }
}
static void STDMETHODCALLTYPE HookDispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) {
    if (BypassGameHooks(c)) { OrigDispatch(c)(c,x,y,z); return; }
    BatchFlush(c, BW_DISPATCH);
    g_cDispatch.fetch_add(1, std::memory_order_relaxed);
    if (g_capturing.load(std::memory_order_relaxed)) CapDispatch();
    MirDisp mdp; bool mir = MirrorPreDispatch(c, mdp);
    OrigDispatch(c)(c,x,y,z);
    if (mir) MirrorPostDispatch(c, mdp, [&]{ OrigDispatch(c)(c,x,y,z); }); }
static void STDMETHODCALLTYPE HookVSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { OrigVSSetCB(c)(c,s,n,b); return; }
    PerfPart pp(g_perfSetCBHook);
    g_cVSSet.fetch_add(1, std::memory_order_relaxed); CensusCB(g_vs, s, n, b); RecordBound(s,n,b,0); GameCall gc; OrigVSSetCB(c)(c,s,n,b); }
static void STDMETHODCALLTYPE HookPSSetCB(ID3D11DeviceContext* c, UINT s, UINT n, ID3D11Buffer* const* b) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { OrigPSSetCB(c)(c,s,n,b); return; }
    PerfPart pp(g_perfSetCBHook);
    g_cPSSet.fetch_add(1, std::memory_order_relaxed); CensusCB(g_ps, s, n, b); RecordBound(s,n,b,1); GameCall gc; OrigPSSetCB(c)(c,s,n,b); }
struct MappedCB { ID3D11Resource* r; void* p; UINT size; };
static thread_local MappedCB t_mapped[8]; static thread_local int t_mappedN = 0;
typedef void (STDMETHODCALLTYPE* PFN_Unmap)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
static PFN_Unmap g_origUnmap = nullptr, g_origDefUnmap = nullptr;
static PFN_Unmap OrigUnmap(ID3D11DeviceContext* c) { return (IsDeferredContext(c) && g_origDefUnmap) ? g_origDefUnmap : (g_origUnmap ? g_origUnmap : g_origDefUnmap); }
static void BUnmap(ID3D11DeviceContext* c, ID3D11Resource* r) { OrigUnmap(c)(c, r, 0); }
// [BATCH] write a constant buffer's right-eye twin: pending draws keep its old contents, the new ones are remembered
static void TwinWrite(ID3D11DeviceContext* c, ID3D11Buffer* tw, const void* data, UINT size) {
    BatchHazard(c, tw, BW_TEXWRITE); OrigUpdateSubres(c)(c, tw, 0, nullptr, data, 0, 0); BatchNote(c, tw, data, size);
}
static void STDMETHODCALLTYPE HookUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { OrigUnmap(c)(c,r,sub); return; }
    PerfPart pp(g_perfUnmapHook);
    int idx = -1;
    for (int i = 0; i < t_mappedN; ++i) if (t_mapped[i].r == r) { idx = i; break; }
    if (idx < 0) { GameCall gc; OrigUnmap(c)(c, r, sub); return; }
    MappedCB m = t_mapped[idx]; t_mapped[idx] = t_mapped[--t_mappedN];
    if (GfdCam::EventRunning()) CbChangeNote(r, m.p, m.size);   // [CBCHANGE] game bytes, before any patch
    static thread_local float L[4096], R[4096];
    memcpy(L, m.p, m.size); memcpy(R, L, m.size);
    if (IsMainCameraCB(L, m.size) && !g_stereoPaused.load(std::memory_order_relaxed)) {
        static thread_local float original[4096]; memcpy(original, L, m.size);
        HeadXf hx; P5RXr::Pose used{}; const bool haveHead = BuildHeadXf(hx, used);
        const float half = CameraControls::HalfEye(CameraControls::Get());
        InjectEyeShift(L, -half, haveHead ? &hx : nullptr, &used.renderedFov[0]);
        InjectEyeShift(R, +half, haveHead ? &hx : nullptr, &used.renderedFov[1]);
        used.renderedFovValid = haveHead;
        memcpy(used.renderedView[0], L+16, 64); memcpy(used.renderedView[1], R+16, 64); used.renderedViewValid = haveHead;
        CamTraceNote(c, 1, haveHead, used, original+16, L+16);   // [CAMTRACE]
        CamSetStore(original, L, R, haveHead ? &used : nullptr);
        PatchMainCameraHistory(original, L, R, m.size);
        ShotHold(original, L, R, m.size);   // [SHOTHOLD]
        g_mainCamBuf.store(r, std::memory_order_relaxed);
        g_camCBWanted.store(m.size, std::memory_order_relaxed);
        P5RXr::SetGameProjection(g_capM00.load(), g_capM11.load());
        if (haveHead) { if (!g_exactTag.load(std::memory_order_relaxed)) P5RXr::SetRenderPose(&used); NoteFramePose(c, used); ++g_headApplied; }
        memcpy(m.p, L, m.size);
        { GameCall gc; OrigUnmap(c)(c, r, sub); } BatchNote(c, r, L, m.size);
        if (ID3D11Buffer* right = CamBufRight((ID3D11Buffer*)r, m.size)) {
            TwinWrite(c, right, R, m.size); CamBufMark((ID3D11Buffer*)r, true);
        }
        if (g_rightCamCB && g_camCBSize == m.size) TwinWrite(c, g_rightCamCB, R, m.size);
        ++g_cbHitMap; ++g_cInject;
        return;
    }
    int hits = CamPayloadHas(L, m.size) ? PatchCamPayload(L, R, m.size) : 0;
    if (hits) memcpy(m.p, L, m.size);                       // left eye goes to the game's own buffer
    { GameCall gc; OrigUnmap(c)(c, r, sub); } BatchNote(c, r, L, m.size);
    if (!hits) {
        if (ID3D11Buffer* tw = CamBufRightIfCam((ID3D11Buffer*)r)) { TwinWrite(c, tw, L, m.size); g_cbMirrorUpd.fetch_add(1, std::memory_order_relaxed); }
        return;
    }
    g_cbHitMap.fetch_add(1, std::memory_order_relaxed);
    if (ID3D11Buffer* rb = CamBufRight((ID3D11Buffer*)r, m.size)) { TwinWrite(c, rb, R, m.size); CamBufMark((ID3D11Buffer*)r, true); }
}
static HRESULT STDMETHODCALLTYPE HookMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { return OrigMap(c)(c,r,sub,type,flags,out); }
    PerfPart pp(g_perfMapHook);
    if (type != D3D11_MAP_READ && type != D3D11_MAP_WRITE_NO_OVERWRITE) BatchHazard(c, r, BW_TEXWRITE);   // [BATCH]
    HRESULT hr; { GameCall gc; hr = OrigMap(c)(c, r, sub, type, flags, out); }
    if (SUCCEEDED(hr) && r) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && out && out->pData && g_dupOn.load(std::memory_order_relaxed)
            && (type == D3D11_MAP_WRITE_DISCARD || type == D3D11_MAP_WRITE_NO_OVERWRITE) && t_mappedN < 8) {
            D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d);
            if ((d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && d.ByteWidth >= 64 && d.ByteWidth <= 16384)
                t_mapped[t_mappedN++] = { r, out->pData, d.ByteWidth };   // [CAMCB] scanned + patched at Unmap
        }
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            g_cMapBuf.fetch_add(1, std::memory_order_relaxed);
            if (type == D3D11_MAP_WRITE_DISCARD) g_cMapDiscard.fetch_add(1, std::memory_order_relaxed);
        } else g_cMapOther.fetch_add(1, std::memory_order_relaxed);
    }
    return hr;
}
static bool CamCBOnUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) {
    if (!g_dupOn.load(std::memory_order_relaxed) || !data || ((uintptr_t)data & 3)) return false;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < 64 || d.ByteWidth > 16384) return false;
    ID3D11Buffer* twin = CamBufRightIfCam((ID3D11Buffer*)r);
    if (box) {   // partial upload: no patching, but keep an existing twin byte-identical
        if (twin) { OrigUpdateSubres(c)(c, r, sub, box, data, rowPitch, depthPitch); OrigUpdateSubres(c)(c, twin, 0, box, data, rowPitch, depthPitch); g_cbMirrorUpd.fetch_add(1, std::memory_order_relaxed); return true; }
        return false;
    }
    if (IsMainCameraCB((const float*)data, d.ByteWidth)) return false;   // handled by the main path
    static thread_local float L[4096], R[4096];
    // [CPUFIX] ~2,900 uploads/frame, nearly none with a camera: look before copying
    const bool camera = CamPayloadHas((const float*)data, d.ByteWidth);
    if (camera) { memcpy(L, data, d.ByteWidth); memcpy(R, data, d.ByteWidth); }
    if (!camera || !PatchCamPayload(L, R, d.ByteWidth)) {
        if (twin) { { PerfPart pg(g_perfUpdGame); OrigUpdateSubres(c)(c, r, sub, nullptr, data, rowPitch, depthPitch); } BatchNote(c, r, data, d.ByteWidth); TwinWrite(c, twin, data, d.ByteWidth); g_cbMirrorUpd.fetch_add(1, std::memory_order_relaxed); return true; }
        return false;
    }
    g_cbHitUpd.fetch_add(1, std::memory_order_relaxed);
    { PerfPart pg(g_perfUpdGame); OrigUpdateSubres(c)(c, r, sub, nullptr, L, rowPitch, depthPitch); } BatchNote(c, r, L, d.ByteWidth);
    if (ID3D11Buffer* rb = CamBufRight((ID3D11Buffer*)r, d.ByteWidth)) { TwinWrite(c, rb, R, d.ByteWidth); CamBufMark((ID3D11Buffer*)r, true); }
    return true;
}
#include "p5r_headgpu.inl"
static void STDMETHODCALLTYPE HookUpdateSubres(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rowPitch, UINT depthPitch) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { OrigUpdateSubres(c)(c,r,sub,box,data,rowPitch,depthPitch); return; }
    PerfPart pp(g_perfUpdHook);
    g_updCalls.fetch_add(1, std::memory_order_relaxed);
    if (r && data) { D3D11_RESOURCE_DIMENSION dm; r->GetType(&dm); if (dm == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd{}; ((ID3D11Buffer*)r)->GetDesc(&bd); g_updBytes.fetch_add(box ? box->right - box->left : bd.ByteWidth, std::memory_order_relaxed); } }
    { PerfPart ph(g_perfUpdHazard); BatchHazard(c, r, BW_TEXWRITE); }   // [BATCH] (covers its right-eye twin too)
    if (r && data && !box && GfdCam::EventRunning()) {   // [CBCHANGE]
        D3D11_RESOURCE_DIMENSION dm; r->GetType(&dm);
        if (dm == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd{}; ((ID3D11Buffer*)r)->GetDesc(&bd); if (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) CbChangeNote(r, data, bd.ByteWidth); }
    }
    bool camDone; { PerfPart pc(g_perfUpdCam); camDone = r && CamCBOnUpdate(c, r, sub, box, data, rowPitch, depthPitch); }
    if (camDone) return;   // [CAMCB]
    if (MirOn(c) && r && data) do {   // [MIRROR] CPU upload into a twinned resource: same bytes into the twin
        MirGuard mg; if (!mg.on) break;
        if (TwinRes* td = TwinResOf(r, false)) { MirSync(td, r); OrigUpdateSubres(c)(c, td->twin, sub, box, data, rowPitch, depthPitch); td->written = g_mirFrame; }
    } while (0);
    if (r && data) {
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            g_cUpdBuf.fetch_add(1, std::memory_order_relaxed);
            uint32_t seq = g_cUpdSeq.fetch_add(1, std::memory_order_relaxed);
            bool capturing = g_capturing.load(std::memory_order_relaxed);
            bool injecting = g_injectOn.load(std::memory_order_relaxed);
            bool arm = g_stackArm.load(std::memory_order_relaxed);
            bool dup = g_dupOn.load(std::memory_order_relaxed)
                       && !g_stereoPaused.load(std::memory_order_relaxed);
            if (capturing || g_censusFrame || injecting || arm || dup) {
                D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d);
                if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) {
                    UINT size = box ? (box->right - box->left) : d.ByteWidth;
                    bool aligned = ((uintptr_t)data & 3) == 0;
                    // render-fn finder: stack at the main-camera-CB upload (first correlation point)
                    if (arm && !g_didCamStack && aligned && size >= 128 && size <= 16384 && (!box || box->left==0)
                        && IsMainCameraCB((const float*)data, size)) {
                        g_didCamStack = true; LogStack("main-camera-upload");
                    }
                    if (capturing || g_censusFrame) {
                        if (aligned) {
                            if (capturing && size >= 64)          { CapUpload(r, size, (const float*)data); CacheCB(r, size, (const float*)data); }
                            else if (g_censusFrame && size >= 64 && (seq & 31)==0) ScanPayloadCensus(size, (const float*)data);
                        }
                        { std::lock_guard<std::mutex> lk(g_upMx); g_updSizeHist[size]++; }
                    }
                    bool isMainCam = aligned && size >= 128 && size <= 16384 && (!box || box->left == 0)
                                     && IsMainCameraCB((const float*)data, size);
                    // STEREO: left eye -> game CB (-half IPD); right eye -> our g_rightCamCB (+half IPD)
                    if (isMainCam) g_mainCamBuf.store(r, std::memory_order_relaxed);
                    if (isMainCam && dup) {
                        float half = CameraControls::HalfEye(CameraControls::Get());   // per-eye ± half-separation
                        static thread_local uint8_t leftS[16384], rightS[16384];
                        memcpy(leftS, data, size); memcpy(rightS, data, size);
                        // [HEADVR] one head pose for both eyes, tagged onto the submitted frame
                        HeadXf hx; P5RXr::Pose used{};
                        bool haveHead = BuildHeadXf(hx, used);
                        InjectEyeShift((float*)leftS, -half, haveHead ? &hx : nullptr, &used.renderedFov[0]);
                        InjectEyeShift((float*)rightS, +half, haveHead ? &hx : nullptr, &used.renderedFov[1]);
                        used.renderedFovValid = haveHead;
                        memcpy(used.renderedView[0], (float*)leftS+16, 64); memcpy(used.renderedView[1], (float*)rightS+16, 64); used.renderedViewValid = haveHead;
                        CamTraceNote(c, 2, haveHead, used, (const float*)data+16, (const float*)leftS+16);   // [CAMTRACE]
                        if (haveHead) { if (!g_exactTag.load(std::memory_order_relaxed)) P5RXr::SetRenderPose(&used); NoteFramePose(c, used); g_headApplied.fetch_add(1, std::memory_order_relaxed); }
                        CamSetStore((const float*)data, (const float*)leftS, (const float*)rightS, haveHead ? &used : nullptr);   // [CAMCB]
                        PatchMainCameraHistory((const float*)data, (float*)leftS, (float*)rightS, size);
                        ShotHold((const float*)data, (float*)leftS, (float*)rightS, size);   // [SHOTHOLD]
                        P5RXr::SetGameProjection(g_capM00.load(std::memory_order_relaxed), g_capM11.load(std::memory_order_relaxed));
                        // v18: request the buffer, never create it here — this hook runs on the
                        // game's worker threads (FC3 lesson, Hinny tha GOAT: no allocation inside a hooked call).
                        // Until the render thread builds it, the left eye is still correct.
                        g_camCBWanted.store(size, std::memory_order_relaxed);
                        // Camera pools / multiple camera passes must not all share the last upload.
                        if (ID3D11Buffer* right = CamBufRight((ID3D11Buffer*)r, size)) {
                            if (!box) TwinWrite(c, right, rightS, size); else { BatchHazard(c, right, BW_TEXWRITE); OrigUpdateSubres(c)(c, right, 0, nullptr, rightS, 0, 0); }
                            CamBufMark((ID3D11Buffer*)r, true);
                        }
                        g_cInject.fetch_add(1, std::memory_order_relaxed);
                        if (g_ecl2On.load(std::memory_order_relaxed) && g_rightCamCB && g_camCBSize == size
                            && !IsImmediateCtx(c)) {
                            // [ECL2] record "copy camera from our source" instead of baking the data in
                            {
                                std::lock_guard<std::mutex> lk(g_eclMx);
                                CamTag& t = g_ctxCam[c];
                                t.has = true; t.size = size;
                                t.left.assign(leftS, leftS + size); t.right.assign(rightS, rightS + size);
                            }
                            D3D11_BUFFER_DESC dd{}; ((ID3D11Buffer*)r)->GetDesc(&dd);
                            if (!box && dd.ByteWidth == size) c->CopyResource(r, g_rightCamCB);
                            else { D3D11_BOX sb{0,0,0,size,1,1}; c->CopySubresourceRegion(r, sub, box ? box->left : 0, 0, 0, g_rightCamCB, 0, &sb); }
                            return;
                        }
                        if (!g_ecl2On.load(std::memory_order_relaxed) && g_rightCamCB && g_camCBSize == size) {
                            TwinWrite(c, g_rightCamCB, rightS, size); }
                        { PerfPart pg(g_perfUpdGame); OrigUpdateSubres(c)(c, r, sub, box, leftS, rowPitch, depthPitch); }
                        if (!box) BatchNote(c, r, leftS, size);
                        return;
                    }
                    // sway test (F1): single eye shift on the game CB
                    if (isMainCam && injecting) {
                        static thread_local uint8_t scratch[16384];
                        memcpy(scratch, data, size);
                        float mag = (float)g_shiftUnits.load(std::memory_order_relaxed);
                        float dsh = g_swayOn.load(std::memory_order_relaxed)
                                    ? mag * sinf((float)g_frame.load(std::memory_order_relaxed) * 0.05f)
                                    : mag;
                        InjectEyeShift((float*)scratch, dsh);
                        g_cInject.fetch_add(1, std::memory_order_relaxed);
                        OrigUpdateSubres(c)(c, r, sub, box, scratch, rowPitch, depthPitch);
                        return;
                    }
                }
            }
        } else g_cUpdOther.fetch_add(1, std::memory_order_relaxed);
    }
    { PerfPart pg(g_perfUpdGame); OrigUpdateSubres(c)(c, r, sub, box, data, rowPitch, depthPitch); }
    if (r && data && !box) {   // [BATCH] remember full constant-buffer uploads
        PerfPart pn(g_perfUpdNote);
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC d{}; ((ID3D11Buffer*)r)->GetDesc(&d);
            if (d.BindFlags == D3D11_BIND_CONSTANT_BUFFER && d.ByteWidth <= 16384) BatchNote(c, r, data, d.ByteWidth); }
    }
}
static void STDMETHODCALLTYPE HookOMSetRTs(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    PerfScope perfState(g_perfState);
    if (BypassGameHooks(c)) { OrigOMSetRTs(c)(c,n,rtvs,dsv); return; }
    PerfPart pp(g_perfRTHook);
    BatchFlush(c, BW_PASS);                   // [BATCH] the pass ends: its right-eye draws, one target switch
    GpuProfOnRT(c, n, rtvs, dsv);             // [GPUPROF]
    g_cRTSet.fetch_add(1, std::memory_order_relaxed);
    // After the title AV the old Present path is gone, but context hooks still see the live frame.
    // Capture the full-size RGBA8 final target so the final draw can drive OpenXR directly.
    if (g_presentPathDead.load(std::memory_order_relaxed) && n && rtvs && rtvs[0] && g_lastSc) {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
                D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
                DXGI_SWAP_CHAIN_DESC sd{};
                bool finalTarget = SUCCEEDED(g_lastSc->GetDesc(&sd))
                    && td.Width == sd.BufferDesc.Width && td.Height == sd.BufferDesc.Height
                    && td.Format == sd.BufferDesc.Format;
                if (finalTarget) {
                    if (g_fallbackFinalTex != tex) {
                        if (g_fallbackFinalTex) g_fallbackFinalTex->Release();
                        g_fallbackFinalTex = tex;
                    } else tex->Release();
                    g_fallbackSubmitPending.store(true, std::memory_order_relaxed);
                } else tex->Release();
            }
            res->Release();
        }
    }
    // stereo dup: is a scene RT (>=2000 wide) bound? track it, and clear the right-eye twin once/frame.
    // MUST respect the resize pause: recreating + clearing the twin mid-mode-switch was the title-freeze
    // trigger window (twin released by resize, instantly rebuilt here, driver AV followed).
    // v22: describe the bound target ONCE — the census needs it on every bind, not only while
    // duplication is running.
    ID3D11RenderTargetView* r0=(n&&rtvs)?rtvs[0]:nullptr; UINT w=0, h=0; DXGI_FORMAT fmt=DXGI_FORMAT_UNKNOWN;
    if (r0 && (g_rtCensusOn.load(std::memory_order_relaxed) || g_dupOn.load(std::memory_order_relaxed))) {
        ID3D11Resource* res=nullptr; r0->GetResource(&res);
        if (res){ ID3D11Texture2D* t=nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&t))&&t){ D3D11_TEXTURE2D_DESC td{}; t->GetDesc(&td); w=td.Width; h=td.Height; fmt=td.Format; t->Release(); }
            res->Release(); }
    }
    if (CtxScene* hs = CtxSceneSlot(c)) {   // [HUD] remember RT0/depth for draw classification
        hs->rtW.store(r0 ? w : 0, std::memory_order_relaxed); hs->rtH.store(r0 ? h : 0, std::memory_order_relaxed);
        hs->rtFmt.store((uint32_t)fmt, std::memory_order_relaxed); hs->rtDsv.store(dsv != nullptr, std::memory_order_relaxed);
        void* rtTex = nullptr; if (r0) { ID3D11Resource* rr = nullptr; r0->GetResource(&rr); rtTex = rr; if (rr) rr->Release(); }
        hs->rtTex.store(rtTex, std::memory_order_relaxed);
    }
    if (g_rtCensusOn.load(std::memory_order_relaxed)) {
        int slot = r0 ? RtStatSlot((void*)r0, w, h, (uint32_t)fmt) : -1;
        if (CtxScene* cs = CtxSceneSlot(c)) cs->rtSlot.store(slot, std::memory_order_relaxed);
    }
    if (g_stereoPaused.load(std::memory_order_relaxed)) {
        g_inScenePass.store(false, std::memory_order_relaxed);
        if (CtxScene* s = CtxSceneSlot(c)) s->inScene.store(false, std::memory_order_relaxed);
    } else if (g_dupOn.load(std::memory_order_relaxed)) {
        bool scene = (w>=2000);
        bool ui = !scene && g_bbW && g_bbH && w==g_bbW && h==g_bbH && fmt==g_bbFmt;
        g_inScenePass.store(scene, std::memory_order_relaxed);   // diagnostics only
        if (CtxScene* s = CtxSceneSlot(c)) {
            s->inScene.store(scene, std::memory_order_relaxed);
            s->inUI.store(ui, std::memory_order_relaxed);
            if (scene) { s->w.store(w, std::memory_order_relaxed); s->h.store(h, std::memory_order_relaxed); }
        }
        if (scene) {
            // v18: record the scene-pass size for the twin's VIEWPORT, and ask the RENDER THREAD to
            // build the twin. Creating D3D resources inside a hooked call on a worker thread is the
            // FC3 nvwgf2um lesson — queue the create, never do it here.
            g_scenePassW.store(w, std::memory_order_relaxed);
            g_scenePassH.store(h, std::memory_order_relaxed);
            g_twinWanted.store(true, std::memory_order_relaxed);
            uint64_t fr=g_frame.load(std::memory_order_relaxed);
            if (g_sceneClearedFrame!=fr && g_rightRTV && g_rightDSV) { g_sceneClearedFrame=fr;
                const float clr[4]={0,0,0,1}; c->ClearRenderTargetView(g_rightRTV,clr); c->ClearDepthStencilView(g_rightDSV,D3D11_CLEAR_DEPTH,1.0f,0); } }
    }
    bool cap = g_capturing.load(std::memory_order_relaxed);
    bool arm = g_stackArm.load(std::memory_order_relaxed);
    if (cap || arm) {
        ID3D11RenderTargetView* rtv0 = (n && rtvs) ? rtvs[0] : nullptr;
        if (cap) CapRT(rtv0, dsv);
        // main scene pass detector: the 2400x1350 (125%-scale) HDR RT is the only one wider than 1920
        UINT w=0, h=0;
        if (rtv0) { ID3D11Resource* res=nullptr; rtv0->GetResource(&res);
            if (res) { ID3D11Texture2D* tex=nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td); w=td.Width; h=td.Height; tex->Release(); }
                res->Release(); } }
        bool main = (w >= 2000);
        bool was = g_inMainPass.exchange(main, std::memory_order_relaxed);
        if (main) { g_mainPassW.store(w, std::memory_order_relaxed); g_mainPassH.store(h, std::memory_order_relaxed);
            if (arm && !was && !g_didRtStack) { g_didRtStack = true; LogStack("scene-RT-bind"); }   // first pass entry this arm
        }
    }
    { GameCall gc; OrigOMSetRTs(c)(c, n, rtvs, dsv); }
}

// ------------------------------------------------------------------ status dump
static void DumpStatus() {
    Log("==== STATUS (F8) frame=%llu ====", (unsigned long long)g_frame.load());
    IDXGISwapChain* sc = g_lastSc;
    if (sc) { DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)))
            Log("[swapchain] %ux%u fmt=%d buffers=%u windowed=%d", d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, d.Windowed); }
    Log("[last-frame] drawIdx=%u draw=%u drawIdxInst=%u drawInst=%u dispatch=%u rtSets=%u",
        g_lDrawIdx, g_lDraw, g_lDrawIdxInst, g_lDrawInst, g_lDispatch, g_lRTSet);
    Log("[last-frame] mapBuf=%u (discard=%u) mapOther=%u updBuf=%u updOther=%u vsSet=%u psSet=%u",
        g_lMapBuf, g_lMapDiscard, g_lMapOther, g_lUpdBuf, g_lUpdOther, g_lVSSet, g_lPSSet);
    for (int stage = 0; stage < 2; ++stage) {
        StageCB& st = stage ? g_ps : g_vs;
        const char* nm = stage ? "PS" : "VS";
        std::lock_guard<std::mutex> lk(st.mx);
        for (int s = 0; s < kSlots; ++s) {
            uint32_t sets = st.slotSets[s].load(std::memory_order_relaxed);
            if (!sets) continue;
            char line[512]; int off = 0;
            off += snprintf(line + off, sizeof(line) - off, "[%s b%d] sets=%u distinct=%zu sizes:", nm, s, sets, st.distinct[s].size());
            int shown = 0;
            for (auto& kv : st.sizeHist[s]) { if (shown++ >= 10) { off += snprintf(line + off, sizeof(line) - off, " ..."); break; }
                off += snprintf(line + off, sizeof(line) - off, " %u(x%u)", kv.first, kv.second); }
            Log("%s", line);
        }
    }
    { std::lock_guard<std::mutex> lk(g_upMx);
      if (!g_updSizeHist.empty()) { char l[512]; int o=0; o+=snprintf(l+o,sizeof(l)-o,"[cb-upload]"); int n=0;
          for (auto& kv : g_updSizeHist){ if(n++>=14){o+=snprintf(l+o,sizeof(l)-o," ...");break;} o+=snprintf(l+o,sizeof(l)-o," %u(x%u)",kv.first,kv.second);} Log("%s", l); } }
    { std::lock_guard<std::mutex> lk(g_matMx);
      Log("[census matrix hist] %zu keys (sampled every 16th frame, every 32nd upload):", g_matHist.size());
      std::vector<std::pair<uint32_t,MatKey>> order;
      for (auto& kv : g_matHist) order.push_back({kv.second, kv.first});
      std::sort(order.begin(), order.end(), [](auto&a, auto&b){ return a.first>b.first; });
      int shown=0;
      for (auto& o : order) { if (shown++>=30) { Log("      ... (%zu more)", order.size()-30); break; }
          Log("    [%c] size=%5u off=0x%04X n=%u", o.second.cls, o.second.size, o.second.off, o.first); } }
    Log("==== END STATUS ====");
}
static void ResetHists() {
    for (int stage = 0; stage < 2; ++stage) {
        StageCB& st = stage ? g_ps : g_vs;
        std::lock_guard<std::mutex> lk(st.mx);
        for (int s = 0; s < kSlots; ++s) { st.slotSets[s].store(0); st.distinct[s].clear(); st.sizeHist[s].clear(); }
    }
    { std::lock_guard<std::mutex> lk(g_upMx); g_updSizeHist.clear(); }
    { std::lock_guard<std::mutex> lk(g_matMx); g_matHist.clear(); }
    Log("[key] histograms reset");
}

// ------------------------------------------------------------------ lazy context hooking at first Present
// MinHook is NOT thread-safe, and our hooks get installed from two threads at once during boot:
// the init thread (swapchain/factory hooks) races the game's device thread (deferred-context hooks
// via HookCreateDeferredContext). Concurrent MH_CreateHook/MH_EnableHook corrupts MinHook's hook
// table — that was the v7 frame-1 crash (jump to RIP=0 from a torn detour). Serialize every MH_* call.
static std::mutex g_mhMx;
static bool HookFn(void* target, void* detour, void** orig, const char* name) {
    std::lock_guard<std::mutex> mhLk(g_mhMx);
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { Log("[hook] %s CREATE FAILED (%d)", name, (int)s); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK && s != MH_ERROR_ENABLED) { Log("[hook] %s ENABLE FAILED (%d)", name, (int)s); return false; }
    char m[192]; ModOf(target, m, sizeof(m));
    Log("[hook] %s @ %p (%s)", name, target, m);
    return true;
}
// ================= D3D-level per-eye stereo — DRAW DUPLICATION (stable D3D hooks only) =================
// The scene renders once (left, via camera injection). For the right eye we re-issue every scene-pass
// draw into a right-eye twin RT with a right-eye camera CB. Pure D3D — the same hook family the proven
// camera injection uses (never crashes). NUMPAD9 = stereo dup on/off; NUMPAD8 = show right eye (debug).
// g_dev / g_ctxImm are declared near the deferred-context registry (needed there for classification)
static std::atomic<bool> g_viewRight{false};   // debug: blit the right-eye twin onto the backbuffer
static ID3D11Texture2D* g_rightRT=nullptr;
static ID3D11Texture2D* g_rightDepth=nullptr;
static UINT g_twW=0, g_twH=0;
// (declared earlier: g_bbW/g_bbH/g_bbFmt — needed by the UI-pass test in HookOMSetRTs)
// g_dupOn, g_inScenePass, g_rightCamCB, g_camCBSize, g_rightRTV, g_rightDSV, g_sceneClearedFrame are
// declared earlier (before HookDrawIndexed) so the earlier hooks (UpdateSubres/OMSetRTs) can use them.

static void EnsureRightCamCB(UINT size) {
    if (g_rightCamCB && g_camCBSize==size) return;
    if (g_rightCamCB) { g_rightCamCB->Release(); g_rightCamCB=nullptr; g_camCBSize=0; }
    if (!g_dev || size==0 || size>65536) return;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=size; bd.Usage=D3D11_USAGE_DEFAULT; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(g_dev->CreateBuffer(&bd,nullptr,&g_rightCamCB))) { g_camCBSize=size; Log("[dup] right cam CB %u bytes", size); }
}
static void EnsureRightRT() {
    if (g_rightRTV && g_rightDSV) return;
    if (!g_dev || !g_lastSc) return;
    DXGI_SWAP_CHAIN_DESC sd{}; if (FAILED(g_lastSc->GetDesc(&sd))) return;
    g_twW=sd.BufferDesc.Width; g_twH=sd.BufferDesc.Height; if (!g_twW||!g_twH) return;
    D3D11_TEXTURE2D_DESC td{}; td.Width=g_twW; td.Height=g_twH; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1; td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td,nullptr,&g_rightRT))) g_dev->CreateRenderTargetView(g_rightRT,nullptr,&g_rightRTV);
    D3D11_TEXTURE2D_DESC dd{}; dd.Width=g_twW; dd.Height=g_twH; dd.MipLevels=1; dd.ArraySize=1;
    dd.Format=DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count=1; dd.Usage=D3D11_USAGE_DEFAULT; dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(g_dev->CreateTexture2D(&dd,nullptr,&g_rightDepth))) g_dev->CreateDepthStencilView(g_rightDepth,nullptr,&g_rightDSV);
    Log("[dup] right-eye twin %ux%u RGBA8 rtv=%p dsv=%p", g_twW, g_twH, (void*)g_rightRTV, (void*)g_rightDSV);
}
static void TeardownDupForResize(uint64_t frame) {
    const bool hadResources = g_rightRTV || g_rightRT || g_rightDSV || g_rightDepth || g_rightCamCB;
    HRESULT fenceHr = S_FALSE;
    bool gpuDrained = false;
    DWORD waitedMs = 0;

    // ResizeBuffers and Present are proven to run on this same render thread. Remove every binding
    // touched by duplication, then wait for an EVENT query covering all commands issued so far.
    if (g_ctxImm && g_origOMSetRTs && g_origVSSetCB && g_origPSSetCB) {
        ID3D11Buffer* nullCB = nullptr;
        g_origOMSetRTs(g_ctxImm, 0, nullptr, nullptr);
        g_origVSSetCB(g_ctxImm, 2, 1, &nullCB);
        g_origPSSetCB(g_ctxImm, 2, 1, &nullCB);

        ID3D11Query* fence = nullptr;
        D3D11_QUERY_DESC qd{}; qd.Query = D3D11_QUERY_EVENT;
        HRESULT createHr = g_dev ? g_dev->CreateQuery(&qd, &fence) : E_POINTER;
        if (SUCCEEDED(createHr) && fence) {
            g_ctxImm->End(fence);
            g_ctxImm->Flush();
            const ULONGLONG start = GetTickCount64();
            BOOL done = FALSE;
            do {
                fenceHr = g_ctxImm->GetData(fence, &done, sizeof(done), 0);
                if (fenceHr == S_OK && done) { gpuDrained = true; break; }
                SwitchToThread();
                waitedMs = (DWORD)(GetTickCount64() - start);
            } while (waitedMs < 2000 && fenceHr == S_FALSE);
            fence->Release();
        } else {
            fenceHr = createHr;
            g_ctxImm->Flush();
        }
    }

    // v20: exclusive — no worker thread can be inside DuplicateDrawIndexed while these die.
    AcquireSRWLockExclusive(&g_dupLock);
    if (g_rightRTV) { g_rightRTV->Release(); g_rightRTV=nullptr; }
    if (g_rightRT)  { g_rightRT->Release();  g_rightRT=nullptr; }
    if (g_rightDSV) { g_rightDSV->Release(); g_rightDSV=nullptr; }
    if (g_rightDepth){ g_rightDepth->Release(); g_rightDepth=nullptr; }
    if (g_rightCamCB){ g_rightCamCB->Release(); g_rightCamCB=nullptr; }
    ReleaseSRWLockExclusive(&g_dupLock);
    g_camCBSize=0; g_twW=0; g_twH=0; g_sceneClearedFrame=0;
    g_inScenePass.store(false, std::memory_order_relaxed);
    // v18: every context must forget the scene pass too — a stale per-context flag would let a
    // worker keep duplicating into RTVs this teardown just released.
    for (auto& s : g_ctxScene) { s.inScene.store(false, std::memory_order_relaxed); s.inUI.store(false, std::memory_order_relaxed); }
    g_twinWanted.store(false, std::memory_order_relaxed);
    g_camCBWanted.store(0, std::memory_order_relaxed);
    Log("[dup] PRE-RESIZE teardown f=%llu hadResources=%d gpuDrained=%d fenceHr=0x%08X waited=%lums",
        (unsigned long long)frame, (int)hadResources, (int)gpuDrained, (unsigned)fenceHr, waitedMs);
}
// Re-issue one draw into the right eye. Uses ORIGINAL fn pointers throughout so it never re-enters our
// hooks (no recursion, no double-count). Saves/restores the game's RTs + camera CB slot around it.
// v20: is the FAULTING thread inside our duplication when the AV fires? The VEH reads this
// thread_local, so the next crash says outright whether our path is on the stack or not.
static thread_local int g_tlsInDup = 0;
static void DuplicateDrawIndexed(ID3D11DeviceContext* c, UINT n, UINT s, INT b, bool shiftCam) {
    if (!TryAcquireSRWLockShared(&g_dupLock)) return;   // teardown in progress: skip this draw
    struct DupGuard { ~DupGuard(){ g_tlsInDup = 0; ReleaseSRWLockShared(&g_dupLock); } } dupGuard;
    g_tlsInDup = shiftCam ? 1 : 2;
    if (!g_rightRTV || !g_rightDSV) return;
    if (shiftCam && !g_rightCamCB) return;
    if (shiftCam) g_cDupScene.fetch_add(1, std::memory_order_relaxed);
    ID3D11RenderTargetView* svRTV[8]={}; ID3D11DepthStencilView* svDSV=nullptr;
    c->OMGetRenderTargets(8, svRTV, &svDSV);
    ID3D11Buffer* svVS=nullptr; c->VSGetConstantBuffers(2,1,&svVS);
    ID3D11Buffer* svPS=nullptr; c->PSGetConstantBuffers(2,1,&svPS);
    // v18 ALIGNMENT FIX: the scene pass runs a 2400x1350 viewport, but the twin is the backbuffer's
    // 1920x1080 (that size is required for the XR CopyResource). Inheriting the game's viewport
    // mapped clip space onto a 2400x1350 rect and the RT kept only its top-left corner, so the right
    // eye's content sat right and low of the left eye's — exactly the reported misalignment.
    // Clip space is resolution independent, so a viewport matching the TWIN reproduces the same
    // framing at the twin's resolution. Scissor gets the same treatment (a scene-sized scissor
    // would re-crop what the viewport just fixed).
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT svVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    c->RSGetViewports(&nvp, svVP);
    UINT nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT svSC[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    c->RSGetScissorRects(&nsc, svSC);
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)g_twW; vp.Height = (float)g_twH;
    vp.MinDepth = (nvp && svVP[0].Width > 0.0f) ? svVP[0].MinDepth : 0.0f;
    vp.MaxDepth = (nvp && svVP[0].Width > 0.0f) ? svVP[0].MaxDepth : 1.0f;
    c->RSSetViewports(1, &vp);
    if (nsc) { D3D11_RECT sr{0, 0, (LONG)g_twW, (LONG)g_twH}; c->RSSetScissorRects(1, &sr); }
    OrigOMSetRTs(c)(c, 1, &g_rightRTV, g_rightDSV);
    if (shiftCam) {   // UI must sit at the same screen position in both eyes: no camera swap
        OrigVSSetCB(c)(c, 2,1,&g_rightCamCB);
        OrigPSSetCB(c)(c, 2,1,&g_rightCamCB);
    }
    OrigDrawIndexed(c)(c, n, s, b);
    OrigOMSetRTs(c)(c, 8, svRTV, svDSV);
    if (shiftCam) {
        OrigVSSetCB(c)(c, 2,1,&svVS);
        OrigPSSetCB(c)(c, 2,1,&svPS);
    }
    if (nvp) c->RSSetViewports(nvp, svVP);
    if (nsc) c->RSSetScissorRects(nsc, svSC);
    for (int i=0;i<8;i++) if (svRTV[i]) svRTV[i]->Release();
    if (svDSV) svDSV->Release(); if (svVS) svVS->Release(); if (svPS) svPS->Release();
}
static void PresentDebugBlitRight() {   // NUMPAD8: copy the right-eye twin onto the backbuffer to see it
    if (!g_viewRight.load(std::memory_order_relaxed) || !g_rightRT || !g_lastSc || !g_ctxImm) return;
    ID3D11Texture2D* bb=nullptr;
    if (SUCCEEDED(g_lastSc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC bd{}; bb->GetDesc(&bd);
        if (bd.Width==g_twW && bd.Height==g_twH) g_ctxImm->CopyResource(bb, g_rightRT);
        bb->Release();
    }
}
// ---- headset submission: bring up OpenXR (once the twin exists) and submit both eyes each frame ----
// Init is non-blocking (async bring-up thread polls for the HMD); IsRunning() flips true when the
// session is live, so the headset can be connected at any time and it'll start submitting within ~5s.
static bool g_xrTried=false, g_xrWasRunning=false;
// [PACETEST] crash bisect: VR OFF, frames paced to ~90 Hz by a sleep (mimics xrWaitFrame blocking).
static constexpr bool kPaceTestNoXr = false;   // pacing alone: 5 min, no crash
static void UpdateMirrorActivation(uint64_t frame) {
    const bool was = g_mirrorActive;
    // Draw count measures scene complexity, not whether a scene is safe for stereo.
    // Keep the camera/resources through quiet fixed-camera rooms; explicit resize pause
    // and XR readiness still control activation.
    g_mirrorActive = g_mirrorOn && g_ctxImm && g_dupOn.load(std::memory_order_relaxed)
        && (g_xrWasRunning || kPaceTestNoXr) && !g_stereoPaused.load(std::memory_order_relaxed)
        && g_rightCamCB && g_origCopyRes;
    if (was != g_mirrorActive) {
        Log("[mirror] %s f=%llu drawIdx=%u (camera/XR gate)", g_mirrorActive ? "ACTIVE" : "inactive", (unsigned long long)frame, g_lDrawIdx);
        if (was && !g_mirrorActive) MirrorFlushAll("explicit stereo/XR pause");
    }
}
static void PaceTo90() {
    static LARGE_INTEGER last{}, fq{}; if (!fq.QuadPart) QueryPerformanceFrequency(&fq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart) { double ms = 1000.0 * (now.QuadPart - last.QuadPart) / fq.QuadPart; if (ms < 11.1) Sleep((DWORD)(11.1 - ms)); }
    QueryPerformanceCounter(&last);
}
// [SCREEN] menu screen size/distance in meters ([Screen] WidthMeters / DistanceMeters).
static void ScreenPlacement(float& width, float& distance) {
    static float s_width = 0.f, s_distance = 0.f;
    if (s_width <= 0.f && CameraControls::iniPath[0]) {
        wchar_t text[32] = {};
        GetPrivateProfileStringW(L"Screen", L"WidthMeters", L"2.4", text, 32, CameraControls::iniPath);
        float w = (float)_wtof(text);
        GetPrivateProfileStringW(L"Screen", L"DistanceMeters", L"2.2", text, 32, CameraControls::iniPath);
        float d = (float)_wtof(text);
        s_width = (w >= 0.3f && w <= 20.f) ? w : 2.4f;
        s_distance = (d >= 0.5f && d <= 20.f) ? d : 2.2f;
        Log("[screen] placement width=%.2fm distance=%.2fm", s_width, s_distance);
    }
    if (s_width > 0.f) { width = s_width; distance = s_distance; }
}
#include "p5r_freeze.inl"
// [RENDERFIRST] Native frame ordering: wait/locate -> game camera upload/draw ->
// submit with that prediction -> wait/locate for the next render. The old order
// located at submission, then rendered toward that already-used display time.
static void PrepareNextHeadsetFrame() {
    bool prepared;
    { PerfScope perfWait(g_perfXrWait); prepared = P5RXr::PrepareRenderFrame(); }
    if (!prepared) return;
    // Read located eyes on the XR/render thread; upload hooks only read these atomics.
    float fillH=0, fillV=0;
    for(int eye=0;eye<2;++eye) {
        P5RXr::Fov lens{};
        if(P5RXr::GetEyeView(eye,nullptr,&lens)) {
            float h=fmaxf(fabsf(tanf(lens.angleLeft)),fabsf(tanf(lens.angleRight)));
            float v=fmaxf(fabsf(tanf(lens.angleUp)),fabsf(tanf(lens.angleDown)));
            if(std::isfinite(h) && std::isfinite(v) && h>0 && v>0) {fillH=fmaxf(fillH,h);fillV=fmaxf(fillV,v);}
        }
    }
    // [OVERSCAN] run 18 (47759710): the render covered EXACTLY the headset lens (spare border 0 deg) while
    // each frame's pose is 14-26 ms old at display, so the compositor's correction slid past the image
    // edge on ~95% of frames during head motion, by a different amount every frame (a held
    // frame was steady). Render and declare a wider frustum so the correction stays inside the picture.
    // 2026-10-06: 6 -> 2 deg. The head shake was fixed by the final-image pose association (ca4b59f),
    // not by this border; 6 deg cost ~26% px/deg. 2 deg keeps fast turns from showing the image edge.
    constexpr float kOverscanRad = 2.f * 3.14159265f / 180.f;
    if(fillH>0 && fillV>0) { fillH = tanf(atanf(fillH) + kOverscanRad); fillV = tanf(atanf(fillV) + kOverscanRad); }
    if(fillH>0 && fillV>0) {g_fillTanH.store(fillH);g_fillTanV.store(fillV);}
    if (!g_xrWasRunning) { g_xrWasRunning=true; Log("[xr] HEADSET LIVE — submitting both eyes"); }
    static uint32_t preparedCount = 0;
    if (++preparedCount == 1 || preparedCount % 300 == 0)
        Log("[renderfirst] ACTIVE: XR prediction prepared before next game render, count=%u serial=%u; submit reuses this prediction; extra 60fps hold=%s",
            preparedCount, P5RXr::HeadSerial(), g_paceOn.load() ? "ON" : "OFF");
}
struct NextHeadsetFrame {
    ~NextHeadsetFrame() {
        // The no-image path still pairs this render's begin/end before waiting again.
        if (P5RXr::HasRenderFrame()) P5RXr::SubmitEyes(nullptr, nullptr);
        PrepareNextHeadsetFrame();
    }
};
static void SubmitToHeadset() {
    PerfScope perfSubmit(g_perfSubmit);
    if (kPaceTestNoXr) { if (g_mirrorActive) PaceTo90(); return; }
    if (!g_dev || !g_lastSc || !g_bbW || !g_bbH) return;
    // The gameplay gate and menu own the VR lifecycle. Keeping an XR session and its D3D11 swapchains
    // alive through P5R's title-to-game device transition is what poisons the next NVIDIA
    // submission. Do not even initialize OpenXR until the transition is over and the user
    // explicitly requests stereo.
    if (!g_xrStartRequested.load(std::memory_order_relaxed)) return;
    // FULL XR quiesce across a resize: make ZERO OpenXR calls for the window. VDXR runs on the game's
    // shared D3D11 device; the crash (nvwgf2umx.dll+0x653E83, deterministic, only once the XR session is
    // live) is the runtime's GPU work colliding with the game's ResizeBuffers. 0-layer frames still call
    // xrBeginFrame/xrEndFrame (= runtime GPU work) so they did NOT help. Skip XR entirely instead.
    if (g_frame.load(std::memory_order_relaxed) < g_xrSuspendUntil.load(std::memory_order_relaxed)) {
        static uint64_t s_lastLog = 0; uint64_t f = g_frame.load(std::memory_order_relaxed);
        if (f - s_lastLog >= 30) { s_lastLog = f; Log("[xr] SUSPENDED (no XR calls) f=%llu until=%llu", (unsigned long long)f, (unsigned long long)g_xrSuspendUntil.load()); }
        return;
    }
    if (!g_xrTried && g_frame.load(std::memory_order_relaxed) > 300) {   // bring OpenXR up early, after boot resizes settle
        g_xrTried = true;
        P5RXr::SetEyeResolution(g_bbW, g_bbH);         // eye swapchains = backbuffer size (= twin) -> CopyResource ok
        P5RXr::SetGameCopyFn((void*)g_origCopyRes);   // hand-off copy bypasses the mirror hook
        bool started = P5RXr::Init(g_dev);
        Log("[xr] Init started=%d (async, eye res %ux%u, game device). Connect the headset / OpenXR runtime.", (int)started, g_bbW, g_bbH);
    }
    // Bootstrap only: this backbuffer predates the first XR prediction. Locate for
    // the NEXT render and let the compositor keep its last image for this boundary.
    if (!P5RXr::HasRenderFrame()) { PrepareNextHeadsetFrame(); return; }
    // Consume the prediction used by the camera, then wait/locate for the next
    // game render on EVERY exit (world, screen, freeze, missing backbuffer).
    NextHeadsetFrame nextFrame;
    // Resize danger window (first ~15 frames after the LAST resize): the game is mode-switching; do NO
    // GPU work at all (no GetBuffer, no CopyResource) — submit a paired 0-layer frame so the XR frame
    // loop stays alive and the compositor holds the last image. Mono submits resume for the rest of the
    // 90-frame pause, stereo when it expires. This was the title-freeze crash window.
    {
        uint64_t f = g_frame.load(std::memory_order_relaxed);
        uint64_t cd = g_resizeCooldownUntil.load(std::memory_order_relaxed);
        bool danger = (f + 75) < cd;   // within 15 frames of a 90-frame cooldown arm
        static bool wasDanger = false;
        if (danger != wasDanger) { wasDanger = danger; Log("[xr] resize hold %s (f=%llu)", danger?"ON — 0-layer frames":"OFF — submits resume", (unsigned long long)f); }
        if (danger) { P5RXr::SubmitEyes(nullptr, nullptr); return; }   // paired begin<->end, zero layers
    }
    ID3D11Texture2D* bb=nullptr;
    static bool s_screenWas = false;
    if (SUCCEEDED(g_lastSc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&bb)) && bb && g_screenView.load(std::memory_order_relaxed)) {
        // [SCREEN] game menu: the backbuffer (game camera, no fill/head/eye shift) on a flat quad
        P5RMenu::Draw(bb, nullptr);
        float width = 2.4f, distance = 2.2f; ScreenPlacement(width, distance);
        P5RXr::SubmitScreen(bb, width, distance, !s_screenWas);
        s_screenWas = true;
        bb->Release();
        return;
    }
    s_screenWas = false;
    if (bb) {
        // stereo once the twin is live; mono (both eyes = game frame) at menus and during resize pauses
        bool want = g_dupOn.load(std::memory_order_relaxed) && !g_stereoPaused.load(std::memory_order_relaxed);
        if (g_mirrorOn) {
            // [MIRROR] left = the game's backbuffer, right = its twin (full pipeline, right camera)
            ID3D11Texture2D* R = bb;
            if (want) {
                AcquireSRWLockShared(&g_twLock);
                auto it = g_twRes.find((ID3D11Resource*)bb);
                ReleaseSRWLockShared(&g_twLock);
                if (it != g_twRes.end() && !it->second.bad && it->second.written == g_mirFrame && it->second.twin) {
                    D3D11_TEXTURE2D_DESC a{}, b2{}; bb->GetDesc(&a); ((ID3D11Texture2D*)it->second.twin)->GetDesc(&b2);
                    if (a.Width == b2.Width && a.Height == b2.Height && a.Format == b2.Format) R = (ID3D11Texture2D*)it->second.twin;
                }
                if (R != bb) ++g_mirRightTwin; else ++g_mirRightFallback;
            }
            // [FREEZE] world not live (menus): the last live stereo frame instead of the game's still picture
            if (!FreezeSubmit(bb, R)) {
                P5RMenu::Draw(bb, R);
                auto eyes = CameraControls::SelectEyes(bb, R, CameraControls::Get()); SubmitWorldEyes(eyes.left, eyes.right);
            }
        } else if (g_ecl2On.load(std::memory_order_relaxed)) {
            // [ECL2] both eyes through the game's full pipeline; mono (bb both eyes) when unavailable
            ID3D11Texture2D* L = bb; ID3D11Texture2D* R = bb;
            if (Ecl2BuildEyes(bb, want)) { L = g_eyeTexL; R = g_eyeTexR; }
            P5RMenu::Draw(L, R);
            auto eyes = CameraControls::SelectEyes(L, R, CameraControls::Get()); P5RXr::SubmitEyes(eyes.left, eyes.right);
        } else {
            bool stereo = want && g_rightRT;
            ID3D11Texture2D* twinEye = stereo ? g_rightRT : bb;
            P5RMenu::Draw(bb, twinEye);
            auto eyes = CameraControls::SelectEyes(bb, twinEye, CameraControls::Get()); P5RXr::SubmitEyes(eyes.left, eyes.right);
        }
        bb->Release();
    }
}

static void MaybeSubmitFallbackFrame() {
    if (!g_presentPathDead.load(std::memory_order_relaxed)
        || !g_fallbackSubmitPending.exchange(false, std::memory_order_relaxed)) return;
    ID3D11Texture2D* left = g_fallbackFinalTex;
    if (!left) return;
    uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    g_stereoPaused.store(false, std::memory_order_relaxed);
    g_resizeCooldownUntil.store(0, std::memory_order_relaxed);
    g_xrSuspendUntil.store(0, std::memory_order_relaxed);
    if (GetCurrentThreadId() == g_presentTid.load(std::memory_order_relaxed)) BatchFlushEnd(g_ctxImm);   // [BATCH]
    if (!P5RXr::HasRenderFrame()) { PrepareNextHeadsetFrame(); return; }
    NextHeadsetFrame nextFrame;
    ID3D11Texture2D* right = (g_dupOn.load(std::memory_order_relaxed) && g_rightRT) ? g_rightRT : left;
    auto eyes = CameraControls::SelectEyes(left, right, CameraControls::Get()); P5RXr::SubmitEyes(eyes.left, eyes.right);
    static bool first = true;
    static uint64_t count = 0;
    ++count;
    if (first || (count % 120) == 0) {
        first = false;
        Log("[fallback] XR submit from final draw f=%llu count=%llu stereo=%d left=%p right=%p",
            (unsigned long long)f, (unsigned long long)count,
            (int)(right != left), (void*)left, (void*)right);
    }
}
static void HookContextMethods(ID3D11DeviceContext* ctx, bool deferred) {
    if (!ctx) return;
    void** vt = *(void***)ctx;
    char mod[192]{}; ModOf(vt[VT_CTX_DrawIndexed], mod, sizeof(mod));
    Log("[context] hooking %s ctx=%p type=%d DrawIndexed=%p (%s)",
        deferred ? "DEFERRED" : "IMMEDIATE", (void*)ctx, (int)ctx->GetType(),
        vt[VT_CTX_DrawIndexed], mod);
    if (deferred) {
        HookFn(vt[VT_CTX_DrawIndexed],  (void*)HookDrawIndexed,  (void**)&g_origDefDrawIndexed, "DCTX::DrawIndexed");
        HookFn(vt[VT_CTX_Draw],         (void*)HookDraw,         (void**)&g_origDefDraw,        "DCTX::Draw");
        HookFn(vt[VT_CTX_DrawIdxInst],  (void*)HookDrawIdxInst,  (void**)&g_origDefDrawIdxInst, "DCTX::DrawIndexedInstanced");
        HookFn(vt[VT_CTX_DrawInst],     (void*)HookDrawInst,     (void**)&g_origDefDrawInst,    "DCTX::DrawInstanced");
        HookFn(vt[VT_CTX_Dispatch],     (void*)HookDispatch,     (void**)&g_origDefDispatch,    "DCTX::Dispatch");
        HookFn(vt[VT_CTX_VSSetCB],      (void*)HookVSSetCB,      (void**)&g_origDefVSSetCB,     "DCTX::VSSetConstantBuffers");
        HookFn(vt[VT_CTX_PSSetCB],      (void*)HookPSSetCB,      (void**)&g_origDefPSSetCB,     "DCTX::PSSetConstantBuffers");
        HookFn(vt[VT_CTX_Map],          (void*)HookMap,          (void**)&g_origDefMap,         "DCTX::Map");
        HookFn(vt[15],                  (void*)HookUnmap,        (void**)&g_origDefUnmap,       "DCTX::Unmap");
        if (!g_origDefUnmap) g_origDefUnmap = g_origUnmap;
        HookFn(vt[VT_CTX_UpdateSubres], (void*)HookUpdateSubres, (void**)&g_origDefUpdateSubres,"DCTX::UpdateSubresource");
        HookFn(vt[VT_CTX_OMSetRTs],     (void*)HookOMSetRTs,     (void**)&g_origDefOMSetRTs,    "DCTX::OMSetRenderTargets");
        if (!g_origFinishCL) HookFn(vt[114], (void*)HookFinishCL, (void**)&g_origFinishCL, "DCTX::FinishCommandList");
        HookFn(vt[39], (void*)HookDIII, (void**)&g_origDefDIII, "DCTX::DrawIndexedInstancedIndirect");
        HookFn(vt[50], (void*)HookClearRTV,  (void**)&g_origDefClearRTV,  "DCTX::ClearRenderTargetView");
        HookFn(vt[51], (void*)HookClearUAVu, (void**)&g_origDefClearUAVu, "DCTX::ClearUnorderedAccessViewUint");
        HookFn(vt[52], (void*)HookClearUAVf, (void**)&g_origDefClearUAVf, "DCTX::ClearUnorderedAccessViewFloat");
        HookFn(vt[53], (void*)HookClearDSV,  (void**)&g_origDefClearDSV,  "DCTX::ClearDepthStencilView");
        HookFn(vt[40], (void*)HookDII,  (void**)&g_origDefDII,  "DCTX::DrawInstancedIndirect");
        // [BATCH] deferred copies must replay pending right-eye draws first. A code entry shared with the
        // immediate variant keeps one hook and one trampoline: alias only when the addresses match.
        {
            void* const at[4] = { vt[47], vt[46], vt[57], vt[54] };
            for (int i = 0; i < 4; ++i) g_defCopyAt[i] = at[i];
            if (!g_origDefCopyRes)    HookFn(at[0], (void*)HookCopyRes,    (void**)&g_origDefCopyRes,    "DCTX::CopyResource");
            if (!g_origDefCopySubReg) HookFn(at[1], (void*)HookCopySubReg, (void**)&g_origDefCopySubReg, "DCTX::CopySubresourceRegion");
            if (!g_origDefResolve)    HookFn(at[2], (void*)HookResolve,    (void**)&g_origDefResolve,    "DCTX::ResolveSubresource");
            if (!g_origDefGenMips)    HookFn(at[3], (void*)HookGenMips,    (void**)&g_origDefGenMips,    "DCTX::GenerateMips");
            AliasCopyTrampolines();
        }
        // d3d11 shares ONE code address between the deferred and immediate variants of some methods
        // (DrawInstanced: both d3d11.dll+0x150BC0 — see any run's hook log). The second
        // MH_CreateHook on a shared address returns ALREADY_CREATED and leaves its orig NULL, and
        // the dispatch helpers then CALL NULL when that variant fires. That was the boot RIP=0
        // crash: NVIDIA's nvgpucomp64 started issuing one DrawInstanced per present (~7/29 driver
        // change; inst=0/0 in 7/20 logs, 0/1 now). Alias the sibling family's trampoline for any
        // orig the create call skipped — all ten methods, so a future driver shuffle can't bite.
        if (!g_origDefDrawIndexed)  g_origDefDrawIndexed  = g_origDrawIndexed;
        if (!g_origDefDraw)         g_origDefDraw         = g_origDraw;
        if (!g_origDefDrawIdxInst)  g_origDefDrawIdxInst  = g_origDrawIdxInst;
        if (!g_origDefDrawInst)     g_origDefDrawInst     = g_origDrawInst;
        if (!g_origDefDispatch)     g_origDefDispatch     = g_origDispatch;
        if (!g_origDefVSSetCB)      g_origDefVSSetCB      = g_origVSSetCB;
        if (!g_origDefPSSetCB)      g_origDefPSSetCB      = g_origPSSetCB;
        if (!g_origDefMap)          g_origDefMap          = g_origMap;
        if (!g_origDefUpdateSubres) g_origDefUpdateSubres = g_origUpdateSubres;
        if (!g_origDefOMSetRTs)     g_origDefOMSetRTs     = g_origOMSetRTs;
    } else {
        HookFn(vt[VT_CTX_DrawIndexed],  (void*)HookDrawIndexed,  (void**)&g_origDrawIndexed, "CTX::DrawIndexed");
        HookFn(vt[VT_CTX_Draw],         (void*)HookDraw,         (void**)&g_origDraw,        "CTX::Draw");
        HookFn(vt[VT_CTX_DrawIdxInst],  (void*)HookDrawIdxInst,  (void**)&g_origDrawIdxInst, "CTX::DrawIndexedInstanced");
        HookFn(vt[VT_CTX_DrawInst],     (void*)HookDrawInst,     (void**)&g_origDrawInst,    "CTX::DrawInstanced");
        HookFn(vt[VT_CTX_Dispatch],     (void*)HookDispatch,     (void**)&g_origDispatch,    "CTX::Dispatch");
        HookFn(vt[VT_CTX_VSSetCB],      (void*)HookVSSetCB,      (void**)&g_origVSSetCB,     "CTX::VSSetConstantBuffers");
        HookFn(vt[VT_CTX_PSSetCB],      (void*)HookPSSetCB,      (void**)&g_origPSSetCB,     "CTX::PSSetConstantBuffers");
        HookFn(vt[VT_CTX_Map],          (void*)HookMap,          (void**)&g_origMap,         "CTX::Map");
        HookFn(vt[15],                  (void*)HookUnmap,        (void**)&g_origUnmap,       "CTX::Unmap");
        if (!g_origUnmap && g_origDefUnmap) g_origUnmap = g_origDefUnmap;
        HookFn(vt[VT_CTX_UpdateSubres], (void*)HookUpdateSubres, (void**)&g_origUpdateSubres,"CTX::UpdateSubresource");
        HookFn(vt[VT_CTX_OMSetRTs],     (void*)HookOMSetRTs,     (void**)&g_origOMSetRTs,    "CTX::OMSetRenderTargets");
        if (!g_origExecCL) HookFn(vt[58], (void*)HookExecCL, (void**)&g_origExecCL, "CTX::ExecuteCommandList");
        HookFn(vt[39], (void*)HookDIII, (void**)&g_origDIII, "CTX::DrawIndexedInstancedIndirect");
        HookFn(vt[40], (void*)HookDII,  (void**)&g_origDII,  "CTX::DrawInstancedIndirect");
        if (!g_origDIII && g_origDefDIII) g_origDIII = g_origDefDIII;
        if (!g_origDII && g_origDefDII) g_origDII = g_origDefDII;
        if (!g_origDefDIII) g_origDefDIII = g_origDIII;
        if (!g_origDefDII) g_origDefDII = g_origDII;
        // [MIRROR] immediate-only write paths
        if (!g_origClearRTV)   HookFn(vt[50], (void*)HookClearRTV,   (void**)&g_origClearRTV,   "CTX::ClearRenderTargetView");
        if (!g_origClearUAVu)  HookFn(vt[51], (void*)HookClearUAVu,  (void**)&g_origClearUAVu,  "CTX::ClearUnorderedAccessViewUint");
        if (!g_origClearUAVf)  HookFn(vt[52], (void*)HookClearUAVf,  (void**)&g_origClearUAVf,  "CTX::ClearUnorderedAccessViewFloat");
        if (!g_origClearDSV)   HookFn(vt[53], (void*)HookClearDSV,   (void**)&g_origClearDSV,   "CTX::ClearDepthStencilView");
        if (!g_origCopyRes)    HookFn(vt[47], (void*)HookCopyRes,    (void**)&g_origCopyRes,    "CTX::CopyResource");
        if (!g_origCopySubReg) HookFn(vt[46], (void*)HookCopySubReg, (void**)&g_origCopySubReg, "CTX::CopySubresourceRegion");
        if (!g_origResolve)    HookFn(vt[57], (void*)HookResolve,    (void**)&g_origResolve,    "CTX::ResolveSubresource");
        if (!g_origGenMips)    HookFn(vt[54], (void*)HookGenMips,    (void**)&g_origGenMips,    "CTX::GenerateMips");
        // [BATCH] a code entry shared with the deferred variant: same code, same trampoline, either order
        { void* const at[4] = { vt[47], vt[46], vt[57], vt[54] }; for (int i = 0; i < 4; ++i) g_immCopyAt[i] = at[i]; AliasCopyTrampolines(); }
        if (!g_origClearRTV  && g_origDefClearRTV)  g_origClearRTV  = g_origDefClearRTV;
        if (!g_origClearDSV  && g_origDefClearDSV)  g_origClearDSV  = g_origDefClearDSV;
        if (!g_origClearUAVu && g_origDefClearUAVu) g_origClearUAVu = g_origDefClearUAVu;
        if (!g_origClearUAVf && g_origDefClearUAVf) g_origClearUAVf = g_origDefClearUAVf;
        if (!g_origClearRTV || !g_origClearDSV || !g_origClearUAVu || !g_origClearUAVf || !g_origCopyRes ||
            !g_origCopySubReg || !g_origResolve || !g_origGenMips) {
            g_mirrorOn = false; Log("[mirror] DISABLED: an immediate write-path hook did not install");
        }
        // shared-address aliasing, immediate side (see the deferred branch for the full story) —
        // whichever family hooked the shared code first owns the trampoline; the other borrows it.
        if (!g_origDrawIndexed  && g_origDefDrawIndexed)  g_origDrawIndexed  = g_origDefDrawIndexed;
        if (!g_origDraw         && g_origDefDraw)         g_origDraw         = g_origDefDraw;
        if (!g_origDrawIdxInst  && g_origDefDrawIdxInst)  g_origDrawIdxInst  = g_origDefDrawIdxInst;
        if (!g_origDrawInst     && g_origDefDrawInst)     { g_origDrawInst   = g_origDefDrawInst;
            Log("[context] CTX::DrawInstanced shares code with DCTX — aliased trampoline (null-call fix)"); }
        if (!g_origDispatch     && g_origDefDispatch)     g_origDispatch     = g_origDefDispatch;
        if (!g_origVSSetCB      && g_origDefVSSetCB)      g_origVSSetCB      = g_origDefVSSetCB;
        if (!g_origPSSetCB      && g_origDefPSSetCB)      g_origPSSetCB      = g_origDefPSSetCB;
        if (!g_origMap          && g_origDefMap)          g_origMap          = g_origDefMap;
        if (!g_origUpdateSubres && g_origDefUpdateSubres) g_origUpdateSubres = g_origDefUpdateSubres;
        if (!g_origOMSetRTs     && g_origDefOMSetRTs)     g_origOMSetRTs     = g_origDefOMSetRTs;
    }
}
static HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(ID3D11Device* dev, UINT flags, ID3D11DeviceContext** out) {
    HRESULT hr = g_origCreateDeferredContext(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (P5RMenu::internalCall || P5RXr::IsInternalCall()) {
            const UINT runtime = 1;
            (*out)->SetPrivateData(kRuntimeContextTag, sizeof(runtime), &runtime);
            return hr;
        }
        if (dev != g_dev) return hr;
        RegisterDeferredContext(*out);
        HookContextMethods(*out, true);
        Log("[deferred] captured game context #%u ctx=%p", g_deferredContextCount.load(), (void*)*out);
    }
    return hr;
}

static void HookLiveContext(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) { Log("[first] GetDevice FAILED"); return; }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    IDXGIDevice* dxdev = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC adesc{};
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)) && dxdev) {
        if (SUCCEEDED(dxdev->GetAdapter(&ad)) && ad) { ad->GetDesc(&adesc); ad->Release(); }
        dxdev->Release();
    }
    char dm[192], cm[192]; ModOf(*(void**)dev, dm, sizeof(dm)); ModOf(ctx ? *(void**)ctx : nullptr, cm, sizeof(cm));
    Log("[first] device=%p FL=0x%X vtbl(%s)  ctx=%p vtbl(%s)  adapter=%S", (void*)dev, (unsigned)fl, dm, (void*)ctx, cm, adesc.Description);
    if (ctx) {
        HookContextMethods(ctx, false);
        // keep device + immediate context alive for building the right-eye stereo resources
        dev->AddRef(); g_dev = dev;
        ctx->AddRef(); g_ctxImm = ctx;
        Log("[dup] stored device=%p ctx=%p for stereo duplication", (void*)g_dev, (void*)g_ctxImm);
        ctx->Release();
    } else Log("[first] no immediate context — CTX hooks SKIPPED");
    dev->Release();
}

// ---- no-pause-on-focus-loss: P5R stops rendering when its window loses focus (which VR triggers) ----
// Subclass the game window so it always sees itself as active/focused -> never pauses -> headset keeps
// getting frames across transitions.
static WNDPROC g_origWndProc = nullptr;
static HWND    g_gameHwnd = nullptr;
static LRESULT CALLBACK MyWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_ACTIVATE:    w = WA_ACTIVE; break;   // never WA_INACTIVE
        case WM_ACTIVATEAPP: w = TRUE;      break;   // always "app activated"
        case WM_NCACTIVATE:  w = TRUE;      break;
        case WM_KILLFOCUS:   return 0;               // swallow focus loss
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}
static void InstallNoFocusPause(HWND hwnd) {
    if (!hwnd || g_origWndProc) return;
    g_gameHwnd = hwnd;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)MyWndProc);
    Log("[focus] no-pause-on-focus-loss installed on hwnd=%p (origWndProc=%p)", (void*)hwnd, (void*)g_origWndProc);
}

// ------------------------------------------------------------------ present hooks
static bool g_keyWas[256] = {};
static bool KeyPressed(int vk) { bool d=(GetAsyncKeyState(vk)&0x8000)!=0; bool w=g_keyWas[vk&0xFF]; g_keyWas[vk&0xFF]=d; return d && !w; }
static std::atomic<bool> g_ctxHooked{false};

static void InstallGameFnHooks() {
    uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    Log("[gamehook] installing OT-flush instrumentation, P5R.exe base=%p", (void*)base);
    HookFn((void*)(base + 0x1F57E0), (void*)HookOTFlush,       (void**)&g_origOTFlush,       "GFD::OTFlush+0x1F57E0");
    HookFn((void*)(base + 0x1E13CD), (void*)HookOTFlushParent, (void**)&g_origOTFlushParent, "GFD::OTFlushParent+0x1E13CD");
    // +0x1F4B32 (wrapper) is NOT safely hookable — a generic passthrough there crashed at +0x1E13E7
    // (corrupted its call into +0x1E13CD). Dropped: the render-list executor +0x1E13CD is the target.
    (void)&HookOTFlush2; (void)g_origOTFlush2; (void)g_otFlush2Calls; (void)g_otSampleF4b;
    Log("[gamehook] installed (+0x1F57E0, +0x1E13CD). Play a few seconds; structure + per-call samples log follow.");
}

static void OnFrameBoundary(IDXGISwapChain* sc) {
    uint64_t f = g_frame.fetch_add(1) + 1;
    g_presentTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    BatchFlushEnd(g_ctxImm);   // [BATCH] the right-eye backbuffer must be complete before anything reads it
    BatchForgetAll(g_ctxImm);
    // v7: hold a ref on the presenting swapchain. The stall watchdog queries it (GetLastPresentCount)
    // possibly AFTER the game dropped it — without a ref that's a use-after-free on a dead COM object.
    if (sc != g_lastSc) {
        IDXGISwapChain* prev = g_lastSc;
        if (sc) sc->AddRef();
        g_lastSc = sc;
        if (prev) prev->Release();
        if (prev && sc) Log("[present] swapchain CHANGED %p -> %p at f=%llu", (void*)prev, (void*)sc, (unsigned long long)f);
    }
    if (sc) { DXGI_SWAP_CHAIN_DESC scd{}; if (SUCCEEDED(sc->GetDesc(&scd))) { g_bbW=scd.BufferDesc.Width; g_bbH=scd.BufferDesc.Height; g_bbFmt=scd.BufferDesc.Format; } }
    g_stereoPaused.store(g_frame.load(std::memory_order_relaxed) < g_resizeCooldownUntil.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // deferred post-resize twin drop: only HERE, on the render thread, at a frame boundary — no draw
    // in flight can be using it (the resize hook itself never releases; see HookResizeBuffers)
    if (g_twinDirty.exchange(false, std::memory_order_relaxed)) {
        MirrorFlushAll("post-resize, render thread");
        if (g_rightRTV) { g_rightRTV->Release(); g_rightRTV=nullptr; }
        if (g_rightRT)  { g_rightRT->Release();  g_rightRT=nullptr; }
        if (g_rightDSV) { g_rightDSV->Release(); g_rightDSV=nullptr; }
        if (g_rightDepth){ g_rightDepth->Release(); g_rightDepth=nullptr; }
        Log("[dup] twin dropped at frame boundary (post-resize, render thread) f=%llu", (unsigned long long)f);
    }
    g_lDrawIdx=g_cDrawIdx.exchange(0); g_lDraw=g_cDraw.exchange(0);
    g_lDrawIdxInst=g_cDrawIdxInst.exchange(0); g_lDrawInst=g_cDrawInst.exchange(0);
    g_lDispatch=g_cDispatch.exchange(0);
    g_lMapBuf=g_cMapBuf.exchange(0); g_lMapDiscard=g_cMapDiscard.exchange(0); g_lMapOther=g_cMapOther.exchange(0);
    g_lUpdBuf=g_cUpdBuf.exchange(0); g_lUpdOther=g_cUpdOther.exchange(0);
    g_lVSSet=g_cVSSet.exchange(0);   g_lPSSet=g_cPSSet.exchange(0);
    g_lRTSet=g_cRTSet.exchange(0);   g_cUpdSeq.store(0, std::memory_order_relaxed);
    g_lInject=g_cInject.exchange(0);
    g_censusFrame = false;   // perf: CB census off (mutex per bind/upload every 16th frame)

    if (f == 1 && sc) {
        Log("[diag] TITLE-FREEZE XR-ONLY PROBE=%d: XR enabled; duplication path is %s",
            (int)kTitleFreezeXrOnlyProbe, kTitleFreezeXrOnlyProbe ? "FORCED OFF" : "ENABLED");
        Log("[title-gate] ACTIVE: duplication remains OFF until the paired title resizes complete");
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d))) {
            Log("[present] FIRST FRAME: swapchain %p  %ux%u fmt=%d buffers=%u windowed=%d effect=%d hwnd=%p",
                (void*)sc, d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format,
                d.BufferCount, d.Windowed, (int)d.SwapEffect, (void*)d.OutputWindow);
            // v12: leave the game's window procedure completely untouched for this run. v11 claimed
            // to defer the focus rewrite until "post-title", but installed it as soon as the second
            // ResizeBuffers returned -- before the next Present -- so it was not a clean test. The
            // v10 stack capture parked the render thread in a WaitForSingleObjectEx inside P5R; this
            // run proves or kills the focus-message-rewrite theory without ambiguity.
            g_gameHwnd = d.OutputWindow;
            Log("[focus] no-pause rewrite stays DISABLED; menu filters input only when open");
        }
        if (!g_ctxHooked.exchange(true)) HookLiveContext(sc);
    }
    if (f==60 || f==600 || f==3600 || (g_perFrameLine && (f % 60)==0))
        Log("[f%llu] drawIdx=%u draw=%u inst=%u/%u disp=%u rt=%u | mapBuf=%u updBuf=%u | vsSet=%u psSet=%u",
            (unsigned long long)f, g_lDrawIdx, g_lDraw, g_lDrawIdxInst, g_lDrawInst, g_lDispatch,
            g_lRTSet, g_lMapBuf, g_lUpdBuf, g_lVSSet, g_lPSSet);

    // v15: the mod's own threads are killed at the title transition (heartbeat thread died at
    // stall+1s in 5/5 runs on 2026-08-31 while the game played on — his report). The game's
    // render thread is immortal, so everything that matters lives HERE now. This render-thread
    // heartbeat proves whether the Present hook itself survives the kill.
    if ((f % 120) == 0) {
        static HANDLE s_hbp = INVALID_HANDLE_VALUE;
        if (s_hbp == INVALID_HANDLE_VALUE) {
            char mod[MAX_PATH]{}; GetModuleFileNameA((HMODULE)&__ImageBase, mod, MAX_PATH);
            char* slash = strrchr(mod, '\\'); if (slash) *(slash + 1) = 0;
            char path[MAX_PATH]; wsprintfA(path, "%sp5r_hb_present.txt", mod);
            s_hbp = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (s_hbp != INVALID_HANDLE_VALUE) {
            char line[128];
            int n = wsprintfA(line, "hbp f=%u t=%ums drawIdx=%u dup=%d xrReq=%d\r\n",
                              (unsigned)f, (unsigned)GetTickCount64(), g_lDrawIdx,
                              (int)g_dupOn.load(std::memory_order_relaxed),
                              (int)g_xrStartRequested.load(std::memory_order_relaxed));
            DWORD wrote = 0; WriteFile(s_hbp, line, (DWORD)n, &wrote, nullptr);
            FlushFileBuffers(s_hbp);
        }
    }
    // v20: clear every context's pass membership at the frame boundary. "Am I in the scene/UI
    // pass" is only meaningful within a frame; a flag left set across a scene transition (movie,
    // load) lets a worker keep duplicating into targets the game is tearing down — and every
    // crash so far landed exactly at such a transition (draw count collapsing to ~33).
    for (auto& cs : g_ctxScene) {
        cs.inScene.store(false, std::memory_order_relaxed);
        cs.inUI.store(false, std::memory_order_relaxed);
    }
    // v21: rebuild the XR eye swapchains a few frames after the resize, on the render thread.
    {
        uint64_t due = g_xrRebuildAfter.load(std::memory_order_relaxed);
        if (due && f >= due) {
            g_xrRebuildAfter.store(0, std::memory_order_relaxed);
            bool ok = P5RXr::RecreateDeviceResources();
            Log("[resize] XR eye swapchains rebuilt after resize: %s (f=%llu)", ok?"ok":"FAILED", (unsigned long long)f);
        }
    }
    // v18: build the dup resources HERE, on the render thread, at a frame boundary. The hooks that
    // discover the scene pass run on worker threads, and allocating a D3D resource from inside a
    // hooked call re-enters the driver mid-allocation (the FC3 nvwgf2um crash). They only raise a
    // flag; this is the only place that creates.
    if (g_twinWanted.load(std::memory_order_relaxed) && g_dupOn.load(std::memory_order_relaxed)
        && !g_stereoPaused.load(std::memory_order_relaxed)) {
        if (!g_rightRTV || !g_rightDSV) EnsureRightRT();
        if (g_rightRTV && g_rightDSV) g_twinWanted.store(false, std::memory_order_relaxed);
    }
    if (uint32_t want = g_camCBWanted.load(std::memory_order_relaxed)) {
        if (!g_rightCamCB || g_camCBSize != want) EnsureRightCamCB(want);
        if (g_rightCamCB && g_camCBSize == want) g_camCBWanted.store(0, std::memory_order_relaxed);
    }
    // v15: INSERT is polled HERE too — the controller thread that owned it is dead by the time
    // the title gate opens (the exact v10 lesson, repeated: INSERT was pressed into a dead thread).

    // v16: open the gate on SUSTAINED BUSY FRAMES, not resize pairs. The paired-resize
    // heuristic only fired during the QUIT teardown in a real play session (proven: pair at
    // f=6003/6005 while both heartbeats show him exiting at f=6006) — so VR could never arm
    // during play and INSERT was refused ("[key] INSERT ignored") the entire session. Sixty
    // consecutive frames with a gameplay-scale draw count is the direct signal for "the scene
    // is really rendering"; the 90-frame XR/dup quiesce in HookResizeBuffers still protects
    // every later transition.
    if (!g_titleTransitionComplete.load(std::memory_order_relaxed) && f > 600) {
        static uint32_t s_busyStreak = 0;
        s_busyStreak = (g_lDrawIdx > 800) ? s_busyStreak + 1 : 0;
        if (s_busyStreak >= 60) {
            g_stereoArmAfter.store(f, std::memory_order_relaxed);
            g_titleTransitionComplete.store(true, std::memory_order_relaxed);
            Log("[gate] OPEN at f=%llu: 60 consecutive busy frames (drawIdx=%u) — VR arms in ~2s",
                (unsigned long long)f, g_lDrawIdx);
        }
    }
    // v15: and VR no longer waits for INSERT at all. First busy gameplay frame after the title
    // gate opens -> stereo + XR come up on their own. Insert opens the settings menu.
    static bool s_autoStarted = false;
    if (!s_autoStarted && g_titleTransitionComplete.load(std::memory_order_relaxed)
        && f >= g_stereoArmAfter.load(std::memory_order_relaxed) + 120 && g_lDrawIdx > 800) {
        s_autoStarted = true;
        g_dupOn.store(true, std::memory_order_relaxed);
        g_stereoPaused.store(false, std::memory_order_relaxed);
        g_xrSuspendUntil.store(0, std::memory_order_relaxed);
        g_xrStartRequested.store(true, std::memory_order_relaxed);
        Log("[auto] VR AUTO-START at f=%llu (busy frame drawIdx=%u) — no INSERT needed", (unsigned long long)f, g_lDrawIdx);
    }

    // capture state machine (BF lineage): dump completed capture; arm requested one
    if (g_capturing.load(std::memory_order_relaxed)) {
        g_capturing.store(false);
        size_t n = DumpForensicCapture();
        if (g_autoPending) {
            g_autoPending = false;
            if (n >= 800) { g_autoCaptureDone = true; Log("[cap] auto capture GOOD (%zu events) — latching off", n); }
            else            Log("[cap] auto capture weak (%zu events) — will retry on next busy frame", n);
        }
    } else if (g_captureRequest) {
        g_captureRequest = false;
        { std::lock_guard<std::mutex> lk(g_capMx); g_capture.clear(); g_capture.reserve(kCaptureMax); g_camDumps.clear(); }
        { std::lock_guard<std::mutex> lk(g_cbcMx); g_cbCache.clear(); g_mainBound.clear(); g_mainBoundSeen.clear(); }
        g_inMainPass.store(false); g_mainPassW.store(0); g_mainPassH.store(0); g_mainSnapDone.store(false);
        g_capturing.store(true); Log("[cap] forensic ARMED for next frame (f=%llu)", (unsigned long long)f);
    }
    // self-healing auto-capture: first genuinely busy gameplay frame
    if (false && !g_autoCaptureDone && !g_autoPending && !g_capturing.load(std::memory_order_relaxed) && !g_captureRequest &&   // perf: auto forensic capture off
        f > 600 && g_lDrawIdx > 800) {
        g_autoPending = true; g_captureRequest = true;
        Log("[cap] AUTO-ARM: busy gameplay frame (f=%llu drawIdx=%u)", (unsigned long long)f, g_lDrawIdx);
    }

    // --- stereo: auto-enable dup in gameplay, bring up the headset, submit both eyes ---
    // Never issue a duplicated draw before the title transition's paired resizes. The control run
    // proved XR-only survives that transition; any pre-title duplicate history wedges NVIDIA Present.
    bool titleReady = g_titleTransitionComplete.load(std::memory_order_relaxed)
                      && f >= g_stereoArmAfter.load(std::memory_order_relaxed);
    CameraControls::Init((HMODULE)&__ImageBase);
    auto menuActions = P5RMenu::Begin(g_dev, g_ctxImm, g_gameHwnd, g_bbW, g_bbH, g_dupOn.load(), titleReady);
    g_hmdFill.store(CameraControls::Get().fillHeadset);
    g_headPosOn.store(CameraControls::Get().headPosition);
    g_swapEyes.store(CameraControls::Get().swapEyes);
    if (menuActions.toggleStereo) SetStereoEnabled(!g_dupOn.load());
    if (menuActions.recenter) P5RXr::Recenter();
    // --- [HEADVR] presence controls + GFD source camera ---
    if (KeyPressed(VK_F11) && !P5RMenu::IsOpen())  { bool v=!g_headOn.load();    g_headOn.store(v);    Log("[headvr] head ROTATION %s (F11)", v?"ON":"OFF"); }
    if (KeyPressed(VK_F12) && !P5RMenu::IsOpen())  { auto settings=CameraControls::Get();bool v=!settings.headPosition;settings.headPosition=v;CameraControls::Set(settings);g_headPosOn.store(v); Log("[headvr] head POSITION %s (F12)", v?"ON":"OFF"); }
    if (KeyPressed(VK_HOME) && !P5RMenu::IsOpen()) { P5RXr::Recenter(); Log("[headvr] RECENTER (Home)"); }
    // [HUDDUMP] Numpad 1 = menu 3D models to the panel on/off; Numpad 2 = dump panel + eye images
    if (KeyPressed(VK_NUMPAD1)) { int v = (g_menuOnlyMode.load() + 1) % 3; g_menuOnlyMode.store(v); g_menuModelsOn.store(v == 1);
        Log("[huddump] Numpad 1: menu-only draws %s", v == 0 ? "REMOVED from the world" : v == 1 ? "moved to the panel" : "LEFT in the world"); }
    {   // [FRAMING] run 1 at 3840x3840 looked "squashed vertically": dump both full eye images once,
        // automatically, after ~25 s of live stereo (no key), so the framing can be inspected.
        static uint32_t s_liveStereo = 0; bool autoDump = false;
#ifdef P5R_DIAGNOSTICS
        if (g_mirrorActive && !g_worldFrozen.load()) { ++s_liveStereo; autoDump = (s_liveStereo == 3000); }
#else
        (void)s_liveStereo;
#endif
        g_dumpLabel[0] = 0;
        char label[64];
        if (GfdCam::TakeUiDump(f, label, sizeof(label))) { snprintf(g_dumpLabel, sizeof(g_dumpLabel), "%s", label); autoDump = true; Log("[uidump] f%llu %s: dumping HUD panel + eyes", (unsigned long long)f, g_dumpLabel); }
        g_hudDumpReq.store(KeyPressed(VK_NUMPAD2) || autoDump, std::memory_order_relaxed);
        if (autoDump) Log("[framing] f%llu auto-dumping both eye images (p5r_eyeL.bmp / p5r_eyeR.bmp)", (unsigned long long)f);
    }
    // [PERF] live isolation toggles
    {
        bool wide = CameraControls::Get().wideVisibility, stereo = g_dupOn.load();
        const bool wide0 = wide, stereo0 = stereo;
        PerfSequence(KeyPressed(VK_NUMPAD9), wide, stereo);
        if (wide != wide0) { auto v = CameraControls::Get(); v.wideVisibility = wide; CameraControls::Set(v); }
        if (stereo != stereo0) SetStereoEnabled(stereo);
    }
    if (KeyPressed(VK_NUMPAD0)) { bool v = !P5RCulling::precise.load(); P5RCulling::precise.store(v); Log("[perf] Numpad 0: field culling %s", v ? "PRECISE (rendered eye frusta)" : "OFF (everything drawn, old)"); }
    if (KeyPressed(VK_NUMPAD8)) { bool v = !CameraControls::exactFill.load(); CameraControls::exactFill.store(v); Log("[perf] Numpad 8: headset fill %s", v ? "EXACT per-axis (no horizontal over-render)" : "UNIFORM 16:9 (old)"); }
    if (KeyPressed(VK_SUBTRACT) && !P5RMenu::IsOpen()) { bool v = !g_freezeTest.load(); g_freezeTest.store(v); Log("[freezetest] Numpad -: %s", v ? "HOLDING the current stereo frame (compositor reprojects one still image)" : "live frames again"); }
    if (KeyPressed(VK_DIVIDE)) { bool v = !g_paceOn.load(); g_paceOn.store(v); Log("[pace] Numpad /: frame pacing %s (k=%d)", v ? "ON (steady ~60, whole display periods)" : "OFF (free-running)", g_paceK); }
    if (KeyPressed(VK_MULTIPLY)) { bool v = !g_tagRealPos.load(); g_tagRealPos.store(v); Log("[tagpos] Numpad *: submitted head position %s", v ? "REAL (located)" : "ZEROED (old)"); }
    if (KeyPressed(VK_ADD)) { bool v = !g_mirBatch.load(); g_mirBatch.store(v); Log("[batch] Numpad +: right-eye replay %s", v ? "BATCHED per pass (one target switch)" : "PER-DRAW (old)"); }
    if ((f % 120) == 0) BatchLogStats(f);
    if (KeyPressed(VK_NUMPAD4)) { bool v = !g_smartMirror.load(); g_smartMirror.store(v); Log("[perf] Numpad 4: smart mirror %s (eye-independent passes %s)", v ? "ON" : "OFF", v ? "drawn once" : "duplicated"); }
    if (KeyPressed(VK_NUMPAD5)) { SetStereoEnabled(!g_dupOn.load()); Log("[perf] Numpad 5: stereo %s", g_dupOn.load() ? "ON" : "OFF"); }
    if (KeyPressed(VK_NUMPAD6)) { auto v = CameraControls::Get(); v.wideVisibility = !v.wideVisibility; CameraControls::Set(v); Log("[perf] Numpad 6: wide visibility %s", v.wideVisibility ? "ON" : "OFF"); }
    if (KeyPressed(VK_NUMPAD7)) { bool v = !g_hudLayerOn.load(); g_hudLayerOn.store(v); Log("[perf] Numpad 7: HUD layer %s", v ? "ON" : "OFF"); }
    if (KeyPressed(VK_NUMPAD3)) { bool v = !g_freezeOn.load(); g_freezeOn.store(v); Log("[freeze] Numpad 3: live-stereo world behind menus %s", v ? "ON" : "OFF"); }
    if (KeyPressed(VK_END) && !P5RMenu::IsOpen())  { auto settings=CameraControls::Get(); settings.fillHeadset=!settings.fillHeadset; CameraControls::Set(settings); bool v=settings.fillHeadset; g_hmdFill.store(v); Log("[headvr] headset-FOV fill %s (End; source FOV write stays OFF)", v?"ON":"OFF"); }
    if (f == 600) GfdCam::StartScan(Log);
    if ((f % 600) == 0) { uint32_t rf = 0, om = 0; GfdCam::RoomCamStats(rf, om);
        if (rf || om) Log("[roomcam] f%llu field updates as follow camera (room mode 4) in 600f=%u; other unlocked camera mode seen=%d", (unsigned long long)f, rf, om ? (int)om - 1 : -1); }
    { static uint32_t s_pt=0; const uint32_t pt=P5RInput::fpToggles.load(); if (pt!=s_pt) { s_pt=pt; static const char* k[3]={"field","cutscene","conversation"}; Log("[fptoggle] f%llu R3 hold/F5: %s first person %s (event=%d confidant=%d)", (unsigned long long)f, k[P5RInput::fpToggleKind.load()], P5RInput::fpToggleOn.load()?"ON":"OFF", (int)GfdCam::EventRunning(), (int)GfdCam::ConfidantRunning()); } }
    if ((f % 600) == 0 && (g_fpAnchorUses.load() || g_fpManualUses.load()))
        Log("[fpanchor] f%llu camera uploads in 600f: at Joker's eye=%u, old manual offset=%u", (unsigned long long)f, g_fpAnchorUses.exchange(0), g_fpManualUses.exchange(0));
    if ((f % 600) == 0 && g_evFpUses.load()) Log("[eventfp] f%llu camera uploads from Joker's eyes in events in 600f=%u", (unsigned long long)f, g_evFpUses.exchange(0));
    {
        float hv = g_hmdVFov.load(std::memory_order_relaxed);
        bool en = g_srcFovOn.load(std::memory_order_relaxed) && g_hmdFill.load(std::memory_order_relaxed)
                  && g_dupOn.load(std::memory_order_relaxed) && (g_xrWasRunning || kFakeHmd) && hv > 0.3f
                  && g_lDrawIdx > 800;   // v2: gameplay frames only — never during loads/menus
        GfdCam::visibilityRequested.store(g_headOn.load() && g_dupOn.load() && g_xrWasRunning);
        GfdCam::OnFrame(f, g_cbVFov.load(std::memory_order_relaxed), hv, en);
        // [MENUVR] game menus stay in VR (docked panel) unless the player chose the flat screen; the
        // title screen, pre-rendered movies and title Load Game are always the flat screen.
        const char* menuTask = GfdCam::ScreenTask();
        // 10-08: a "loading screen" while the game draws the live world is over (field already up)
        const bool loadingDone = menuTask && !strcmp(menuTask, "fld_scn_change") && g_worldLiveRaw.load(std::memory_order_relaxed);
        const bool gameMenu = GfdCam::ScreenMode() && !loadingDone;
        const bool flatOnly = GfdCam::FlatTask(menuTask)    // [FLATSCREEN] title, movies, Load Game, rail map
            || (GfdCam::SolidTask(menuTask) && HudSettings::Get().systemFlat);   // save/loading/network: Insert option
        const bool menuVr = gameMenu && !flatOnly && HudSettings::Get().menusInVr && g_xrWasRunning;
        g_menuInVr.store(menuVr, std::memory_order_relaxed);
        g_blackWorld.store(menuVr && GfdCam::SolidTask(menuTask), std::memory_order_relaxed);   // [BLACKWORLD] save / loading / network page
        g_screenView.store(gameMenu && !menuVr, std::memory_order_relaxed);
    }
    uint32_t headThisFrame = g_headApplied.exchange(0, std::memory_order_relaxed);
    HeadTagFrame(f, headThisFrame != 0);
    HeadGpuBoundary(f);   // [HEADTAG] tag = pose of the executed camera list (mono/menu frames: plain located pose)
    if ((f % 300) == 0 && g_xrWasRunning)
        Log("[headvr] f%llu headApplied=%u rot=%d pos=%d fill=%d hmdVFov=%.1f gameVFov=%.1f dolly=%d src=%d",
            (unsigned long long)f, headThisFrame, (int)g_headOn.load(), (int)g_headPosOn.load(), (int)g_hmdFill.load(),
            g_hmdVFov.load()*57.2958f, g_cbVFov.load()*57.2958f, g_dolly.load(), (int)GfdCam::Active());
    if ((f % 300) == 0 && g_xrWasRunning)
        Log("[headvr] dup filter: skipped %u non-main-camera scene draws in 300f", g_dupSkipNotCam.exchange(0));
    PerfGpuFrameBoundary();
    GpuProfPresent(f, g_mirrorActive && !g_worldFrozen.load(), g_lDrawIdx, KeyPressed(VK_DECIMAL));   // [GPUPROF] auto per-pass capture
    if ((f % 120) == 0) Log("[perf] f%llu smart mirror=%d eye-independent draws not duplicated in 120f=%u | fill=%s lens h=%.1f v=%.1f deg",
        (unsigned long long)f, (int)g_smartMirror.load(), g_mirSkipIndep.exchange(0), CameraControls::exactFill.load() ? "exact" : "uniform",
        g_capM00.load() > 0.01f ? 2.f * atanf(1.f / g_capM00.load()) * 57.2958f : 0.f, g_capM11.load() > 0.01f ? 2.f * atanf(1.f / g_capM11.load()) * 57.2958f : 0.f);
    PerfReport(f, g_dupOn.load(), CameraControls::Get().wideVisibility, g_hudLayerOn.load());
    if (f == 300 || (f % 3000) == 0)
        Log("[resspoof] f%llu armed=%d request %dx%d | GetMonitorInfoW calls %u (spoofed %u) | window size limit raised %u | backbuffer %ux%u | [aspect] SetResolution calls %u, 3D aspect %.4f",
            (unsigned long long)f, (int)ResSpoof::armed, ResSpoof::reqW, ResSpoof::reqH, ResSpoof::monCalls.load(), ResSpoof::monSpoofed.load(), ResSpoof::minmaxRaised.load(), g_bbW, g_bbH,
            ResSpoof::setResCalls.load(), ResSpoof::aspectPair ? ResSpoof::aspectPair[1] : 0.f);
    HudOnPresent(f, sc);   // [HUD] scan summary + bmp of the previous frame, before submit
    HeadPace(f, g_mirrorActive && !g_worldFrozen.load());   // [PACE] every stereo frame = the same whole number of headset refreshes (ME1 default)
    { const auto camera=CameraControls::Get();
#ifdef P5R_DIAGNOSTICS   // developer builds only: records frame thumbnails to p5r_flash_*/ in the game folder
      P5RXr::SetFlashRecording(GfdCam::EventRunning() && (GfdCam::ConfidantRunning()?camera.eventFirstPerson:camera.cutsceneFirstPerson),f);
#else
      (void)camera; P5RXr::SetFlashRecording(false,f);
#endif
    }
    { static uint32_t s_haze=0; static unsigned s_hd=0; const uint32_t hz=P5RXr::LastHazeFrame();   // [HAZE] camera + buffer changes at a fog switch
      if (hz && hz!=s_haze && f >= (uint64_t)hz + 3) { s_haze=hz; if (s_hd++ < 30) { Log("[haze] fog switch at f%u: camera uploads and constant-buffer changes follow", hz); CamTraceDump(hz); CbChangeDump(hz); } } }
    { static uint32_t s_spike=0; static unsigned s_dumps=0; const uint32_t sp=P5RXr::LastEyeSpikeFrame();   // [CAMTRACE]
      if (sp && sp!=s_spike && f >= (uint64_t)sp + 3) { s_spike=sp; if (s_dumps++ < 40) CamTraceDump(sp); } }
    SubmitToHeadset();   // XR init early, drive session, submit left+right each frame
    g_hudXrLive.store(g_xrWasRunning && P5RXr::IsRunning(), std::memory_order_relaxed);   // [HUDLAYER]
    HudLayerAfterSubmit();
    if (P5RMenu::IsOpen() && !P5RMenu::WasDrawn() && f >= g_resizeCooldownUntil.load()) {
        ID3D11Texture2D* menuBackbuffer = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&menuBackbuffer))) {
            P5RMenu::Draw(menuBackbuffer, nullptr); menuBackbuffer->Release();
        }
    }
    Ecl2BuildEyes(nullptr, false);   // [ECL2] drain list refs on frames the headset path skipped (no-op if consumed)
    if (kD3DDebugLayer) { if (!g_infoQ && g_dev) DebugLayerAttach(g_dev); DebugLayerDrain("f"); }
    // [MIRROR] next frame: remember the backbuffer (raw pointer only), arm/disarm, housekeeping, stats
    {
        g_curBB = nullptr;
        if (sc) { ID3D11Texture2D* b = nullptr; if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b)) && b) { g_curBB = b; b->Release(); } }
        if (g_mirrorActive && g_hudDumpReq.load(std::memory_order_relaxed) && sc) {   // perf: no periodic 4K readback
            ID3D11Texture2D* bbT = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bbT)) && bbT) {
                AcquireSRWLockShared(&g_twLock);
                auto it = g_twRes.find((ID3D11Resource*)bbT);
                ID3D11Texture2D* tw = (it != g_twRes.end() && it->second.twin && !it->second.bad && it->second.written == g_mirFrame) ? (ID3D11Texture2D*)it->second.twin : nullptr;
                ReleaseSRWLockShared(&g_twLock);
                if (tw) StereoSelfCheck(bbT, tw, f, true);
                else Log("[stereo-check] f%llu no right-eye backbuffer twin this frame", (unsigned long long)f);
                bbT->Release();
            }
        }
        UpdateMirrorActivation(f);
        static bool s_flagsLogged = false;
        if (!s_flagsLogged && g_dev) { s_flagsLogged = true; UINT cf = g_dev->GetCreationFlags();
            Log("[mirror] game device creation flags=0x%X%s", cf, (cf & D3D11_CREATE_DEVICE_SINGLETHREADED) ? " SINGLETHREADED (no concurrent resource creation allowed!)" : ""); }
        if ((f % 120) == 0 && (g_mirrorActive || !g_twRes.empty())) {
            Log("[mirror] f%llu 120f: draws=%u camSwaps=%u disp=%u ops=%u syncs=%u skips=%u newTwins=%u otherTid=%u | rightEye twin=%u FALLBACK=%u | drawTid=%lu presentTid=%lu | twins res=%zu views=%zu | mainCamBuf=%p rightCamCB=%p sep=%d",
                (unsigned long long)f, g_mirDraws, g_mirCamSwaps, g_mirDisp, g_mirOps, g_mirSyncs, g_mirSkips, g_mirNew, g_mirOtherTid,
                g_mirRightTwin, g_mirRightFallback, (unsigned long)g_mirDrawTid, (unsigned long)g_presentTid.load(),
                g_twRes.size(), g_twView.size(), g_mainCamBuf.load(), (void*)g_rightCamCB, (int)CameraControls::HalfEye(CameraControls::Get()));
            g_mirDraws = g_mirDisp = g_mirOps = g_mirSyncs = g_mirSkips = g_mirNew = 0;
            g_mirCamSwaps = g_mirRightTwin = g_mirRightFallback = g_mirOtherTid = 0;
            { static LARGE_INTEGER s_t{}; LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
              double fps = s_t.QuadPart ? 120.0 * fq.QuadPart / double(now.QuadPart - s_t.QuadPart) : 0.0; s_t = now;
              size_t graves; AcquireSRWLockShared(&g_graveLock); graves = g_grave.size(); ReleaseSRWLockShared(&g_graveLock);
              Log("[mirror] deferred-context mirrored draws=%u | FPS=%.1f | stale bb views replaced=%u | indirect draws=%u | graveyard=%zu bbViews=%zu | queued=%u built(render thread)=%u",
                  g_mirDefDraws.exchange(0), fps, g_mirStaleViews.exchange(0), g_cIndirect.exchange(0), graves, g_bbViews.size(), g_mirQueued.exchange(0), g_mirBuilt.exchange(0));
              Log("[mirror] deferred clears mirrored=%u | deferred sync-copies recorded=%u", g_mirDefClears.exchange(0), g_mirSyncDef.exchange(0));
              Log("[isolation] runtimeBypasses=%llu foreignBypasses=%llu", (unsigned long long)g_runtimeBypasses.load(), (unsigned long long)g_foreignBypasses.load());
              Log("[mirror] descriptor view reuses=%u", g_mirViewReuses.exchange(0)); }
            Log("[camcb] 120f: camera found in CB uploads: update=%u map=%u blocks=%u (transposed %u) | non-camera uploads mirrored=%u gpu copies mirrored=%u | right-twin binds=%u | camera-bearing buffers=%zu",
                g_cbHitUpd.exchange(0), g_cbHitMap.exchange(0), g_cbBlocks.exchange(0), g_cbBlocksT.exchange(0), g_cbMirrorUpd.exchange(0), g_cbCopyMir.exchange(0), g_cbCamSwaps.exchange(0), g_camBufs.size());
        }
        CamBuildQueued();
        MirBuildQueued();
        BatchPoolBuild();   // [BATCH] snapshot buffers the hooks asked for
        if ((f % 60) == 0) MirrorViewPurge();
        ++g_mirFrame;
        GraveSweep(f, false);
    }
    if (g_dupOn.load(std::memory_order_relaxed) && !g_xrWasRunning) {
        g_viewRight.store(((f/120)&1)!=0, std::memory_order_relaxed);   // no headset yet -> monitor debug: alternate view
        PresentDebugBlitRight();
    } else g_viewRight.store(false, std::memory_order_relaxed);
    if (g_dupOn.load(std::memory_order_relaxed) && (f % 120)==0) {
        Log("[dup] f%llu twin=%ux%u xrLive=%d paused=%d dupDraws/frame~=%u", (unsigned long long)f, g_twW, g_twH,
            (int)g_xrWasRunning, (int)g_stereoPaused.load(std::memory_order_relaxed), g_lDrawIdx);
        // v18 [ALIGN]: the numbers that decide whether the eyes can line up at all — scene pass vs
        // twin size (a mismatch is what cropped the right eye), how many contexts think they are in
        // the scene pass (>1 means the per-context split is not holding), and the eye order in use.
        int inScene = 0; for (auto& s : g_ctxScene) if (s.inScene.load(std::memory_order_relaxed)) ++inScene;
        // v22 [RTCENSUS]: every render target that took draws this window, in bind order. The UI's
        // own target is the one that takes a burst of small draws AFTER the scene target — read it
        // from a frame with the HUD or a menu on screen.
        {
            struct Row { uint32_t seq, w, h, fmt, draws; };
            Row rows[48]; int nr = 0;
            for (int i = 0; i < 48; ++i) {
                uint32_t d = g_rtStat[i].draws.exchange(0, std::memory_order_relaxed);
                if (!d) continue;
                rows[nr++] = { g_rtStat[i].seq.load(std::memory_order_relaxed), g_rtStat[i].w.load(std::memory_order_relaxed),
                               g_rtStat[i].h.load(std::memory_order_relaxed), g_rtStat[i].fmt.load(std::memory_order_relaxed), d };
            }
            for (int i = 1; i < nr; ++i) { Row k = rows[i]; int j = i - 1;
                while (j >= 0 && rows[j].seq > k.seq) { rows[j+1] = rows[j]; --j; } rows[j+1] = k; }
            Log("[rtcensus] f%llu — %d targets took draws over the last 120 frames (bind order):", (unsigned long long)f, nr);
            for (int i = 0; i < nr; ++i)
                Log("[rtcensus]   #%02u %ux%u fmt=%u draws=%u%s", rows[i].seq, rows[i].w, rows[i].h, rows[i].fmt, rows[i].draws,
                    (rows[i].w==g_bbW && rows[i].h==g_bbH) ? "  <- backbuffer-sized" : (rows[i].w>=2000 ? "  <- scene-sized" : ""));
        }
        Log("[uipass] f%llu drawsIntoBackbuffer=%u idxCount min=%u max=%u (1/frame = the fullscreen composite, many = real UI)",
            (unsigned long long)f, g_uiPassDraws.exchange(0, std::memory_order_relaxed),
            g_uiIdxMin.exchange(0xFFFFFFFF, std::memory_order_relaxed), g_uiIdxMax.exchange(0, std::memory_order_relaxed));
        Log("[align] f%llu scenePass=%ux%u twin=%ux%u sceneCtxs=%d eyeOrder=%s sep=%d fov=%.2f dupScene=%u uiDup=%u",
            (unsigned long long)f, g_scenePassW.load(), g_scenePassH.load(), g_twW, g_twH, inScene,
            g_swapEyes.load(std::memory_order_relaxed) ? "twin=LEFT" : "twin=RIGHT",
            (int)CameraControls::HalfEye(CameraControls::Get()), g_fovScale.load(std::memory_order_relaxed),
            g_cDupScene.exchange(0, std::memory_order_relaxed), g_cDupUI.exchange(0, std::memory_order_relaxed));
    }
}
static thread_local DWORD g_presentFaultCode = 0;
static thread_local void* g_presentFaultAddr = nullptr;
static thread_local char g_presentFaultMod[192] = {};
static LONG PresentRecoveryFilter(EXCEPTION_POINTERS* xp) {
    if (!xp || !xp->ExceptionRecord || xp->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    void* xaddr = xp->ExceptionRecord->ExceptionAddress;
    // v9: a CALL/JMP into the null page inside Present (the boot-time RIP=0 crash) is always
    // recoverable — the VEH has already logged the caller by the time this filter runs. Returning
    // S_OK keeps the game alive; if the bad callee heals (e.g. an overlay finishing its own
    // hook install), later presents just work.
    if ((uintptr_t)xaddr < 0x1000) {
        g_presentFaultCode = xp->ExceptionRecord->ExceptionCode;
        g_presentFaultAddr = xaddr;
        strncpy_s(g_presentFaultMod, "NULL-CALL", _TRUNCATE);
        return EXCEPTION_EXECUTE_HANDLER;
    }
    if (g_frame.load(std::memory_order_relaxed) >= g_presentRecoverUntil.load(std::memory_order_relaxed))
        return EXCEPTION_CONTINUE_SEARCH;
    char mod[192]{};
    ModOf(xaddr, mod, sizeof(mod));
    if (!strstr(mod, "nvwgf2umx.dll")) return EXCEPTION_CONTINUE_SEARCH;
    g_presentFaultCode = xp->ExceptionRecord->ExceptionCode;
    g_presentFaultAddr = xaddr;
    strncpy_s(g_presentFaultMod, mod, _TRUNCATE);
    return EXCEPTION_EXECUTE_HANDLER;
}
static HRESULT RecoverPresentFault(const char* path) {
    Log("[present] RECOVERED %s NVIDIA fault code=0x%08X at %p (%s) frame=%llu until=%llu; returning S_OK to keep XR loop alive",
        path, (unsigned)g_presentFaultCode, g_presentFaultAddr, g_presentFaultMod,
        (unsigned long long)g_frame.load(), (unsigned long long)g_presentRecoverUntil.load());
    return S_OK;
}
static void PerfAdd(PerfCounter& c, const LARGE_INTEGER& t0) { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); c.ticks.fetch_add((uint64_t)(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed); c.calls.fetch_add(1, std::memory_order_relaxed); }
static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT s, UINT fl) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0); OnFrameBoundary(sc); PerfAdd(g_perfOurPresent, t0);
    __try { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); HRESULT hr = g_origPresent(sc,s,fl); PerfAdd(g_perfPresentCall, t1); return hr; }
    __except (PresentRecoveryFilter(GetExceptionInformation())) { return RecoverPresentFault("Present"); }
}
static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT s, UINT fl, const DXGI_PRESENT_PARAMETERS* pp) {
    LARGE_INTEGER t0; QueryPerformanceCounter(&t0); OnFrameBoundary(sc); PerfAdd(g_perfOurPresent, t0);
    __try { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); HRESULT hr = g_origPresent1(sc,s,fl,pp); PerfAdd(g_perfPresentCall, t1); return hr; }
    __except (PresentRecoveryFilter(GetExceptionInformation())) { return RecoverPresentFault("Present1"); }
}
static std::atomic<bool> g_lateContextChecked{false};
static HRESULT STDMETHODCALLTYPE HookPresentLate(IDXGISwapChain* sc, UINT s, UINT fl) {
    if (!g_lateContextChecked.exchange(true, std::memory_order_relaxed)) {
        Log("[rebind] post-transition Present is live; refreshing device/context hooks");
        HookLiveContext(sc);
    }
    { LARGE_INTEGER t0; QueryPerformanceCounter(&t0); OnFrameBoundary(sc); PerfAdd(g_perfOurPresent, t0); }
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1); HRESULT hr = g_origPresentLate(sc, s, fl); PerfAdd(g_perfPresentCall, t1); return hr;
}
static HRESULT STDMETHODCALLTYPE HookPresent1Late(IDXGISwapChain1* sc, UINT s, UINT fl, const DXGI_PRESENT_PARAMETERS* pp) {
    if (!g_lateContextChecked.exchange(true, std::memory_order_relaxed)) {
        Log("[rebind] post-transition Present1 is live; refreshing device/context hooks");
        HookLiveContext(sc);
    }
    { LARGE_INTEGER t0; QueryPerformanceCounter(&t0); OnFrameBoundary(sc); PerfAdd(g_perfOurPresent, t0); }
    LARGE_INTEGER t1; QueryPerformanceCounter(&t1); HRESULT hr = g_origPresent1Late(sc, s, fl, pp); PerfAdd(g_perfPresentCall, t1); return hr;
}
static void ReleaseMirrorForResize() {
    HeadGpuReset();
    // The mirror and retained command lists also own views/resources. Waiting ten Present
    // calls to release these is too late for ResizeBuffers, especially during paired resizes.
    g_mirrorActive = false;
    MirrorFlushAll("before ResizeBuffers");
    {
        std::lock_guard<std::mutex> lk(g_eclMx);
        for (auto& item : g_frameCLs) if (item.cl) item.cl->Release();
        g_frameCLs.clear(); g_clCam.clear(); g_ctxCam.clear();
    }
    CamLockX();
    for (const auto& item : g_camBufs) { Bury(item.second.right); Bury(item.first); }
    g_camBufs.clear();
    ReleaseSRWLockExclusive(&g_camBufLock);
    g_mainCamBuf.store(nullptr, std::memory_order_relaxed);
    g_curBB = nullptr;
}
static HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    uint32_t rtid = GetCurrentThreadId();
    g_resizeTid.store(rtid, std::memory_order_relaxed);
    uint64_t nowF = g_frame.load(std::memory_order_relaxed);
    bool hasDupState = g_dupOn.load(std::memory_order_relaxed)
                       || g_rightRTV || g_rightRT || g_rightDSV || g_rightDepth || g_rightCamCB;
    Log("[resize] buffers=%u %ux%u fmt=%d flags=0x%X (frame=%llu tid=%lu presentTid=%lu) — full XR suspend 90f + pause 90f",
        n, w, h, (int)fmt, flags, (unsigned long long)nowF, rtid, (unsigned long)g_presentTid.load());
    // Pause stereo/XR work, then synchronously unbind, GPU-drain, and release every dup-owned
    // resource before ResizeBuffers. The diagnostics proved this is the same render thread.
    // FULL XR SUSPEND: 0-layer frames still invoke the runtime's GPU path and did NOT stop the crash, so
    // make ZERO OpenXR calls for the window — VDXR must be completely idle while the game resizes its
    // swapchain on the shared device.
    g_resizeCooldownUntil.store(nowF + 90, std::memory_order_relaxed);
    g_xrSuspendUntil.store(nowF + 90, std::memory_order_relaxed);
    g_presentRecoverUntil.store(nowF + 120, std::memory_order_relaxed);
    g_stereoPaused.store(true, std::memory_order_relaxed);
    ReleaseMirrorForResize();
    if (hasDupState) TeardownDupForResize(nowF);
    else Log("[dup] PRE-RESIZE teardown skipped: duplication has never been active");
    // D3D command ownership keeps submitted resources alive; release all our delayed refs now.
    GraveSweep(nowF, true);
    // Bracket the driver call so we know if the AV is INSIDE ResizeBuffers vs after it returns, and read
    // the device-removed reason on the way out (device loss = why the headset dies but the game recovers).
    // v21: hand the game's device back COMPLETELY for the resize. The dup teardown above already
    // released our twin and drained the GPU, and XR calls are suspended — yet the run of
    // 2026-09-01 still took an AV inside nvwgf2umx one frame after a paired resize, with
    // inDup=0 and dupRes=0. The only mod-owned D3D11 resources still on the game's device at
    // that moment are the OpenXR runtime's two eye swapchain textures. This is July's leading
    // hypothesis (never tested until now): drop them across the resize, rebuild after. The
    // session, spaces and frame loop stay alive, so the headset holds its last frame for a
    // moment instead of losing the session.
    if (g_xrStartRequested.load(std::memory_order_relaxed)) {
        Log("[resize] releasing XR eye swapchains off the game device before the resize");
        P5RXr::ReleaseDeviceResources();
        g_xrRebuildAfter.store(nowF + 8, std::memory_order_relaxed);   // rebuild once the driver settles
    }
    Log("[resize] -> calling origResizeBuffers ...");
    HRESULT hr;
    __try { hr = g_origResizeBuffers(sc, n, w, h, fmt, flags); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[resize] !! origResizeBuffers FAULTED (SEH-caught in resize hook, code=0x%08X)", (unsigned)GetExceptionCode());
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    HRESULT drr = g_dev ? g_dev->GetDeviceRemovedReason() : 0;
    Log("[resize] <- origResizeBuffers returned hr=0x%08X deviceRemovedReason=0x%08X", (unsigned)hr, (unsigned)drr);
    if (SUCCEEDED(hr) && !g_titleTransitionComplete.load(std::memory_order_relaxed)
        && nowF > 1000) {
        uint64_t first = g_titleResizeFirstFrame.load(std::memory_order_relaxed);
        if (!first || nowF - first > 10) {
            g_titleResizeFirstFrame.store(nowF, std::memory_order_relaxed);
            Log("[title-gate] first gameplay-transition resize f=%llu; XR and duplication have never started",
                (unsigned long long)nowF);
        } else {
            uint64_t arm = nowF;
            g_stereoArmAfter.store(arm, std::memory_order_relaxed);
            g_titleTransitionComplete.store(true, std::memory_order_relaxed);
            Log("[title-gate] paired gameplay-transition resizes complete f=%llu; menu can enable stereo at f=%llu",
                (unsigned long long)nowF, (unsigned long long)arm);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookPresentInstance(IDXGISwapChain* sc, UINT s, UINT fl) {
    OnFrameBoundary(sc);
    return g_origPresent(sc, s, fl);
}
static HRESULT STDMETHODCALLTYPE HookPresent1Instance(IDXGISwapChain1* sc, UINT s, UINT fl,
                                                       const DXGI_PRESENT_PARAMETERS* pp) {
    OnFrameBoundary(sc);
    return g_origPresent1(sc, s, fl, pp);
}
static bool InstallGameplayObjectHooks(IDXGISwapChain* sc) {
    if (!sc || !g_ctxImm || !g_origPresent || !g_origDrawIndexed) return false;

    // ReShade's DXGI wrapper can rewrite the code-entry detours during the title transition
    // while leaving the COM objects and their entrypoint addresses unchanged. Shadow the actual
    // gameplay objects' vtables instead, then call MinHook's already-captured trampolines.
    constexpr SIZE_T kSwapEntries = 32;
    void** oldScVt = *(void***)sc;
    void** newScVt = (void**)HeapAlloc(GetProcessHeap(), 0, kSwapEntries * sizeof(void*));
    if (!newScVt) return false;
    memcpy(newScVt, oldScVt, kSwapEntries * sizeof(void*));
    newScVt[VT_SC_Present] = (void*)HookPresentInstance;
    newScVt[VT_SC_Present1] = (void*)HookPresent1Instance;
    InterlockedExchangePointer((PVOID volatile*)sc, newScVt);

    constexpr SIZE_T kContextEntries = 64;
    void** oldCtxVt = *(void***)g_ctxImm;
    void** newCtxVt = (void**)HeapAlloc(GetProcessHeap(), 0, kContextEntries * sizeof(void*));
    if (!newCtxVt) {
        Log("[rebind] gameplay swapchain vtable installed, context shadow allocation failed");
        return true;
    }
    memcpy(newCtxVt, oldCtxVt, kContextEntries * sizeof(void*));
    newCtxVt[VT_CTX_DrawIndexed] = (void*)HookDrawIndexed;
    newCtxVt[VT_CTX_Draw] = (void*)HookDraw;
    newCtxVt[VT_CTX_DrawIdxInst] = (void*)HookDrawIdxInst;
    newCtxVt[VT_CTX_DrawInst] = (void*)HookDrawInst;
    newCtxVt[VT_CTX_Dispatch] = (void*)HookDispatch;
    newCtxVt[VT_CTX_VSSetCB] = (void*)HookVSSetCB;
    newCtxVt[VT_CTX_PSSetCB] = (void*)HookPSSetCB;
    newCtxVt[VT_CTX_Map] = (void*)HookMap;
    newCtxVt[VT_CTX_UpdateSubres] = (void*)HookUpdateSubres;
    newCtxVt[VT_CTX_OMSetRTs] = (void*)HookOMSetRTs;
    InterlockedExchangePointer((PVOID volatile*)g_ctxImm, newCtxVt);

    Log("[rebind] GAMEPLAY OBJECT VTABLES INSTALLED sc=%p old=%p new=%p ctx=%p old=%p new=%p",
        (void*)sc, (void*)oldScVt, (void*)newScVt,
        (void*)g_ctxImm, (void*)oldCtxVt, (void*)newCtxVt);
    return true;
}

// ================= v7: title-stall forensics =================
// Last run's evidence (7/20): title's paired resizes return clean, NO crash, yet our Present hook
// stops being called one frame later — and the per-object shadow vtables installed by the rebind
// never fire either. The game keeps rendering (monitor live). So presents are flowing through a
// path we cannot see. These diagnostics answer, in ONE run, the three-way question:
//   (a) detour unpatched?  -> boot-vs-now byte compare of the hooked code entries (+ auto re-arm)
//   (b) new swapchain?     -> factory CreateSwapChain/ForHwnd hooks (+ auto-adopt into our pipeline)
//   (c) old chain still presenting? -> GetLastPresentCount polling on the stalled swapchain
static bool SafeRead16(const void* p, uint8_t* out) {
    __try { memcpy(out, p, 16); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeScQueries(IDXGISwapChain* sc, HRESULT* hrP, UINT* lastPresent,
                          HRESULT* hrS, DXGI_FRAME_STATISTICS* fs) {
    __try {
        *hrP = sc->GetLastPresentCount(lastPresent);
        *hrS = sc->GetFrameStatistics(fs);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void Hex16(const uint8_t* p, char* out /* >= 49 */) {
    for (int i = 0; i < 16; ++i) snprintf(out + i * 3, 4, "%02X ", p[i]);
    out[47] = 0;
}
static bool ContainsI(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* h = hay; *h; ++h) {
        size_t i = 0;
        while (i < nl && h[i] && (char)tolower((unsigned char)h[i]) == (char)tolower((unsigned char)needle[i])) ++i;
        if (i == nl) return true;
    }
    return false;
}
static void LogLoadedGfxModules() {
    HMODULE mods[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        Log("[stall] EnumProcessModules FAILED gle=%lu", GetLastError());
        return;
    }
    int n = (int)(needed / sizeof(HMODULE)); if (n > 1024) n = 1024;
    for (int i = 0; i < n; ++i) {
        char path[MAX_PATH]{}; GetModuleFileNameA(mods[i], path, MAX_PATH);
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        if (ContainsI(base, "dxgi") || ContainsI(base, "d3d11") || ContainsI(base, "nvwgf")
            || ContainsI(base, "reshade") || ContainsI(base, "overlay") || ContainsI(base, "rtss")
            || ContainsI(base, "openxr") || ContainsI(base, "virtualdesktop") || ContainsI(base, "vrclient")) {
            MODULEINFO mi{}; K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
            Log("[stall] module %s base=%p size=0x%X", path, (void*)mods[i], (unsigned)mi.SizeOfImage);
        }
    }
}
// (a) detour re-arm: only called when the byte compare PROVES the entry was rewritten. Remove +
// re-create gives us a fresh trampoline over whatever code sits there now; whoever re-patched us
// gets clobbered in turn — acceptable for a diagnosis run, and loudly logged.
static void ReArmHook(void* target, void* detour, void** orig, const char* name) {
    std::lock_guard<std::mutex> mhLk(g_mhMx);   // all MH_* calls hold this — MinHook is not thread-safe
    MH_STATUS r = MH_RemoveHook(target);
    MH_STATUS c = MH_CreateHook(target, detour, orig);
    MH_STATUS e = MH_EnableHook(target);
    Log("[stall] RE-ARM %s @ %p: remove=%d create=%d enable=%d", name, target, (int)r, (int)c, (int)e);
}
static void CheckDetourBytes(bool reArm) {
    if (!g_bootBytesValid) { Log("[stall] no boot byte snapshots — skipping detour check"); return; }
    struct { void* target; uint8_t* boot; void* detour; void** orig; const char* name; } sites[] = {
        { g_initialPresentTarget,  g_presentBytesBoot,  (void*)HookPresent,       (void**)&g_origPresent,       "SC::Present"       },
        { g_initialPresent1Target, g_present1BytesBoot, (void*)HookPresent1,      (void**)&g_origPresent1,      "SC::Present1"      },
        { g_resizeCodeTarget,      g_resizeBytesBoot,   (void*)HookResizeBuffers, (void**)&g_origResizeBuffers, "SC::ResizeBuffers" },
    };
    for (auto& s : sites) {
        if (!s.target) continue;
        uint8_t now[16];
        if (!SafeRead16(s.target, now)) { Log("[stall] %s @ %p: byte read FAULTED", s.name, s.target); continue; }
        char pm[320]; ModPathOf(s.target, pm, sizeof(pm));
        if (memcmp(now, s.boot, 16) == 0) {
            Log("[stall] %s @ %p (%s): detour bytes INTACT", s.name, s.target, pm);
        } else {
            char hb[49], hn[49]; Hex16(s.boot, hb); Hex16(now, hn);
            Log("[stall] %s @ %p (%s): detour bytes REWRITTEN", s.name, s.target, pm);
            Log("[stall]    boot: %s", hb);
            Log("[stall]    now : %s", hn);
            if (reArm) {
                ReArmHook(s.target, s.detour, s.orig, s.name);
                memcpy(s.boot, s.target, 16);   // fresh snapshot so the next round reports on the re-armed state
            }
        }
    }
}
static bool SafeReadU64(const void* p, uint64_t* out) {
    __try { *out = *(const uint64_t*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// v8: what is the (stalled) present/render thread actually DOING? Suspend it briefly, read RIP,
// then conservatively scan its stack for return addresses into loaded modules. A thread parked in
// a wait shows NtWaitFor* at RIP with the wait's caller chain on the stack — that names the
// mechanism (focus poll, fence wait, loader lock, ...) without a debugger attached.
static void LogPresentThreadStack(const char* tag) {
    DWORD tid = g_presentTid.load(std::memory_order_relaxed);
    if (!tid || tid == GetCurrentThreadId()) return;
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { Log("[stall] OpenThread(%lu) FAILED gle=%lu", tid, GetLastError()); return; }
    if (SuspendThread(th) == (DWORD)-1) { Log("[stall] SuspendThread(%lu) FAILED gle=%lu", tid, GetLastError()); CloseHandle(th); return; }
    CONTEXT cx{}; cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(th, &cx)) {
        char rm[320]; ModPathOf((void*)cx.Rip, rm, sizeof(rm));
        Log("[stall] %s tid=%lu RIP=%p (%s) RSP=%p", tag, tid, (void*)cx.Rip, rm, (void*)cx.Rsp);
        const uint64_t* sp = (const uint64_t*)cx.Rsp;
        int printed = 0;
        for (int i = 0; i < 3072 && printed < 28; ++i) {
            uint64_t v = 0;
            if (!SafeReadU64(sp + i, &v)) break;
            if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull) continue;
            HMODULE mod = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCSTR)v, &mod) || !mod) continue;
            // plausible return address: the bytes just before it should end a CALL encoding
            uint64_t preQ = 0;
            if (!SafeReadU64((const uint8_t*)v - 8, &preQ)) continue;
            const uint8_t* pb = (const uint8_t*)&preQ;
            bool callish = (pb[3] == 0xE8) || (pb[2] == 0xFF) || (pb[1] == 0xFF) || (pb[5] == 0xFF) || (pb[0] == 0xFF);
            char mm[192]; ModOf((void*)v, mm, sizeof(mm));
            Log("[stall]   sp+0x%04X: %s%s", i * 8, mm, callish ? "" : "  (weak)");
            ++printed;
        }
    } else Log("[stall] GetThreadContext(%lu) FAILED gle=%lu", tid, GetLastError());
    ResumeThread(th);
    CloseHandle(th);
}
// Capture first, resume, then resolve modules/log: never log while the render thread is suspended.
static void SnapshotRenderStall(){
 DWORD tid=g_presentTid.load();if(!tid||tid==GetCurrentThreadId())return;
 HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,tid);if(!thread)return;
 if(SuspendThread(thread)==DWORD(-1)){CloseHandle(thread);return;}
 CONTEXT context{};context.ContextFlags=CONTEXT_CONTROL;
 uint64_t stack[256]={};SIZE_T bytes=0;BOOL ok=GetThreadContext(thread,&context);
 if(ok)ReadProcessMemory(GetCurrentProcess(),(void*)context.Rsp,stack,sizeof(stack),&bytes);
 ResumeThread(thread);CloseHandle(thread);
 if(!ok){Log("[stall-snapshot] no context tid=%lu",tid);return;}
 char module[192];ModOf((void*)context.Rip,module,sizeof(module));
 Log("[stall-snapshot] tid=%lu RIP=%p %s stackBytes=%llu",tid,(void*)context.Rip,module,(unsigned long long)bytes);
 int printed=0;for(size_t i=0;i<bytes/8&&printed<24;++i){
  MEMORY_BASIC_INFORMATION info{};if(stack[i]<0x10000||!VirtualQuery((void*)stack[i],&info,sizeof(info)))continue;
  if(info.State!=MEM_COMMIT||!(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE)))continue;
  ModOf((void*)stack[i],module,sizeof(module));Log("[stall-snapshot] stack+0x%llx %s (candidate return address)",(unsigned long long)i*8,module);++printed;
 }
}
static void LogFocusState(int round) {
    HWND fg = GetForegroundWindow();
    DWORD fgPid = 0, fgTid = fg ? GetWindowThreadProcessId(fg, &fgPid) : 0;
    char cls[64] = "?";
    if (fg) GetClassNameA(fg, cls, sizeof(cls));   // no cross-process message pump risk (unlike GetWindowText)
    HWND game = g_gameHwnd;
    Log("[stall] r%d focus: fg=%p cls=%s pid=%lu%s | game hwnd=%p visible=%d iconic=%d",
        round, (void*)fg, cls, fgPid, fgPid == GetCurrentProcessId() ? " (US)" : "",
        (void*)game, game ? (int)IsWindowVisible(game) : -1, game ? (int)IsIconic(game) : -1);
}
static void LogStallDiagnostics(int round) {
    uint64_t f = g_frame.load(std::memory_order_relaxed);
    uint32_t draws = g_cDrawIdx.load(std::memory_order_relaxed);   // accumulates while no frame boundary runs
    uint32_t rts   = g_cRTSet.load(std::memory_order_relaxed);
    IDXGISwapChain* sc = g_lastSc;
    // The DXGI queries below can block for the whole stall (v10 r1, v12 r1: SafeScQueries never
    // returned while the render thread was parked). Announce the round FIRST so a hung query
    // mutes only its own data, not the fact that the watchdog is alive.
    Log("[stall] r%d begin f=%llu drawsSinceBoundary=%u rtSetsSinceBoundary=%u", round,
        (unsigned long long)f, draws, rts);
    HRESULT hrP = E_POINTER, hrS = E_POINTER; UINT lastPresent = 0; DXGI_FRAME_STATISTICS fs{};
    bool ok = sc ? SafeScQueries(sc, &hrP, &lastPresent, &hrS, &fs) : false;
    if (sc && !ok) { Log("[stall] r%d sc=%p query FAULTED (object dead?)", round, (void*)sc); }
    Log("[stall] r%d f=%llu drawsSinceBoundary=%u rtSetsSinceBoundary=%u | sc=%p GetLastPresentCount hr=0x%08X n=%u | FrameStats hr=0x%08X presents=%u",
        round, (unsigned long long)f, draws, rts, (void*)sc,
        (unsigned)hrP, lastPresent, (unsigned)hrS, (unsigned)fs.PresentCount);
    LogFocusState(round);
    if (round == 0) {
        CheckDetourBytes(true);
        LogLoadedGfxModules();
    } else if ((round % 5) == 0) {
        CheckDetourBytes(false);   // periodic re-check without re-arming twice
    }
    if (round <= 2 || (round % 5) == 0) LogPresentThreadStack("present-thread");
}
// (b) swapchain-creation visibility. The title resize uses ALLOW_MODE_SWITCH (0x802) — a re-created
// swapchain (or device) at the transition would explain every datum: our per-object state goes
// stale, the boot code hooks never see the new object's path, presents "vanish". These hooks are on
// the REAL factory implementation (obtained from our dummy chain pre-ReShade-era, now native), so
// any proxy layer above still funnels into them.
typedef HRESULT(STDMETHODCALLTYPE* PFN_FacCreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_FacCreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetFullscreenState)(IDXGISwapChain*, BOOL, IDXGIOutput*);
static PFN_FacCreateSwapChain        g_origFacCreateSwapChain = nullptr;
static PFN_FacCreateSwapChainForHwnd g_origFacCreateSwapChainForHwnd = nullptr;
static PFN_SetFullscreenState        g_origSetFullscreenState = nullptr;
enum : int { VT_FAC_CreateSwapChain = 10, VT_FAC2_CreateSwapChainForHwnd = 15, VT_SC_SetFullscreenState = 10 };

static void AdoptNewSwapChain(IDXGISwapChain* sc, HWND hwnd, UINT w, UINT h, const char* how) {
    void* presentTarget = (*(void***)sc)[VT_SC_Present];
    char pm[320]; ModPathOf(presentTarget, pm, sizeof(pm));
    Log("[screate] new swapchain via %s sc=%p hwnd=%p %ux%u Present=%p (%s) f=%llu",
        how, (void*)sc, (void*)hwnd, w, h, presentTarget, pm,
        (unsigned long long)g_frame.load(std::memory_order_relaxed));
    // Adopt only game-sized chains (runtime/overlay helper chains are small); note-but-skip others.
    bool gameWindow = g_gameHwnd && hwnd == g_gameHwnd;
    if (!gameWindow && w < 1000) { Log("[screate] helper-sized chain — logged, not adopted"); return; }
    ID3D11Device* dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
        if (g_dev && dev != g_dev) {
            Log("[screate] !! DEVICE CHANGED old=%p new=%p — refreshing device/context for dup+XR", (void*)g_dev, (void*)dev);
            ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
            if (ctx) {
                HookContextMethods(ctx, false);   // same d3d11 code addresses -> MinHook tolerates re-create
                ID3D11Device* oldDev = g_dev; ID3D11DeviceContext* oldCtx = g_ctxImm;
                dev->AddRef(); g_dev = dev;
                g_ctxImm = ctx;                   // GetImmediateContext already AddRef'd
                if (oldCtx) oldCtx->Release();
                if (oldDev) oldDev->Release();
            }
        }
        dev->Release();
    }
    // If the new chain's Present/Present1 route through code we haven't hooked, hook it now — this
    // is the auto-fix for the "presents vanished" stall if the cause is a re-created swapchain.
    if (presentTarget != g_initialPresentTarget && !g_latePresentHooked.load(std::memory_order_relaxed)) {
        bool ok = HookFn(presentTarget, (void*)HookPresentLate, (void**)&g_origPresentLate, "SC::Present(new-swapchain)");
        g_latePresentHooked.store(ok, std::memory_order_relaxed);
    }
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
        void* p1 = (*(void***)sc1)[VT_SC_Present1];
        if (p1 != g_initialPresent1Target && !g_latePresent1Hooked.load(std::memory_order_relaxed)) {
            bool ok = HookFn(p1, (void*)HookPresent1Late, (void**)&g_origPresent1Late, "SC::Present1(new-swapchain)");
            g_latePresent1Hooked.store(ok, std::memory_order_relaxed);
        }
        sc1->Release();
    }
    void* rb = (*(void***)sc)[VT_SC_ResizeBuffers];
    if (rb != g_resizeCodeTarget) {
        char rm[320]; ModPathOf(rb, rm, sizeof(rm));
        Log("[screate] note: new chain's ResizeBuffers=%p (%s) differs from boot target — title-gate would miss it", rb, rm);
    }
}
static HRESULT STDMETHODCALLTYPE HookFacCreateSwapChain(IDXGIFactory* fac, IUnknown* dev,
                                                        DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = g_origFacCreateSwapChain(fac, dev, d, out);
    UINT w = d ? d->BufferDesc.Width : 0, h = d ? d->BufferDesc.Height : 0;
    Log("[screate] CreateSwapChain hr=0x%08X %ux%u fmt=%d buffers=%u windowed=%d effect=%d flags=0x%X hwnd=%p f=%llu",
        (unsigned)hr, w, h, d ? (int)d->BufferDesc.Format : -1, d ? d->BufferCount : 0,
        d ? (int)d->Windowed : -1, d ? (int)d->SwapEffect : -1, d ? d->Flags : 0,
        d ? (void*)d->OutputWindow : nullptr,
        (unsigned long long)g_frame.load(std::memory_order_relaxed));
    if (SUCCEEDED(hr) && out && *out) AdoptNewSwapChain(*out, d ? d->OutputWindow : nullptr, w, h, "CreateSwapChain");
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookFacCreateSwapChainForHwnd(IDXGIFactory2* fac, IUnknown* dev, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsd, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = g_origFacCreateSwapChainForHwnd(fac, dev, hwnd, d, fsd, o, out);
    UINT w = d ? d->Width : 0, h = d ? d->Height : 0;
    Log("[screate] CreateSwapChainForHwnd hr=0x%08X %ux%u fmt=%d buffers=%u effect=%d flags=0x%X hwnd=%p f=%llu",
        (unsigned)hr, w, h, d ? (int)d->Format : -1, d ? d->BufferCount : 0,
        d ? (int)d->SwapEffect : -1, d ? d->Flags : 0, (void*)hwnd,
        (unsigned long long)g_frame.load(std::memory_order_relaxed));
    if (SUCCEEDED(hr) && out && *out) AdoptNewSwapChain(*out, hwnd, w, h, "CreateSwapChainForHwnd");
    return hr;
}
static HRESULT STDMETHODCALLTYPE HookSetFullscreenState(IDXGISwapChain* sc, BOOL fs, IDXGIOutput* o) {
    Log("[fullscreen] SetFullscreenState(%d) sc=%p f=%llu", (int)fs, (void*)sc,
        (unsigned long long)g_frame.load(std::memory_order_relaxed));
    return g_origSetFullscreenState(sc, fs, o);
}

// v11/v12: diagnostics live on their OWN thread. In the v10 run a stall-round DXGI query hung the
// controller thread, which also owned the INSERT poll and the fallback arming — the user pressed
// INSERT into a dead thread and "nothing happened". The controller now does only cheap,
// unblockable work; if a diagnostic call hangs, only diagnostics stop.
static std::atomic<bool> g_stallActive{false};
// v14 heartbeat: every silent ending so far is ambiguous between (a) process death, (b) the main
// log wedging on g_logMx, (c) the user quitting early. This thread shares NOTHING with the main
// logger: its own file, raw WriteFile, no CRT stdio, no locks. Whatever kills the [init] log's
// voice, the heartbeat keeps ticking until the PROCESS itself is gone — so the last heartbeat
// line timestamps the actual death, and its counters say whether frames/draws were still moving.
static DWORD WINAPI HeartbeatThread(LPVOID) {
    char mod[MAX_PATH]{}; GetModuleFileNameA((HMODULE)&__ImageBase, mod, MAX_PATH);
    char* slash = strrchr(mod, '\\'); if (slash) *(slash + 1) = 0;
    char path[MAX_PATH]; wsprintfA(path, "%sp5r_heartbeat.txt", mod);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        ULONGLONG t = GetTickCount64() - t0;
        DWORD ptid = g_presentTid.load(std::memory_order_relaxed);
        int alive = -1;                       // -1 = present thread not seen yet
        if (ptid) {
            alive = 0;
            if (HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ptid)) {
                DWORD ec = 0;
                if (GetExitCodeThread(th, &ec) && ec == STILL_ACTIVE) alive = 1;
                CloseHandle(th);
            }
        }
        char line[256];
        int n = wsprintfA(line, "hb t=%u.%03us f=%u drawsSinceBoundary=%u stall=%d presentTid=%u alive=%d\r\n",
                          (unsigned)(t / 1000), (unsigned)(t % 1000),
                          (unsigned)g_frame.load(std::memory_order_relaxed),
                          (unsigned)g_cDrawIdx.load(std::memory_order_relaxed),
                          (int)g_stallActive.load(std::memory_order_relaxed),
                          (unsigned)ptid, alive);
        DWORD wrote = 0;
        WriteFile(h, line, (DWORD)n, &wrote, nullptr);
        FlushFileBuffers(h);
        Sleep(1000);
    }
}
static DWORD WINAPI StallDiagThread(LPVOID) {
    int round = 0;
    for (;;) {
        if (g_stallActive.load(std::memory_order_relaxed)) {
            if (round < 90) LogStallDiagnostics(round++);   // v13: cover a full network-timeout arc
        } else if (round) {
            round = 0;
        }
        Sleep(1000);
    }
}
static DWORD WINAPI RenderPathControllerThread(LPVOID) {
    uint64_t lastFrame = g_frame.load(std::memory_order_relaxed);
    ULONGLONG lastProgress = GetTickCount64();
    bool attempted = false;uint64_t snapshotFrame=~uint64_t(0);
    bool focusControlLogged = false;
    bool stalledNow = false;
    for (;;) {

        uint64_t frame = g_frame.load(std::memory_order_relaxed);
        if (frame != lastFrame) {
            lastFrame = frame;
            lastProgress = GetTickCount64();
            if (stalledNow) {
                stalledNow = false;
                g_stallActive.store(false, std::memory_order_relaxed);
                Log("[stall] FRAMES RESUMED at f=%llu", (unsigned long long)frame);
                attempted = false;
                // v13: a resumed present path is authoritative — un-latch the fallback so the
                // final-draw submission can never fight the live Present-driven XR submit.
                if (g_presentPathDead.exchange(false, std::memory_order_relaxed))
                    Log("[fallback] present path recovered; final-draw submission DISARMED");
            }
        }
        if(frame>600 && GetTickCount64()-lastProgress>3000 && snapshotFrame!=frame){snapshotFrame=frame;SnapshotRenderStall();}
        // v12: do not call InstallNoFocusPause at all. v11 called it immediately after the paired
        // resize, which was still early enough to poison the very first post-transition Present.
        // Keeping it disabled is safe for this test: P5R can remain the foreground window while
        // VirtualDesktopXR receives frames, and it gives us a real untouched-message control.
        if (!focusControlLogged && g_titleTransitionComplete.load(std::memory_order_relaxed)) {
            focusControlLogged = true;
            Log("[focus] v13 title transition complete with original WndProc still installed");
        }
        if (!attempted
            && g_titleTransitionComplete.load(std::memory_order_relaxed)
            && GetTickCount64() - lastProgress >= 500) {
            attempted = true;
            IDXGISwapChain* sc = g_lastSc;
            if (!sc) {
                Log("[rebind] frame path stalled after title but no swapchain is available");
            } else {
                void** vt = *(void***)sc;
                void* presentTarget = vt[VT_SC_Present];
                char pm[192]{}; ModOf(presentTarget, pm, sizeof(pm));
                Log("[rebind] frame path stalled at f=%llu; current Present=%p (%s), boot Present=%p",
                    (unsigned long long)frame, presentTarget, pm, g_initialPresentTarget);
                if (presentTarget != g_initialPresentTarget) {
                    bool ok = HookFn(presentTarget, (void*)HookPresentLate,
                                     (void**)&g_origPresentLate, "SC::Present(post-title)");
                    g_latePresentHooked.store(ok, std::memory_order_relaxed);
                }

                IDXGISwapChain1* sc1 = nullptr;
                if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
                    void* present1Target = (*(void***)sc1)[VT_SC_Present1];
                    char p1m[192]{}; ModOf(present1Target, p1m, sizeof(p1m));
                    Log("[rebind] current Present1=%p (%s), boot Present1=%p",
                        present1Target, p1m, g_initialPresent1Target);
                    if (present1Target != g_initialPresent1Target) {
                        bool ok = HookFn(present1Target, (void*)HookPresent1Late,
                                         (void**)&g_origPresent1Late, "SC::Present1(post-title)");
                        g_latePresent1Hooked.store(ok, std::memory_order_relaxed);
                    }
                    sc1->Release();
                }
                if (!g_latePresentHooked.load(std::memory_order_relaxed)
                    && !g_latePresent1Hooked.load(std::memory_order_relaxed)) {
                    // v13: DO NOT install per-object shadow vtables. Every run that installed them
                    // (v7 x2, v12 x2) went silent within ~1s: the shadow copies were 32/64 entries
                    // against real vtables of ~41 (IDXGISwapChain4) and 115+ (ID3D11DeviceContext),
                    // so the game's next call through an uncopied slot (ExecuteCommandList is ~114
                    // and P5R uses it every frame) jumped into heap garbage. They also defended
                    // against ReShade detour rewrites, and ReShade is no longer in this setup.
                    // The stall itself is the game's own title-screen network check-in blocking the
                    // render thread (winsock+SspiCli stack, v12 r0) — the correct response is to
                    // WAIT for it, not to rewire live objects.
                    Log("[rebind] entrypoints unchanged; waiting out the stall passively (v13: no vtable surgery)");
                }
            }
        }
        // v7/v11: while the frame path is stalled post-title, the diag THREAD runs the forensics
        // (byte-compare + module map on its round 0, present-count/focus/stack polling each round).
        if (g_titleTransitionComplete.load(std::memory_order_relaxed)
            && GetTickCount64() - lastProgress >= 500) {
            stalledNow = true;
            g_stallActive.store(true, std::memory_order_relaxed);
            // v8: the fallback (final-draw XR submission) previously armed ONLY on the NVIDIA-AV
            // path via the VEH — the silent stall never armed it. v13: arm at 30s, not 3s. The
            // title stall is the game's own blocking network check-in on the render thread; a
            // TLS connect/timeout can legitimately take 20s+, and arming the fallback during a
            // pause that then resumes leaves presentPathDead wrongly latched.
            if (GetTickCount64() - lastProgress >= 30000
                && !g_presentPathDead.load(std::memory_order_relaxed)) {
                g_presentPathDead.store(true, std::memory_order_relaxed);
                g_stereoPaused.store(false, std::memory_order_relaxed);
                g_resizeCooldownUntil.store(0, std::memory_order_relaxed);
                g_xrSuspendUntil.store(0, std::memory_order_relaxed);
                Log("[fallback] SILENT title stall (30s, no AV) — final-draw XR submission armed; menu needs a live Present path");
            }
        }
        // v8: with the Present path dead, SubmitToHeadset never runs, so nothing would ever call
        // P5RXr::Init — the fallback's BeginFrame would fail forever. Do the (async) init here once
        // the user requests stereo.
        if (g_presentPathDead.load(std::memory_order_relaxed)
            && g_xrStartRequested.load(std::memory_order_relaxed)
            && !g_xrTried && g_dev && g_bbW && g_bbH) {
            g_xrTried = true;
            P5RXr::SetEyeResolution(g_bbW, g_bbH);
            P5RXr::SetGameCopyFn((void*)g_origCopyRes);   // hand-off copy bypasses the mirror hook
            bool started = P5RXr::Init(g_dev);
            Log("[fallback] XR Init from controller thread started=%d (async, eye res %ux%u)", (int)started, g_bbW, g_bbH);
        }
        Sleep(16);
    }
}

// ------------------------------------------------------------------ VEH crash fingerprinter
// [ALLTHREADS] at a driver-thread AV: freeze every other thread and log where it is — the thread that
// fed the driver the bad state is the one busy in d3d11/nvwgf2umx/our module at that moment.
static void LogModAddr(const char* tag, int idx, uint64_t v) {
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &hm) || !hm) return;
    char mp[MAX_PATH] = ""; GetModuleFileNameA(hm, mp, MAX_PATH);
    const char* bn = strrchr(mp, 92); bn = bn ? bn + 1 : mp;
    Log("[ALLTHREADS]   %s #%d %s+0x%llX", tag, idx, bn, (unsigned long long)(v - (uint64_t)hm));
}
static bool Interesting(uint64_t v) {
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &hm) || !hm) return false;
    char mp[MAX_PATH] = ""; GetModuleFileNameA(hm, mp, MAX_PATH);
    const char* bn = strrchr(mp, 92); bn = bn ? bn + 1 : mp;
    return strstr(bn, "d3d11") || strstr(bn, "nvwgf2") || strstr(bn, "MEVR_P5R") || strstr(bn, "P5R.exe") || strstr(bn, "dxgi")
        || strstr(bn, "VirtualDesktop") || strstr(bn, "LibOVR") || strstr(bn, "openxr");
}
static void DumpAllThreads() {
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int dumped = 0;
    for (BOOL ok = Thread32First(snap, &te); ok && dumped < 40; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
        CONTEXT cx{}; cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(th, &cx)) {
            // only threads with something graphics-related on them
            bool hit = Interesting(cx.Rip); int hits = 0;
            const uint64_t* sp = (const uint64_t*)cx.Rsp;
            uint64_t found[16]; int nf = 0;
            for (int i = 0; i < 2048 && nf < 16; ++i) { uint64_t v = 0; if (!SafeReadU64(sp + i, &v)) break; if (v > 0x10000 && Interesting(v)) { found[nf++] = v; ++hits; } }
            if (hit || hits) {
                wchar_t* desc = nullptr; char dn[64] = "";
                if (SUCCEEDED(GetThreadDescription(th, &desc)) && desc) { WideCharToMultiByte(CP_UTF8, 0, desc, -1, dn, sizeof(dn), nullptr, nullptr); LocalFree(desc); }
                Log("[ALLTHREADS] tid=%lu desc='%s'%s%s", te.th32ThreadID, dn,
                    te.th32ThreadID == g_presentTid.load() ? " <RENDER THREAD>" : "", "");
                LogModAddr("rip", 0, cx.Rip);
                for (int k = 0; k < nf; ++k) LogModAddr("stk", k, found[k]);
                ++dumped;
            }
        }
        ResumeThread(th); CloseHandle(th);
    }
    CloseHandle(snap);
    Log("[ALLTHREADS] done (%d graphics-related threads)", dumped);
}
static LONG CALLBACK VehLogger(EXCEPTION_POINTERS* xp) {
    DWORD code = xp->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && kD3DDebugLayer) { static int nb = 0; if (nb++ < 4) { Log("[VEH] BREAKPOINT (debug layer?) — draining messages"); DebugLayerDrain("AT-BREAK"); } }
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW) return EXCEPTION_CONTINUE_SEARCH;
    // v11: our SafeRead16/SafeReadU64 probes fault BY DESIGN on unreadable pages (their SEH guard
    // eats it). Don't log those or let them consume the 8-report budget.
    {
        uintptr_t a = (uintptr_t)xp->ExceptionRecord->ExceptionAddress;
        uintptr_t r16 = (uintptr_t)&SafeRead16, r64 = (uintptr_t)&SafeReadU64;
        if ((a >= r16 && a < r16 + 0x40) || (a >= r64 && a < r64 + 0x40)) return EXCEPTION_CONTINUE_SEARCH;
    }
    static std::atomic<int> n{0};
    if (n.fetch_add(1) < 8) {
        void* addr = xp->ExceptionRecord->ExceptionAddress;
        char m[192]; ModOf(addr, m, sizeof(m));
        uint32_t tid = GetCurrentThreadId();
        if (code == EXCEPTION_ACCESS_VIOLATION && strstr(m, "nvwgf2umx.dll")
            && g_titleTransitionComplete.load(std::memory_order_relaxed)) {
            bool firstDead = !g_presentPathDead.exchange(true, std::memory_order_relaxed);
            g_stereoPaused.store(false, std::memory_order_relaxed);
            g_resizeCooldownUntil.store(0, std::memory_order_relaxed);
            g_xrSuspendUntil.store(0, std::memory_order_relaxed);
            if (firstDead) Log("[fallback] title NVIDIA AV killed Present path; final-draw XR submission armed, menu needs a live Present path");
        }
        Log("[VEH] XR activeCall=%s runtimeBypasses=%llu foreignBypasses=%llu",
            P5RXr::ActiveCall(), (unsigned long long)g_runtimeBypasses.load(), (unsigned long long)g_foreignBypasses.load());
        bool dupRes = g_rightRTV || g_rightRT || g_rightDSV || g_rightDepth || g_rightCamCB;
        Log("[VEH] inDup=%d (0=no, 1=scene dup, 2=UI dup) — is OUR duplication on this thread's stack?", g_tlsInDup);
        if (kD3DDebugLayer) DebugLayerDrain("AT-CRASH");
        if (code == EXCEPTION_ACCESS_VIOLATION && strstr(m, "nvwgf2")) { static bool once = false; if (!once) { once = true; DumpAllThreads(); } }
        {
            wchar_t* desc = nullptr; char dn[96] = "";
            if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc) { WideCharToMultiByte(CP_UTF8, 0, desc, -1, dn, sizeof(dn), nullptr, nullptr); LocalFree(desc); }
            Log("[VEH] crash thread: inMirrorOp=%d stage='%s' mirroredOnDeferred=%d mirrorActive=%d inflight=%d flushing=%d desc='%s'",
                t_mirIn, t_mirStage, t_mirDefUser, (int)g_mirrorActive, g_mirInflight.load(), (int)g_mirFlushing.load(), dn);
            if (xp->ContextRecord) {
                const uint64_t* sp = (const uint64_t*)xp->ContextRecord->Rsp;
                int printed = 0;
                for (int i = 0; i < 1024 && printed < 24; ++i) {
                    uint64_t v = 0;
                    if (!SafeReadU64(sp + i, &v)) break;
                    if (v < 0x10000) continue;
                    HMODULE hm = nullptr;
                    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &hm) || !hm) continue;
                    char mp[MAX_PATH] = ""; GetModuleFileNameA(hm, mp, MAX_PATH);
                    const char* bn = strrchr(mp, '\\'); bn = bn ? bn + 1 : mp;
                    if (!strstr(bn, "d3d11") && !strstr(bn, "nvwgf2") && !strstr(bn, "MEVR_P5R") && !strstr(bn, "P5R.exe") && !strstr(bn, "dxgi")) continue;
                    Log("[VEH]   stk sp+0x%04X: %s+0x%llX", i * 8, bn, (unsigned long long)(v - (uint64_t)hm));
                    ++printed;
                }
            }
        }
        Log("[VEH] code=0x%08X at %p (%s) tid=%lu | presentTid=%lu resizeTid=%lu frame=%llu xrSuspend=%d dupOn=%d dupRes=%d",
            code, addr, m, tid, (unsigned long)g_presentTid.load(), (unsigned long)g_resizeTid.load(),
            (unsigned long long)g_frame.load(),
            (int)(g_frame.load() < g_xrSuspendUntil.load()), (int)g_dupOn.load(), (int)dupRes);
        // v17: for AVs, say WHAT the faulting instruction was doing — read vs write vs execute and
        // the target address. A wild-pointer read at a recognizable address (a freed twin, our CB,
        // a tiny offset = null+field) narrows the suspect list without a debugger.
        if (code == EXCEPTION_ACCESS_VIOLATION && xp->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR kind = xp->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR target = xp->ExceptionRecord->ExceptionInformation[1];
            Log("[VEH] AV detail: %s of %p",
                kind == 0 ? "READ" : kind == 1 ? "WRITE" : kind == 8 ? "EXEC(DEP)" : "op?", (void*)target);
        }
        // v17: for faults inside P5R.exe, scan the stack for game-module return addresses. This is
        // an RSP SCAN, not an unwind — entries can be stale, treat as leads only — but on a packed
        // exe it is the only caller evidence we get.
        if (strstr(m, "P5R.exe") && xp->ContextRecord) {
            const uint64_t* sp = (const uint64_t*)xp->ContextRecord->Rsp;
            uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
            int printed = 0;
            for (int i = 0; i < 768 && printed < 10; ++i) {
                uint64_t v = 0;
                if (!SafeReadU64(sp + i, &v)) break;
                if (v > base && v < base + 0x8000000ull) {
                    Log("[VEH]   sp+0x%04X: P5R.exe+0x%llX (scan, unverified)", i * 8,
                        (unsigned long long)(v - base));
                    ++printed;
                }
            }
        }
        // v9: for a call/jmp into the null page, [RSP] is the return address — it NAMES the caller
        // that jumped to 0. Scan a little deeper for the surrounding call chain.
        if ((uintptr_t)addr < 0x1000 && xp->ContextRecord) {
            uint64_t rsp = xp->ContextRecord->Rsp;
            uint64_t ret = 0;
            if (SafeReadU64((const void*)rsp, &ret)) {
                char cm[320]; ModPathOf((void*)ret, cm, sizeof(cm));
                Log("[VEH] null-call caller: [RSP]=%p (%s)", (void*)ret, cm);
            }
            const uint64_t* sp = (const uint64_t*)rsp;
            int printed = 0;
            for (int i = 0; i < 512 && printed < 12; ++i) {
                uint64_t v = 0;
                if (!SafeReadU64(sp + i, &v)) break;
                if (v < 0x10000 || v > 0x00007FFFFFFFFFFFull) continue;
                HMODULE mod = nullptr;
                if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        (LPCSTR)v, &mod) || !mod) continue;
                char mm[192]; ModOf((void*)v, mm, sizeof(mm));
                Log("[VEH]   sp+0x%03X: %s", i * 8, mm);
                ++printed;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ [D3DDBG] debug layer
// The driver-worker AV can't be explained from its own stack (pure driver). The D3D11 debug layer
// validates every call AS IT IS MADE and names the illegal one. Both the game's device and our dummy
// (whose vtables our hooks are installed from) get D3D11_CREATE_DEVICE_DEBUG, so the hooks sit on the
// validating layer and OUR mirrored calls are checked too. Messages are drained into the log each frame.
typedef HRESULT (WINAPI* PFN_D3D11CreateDevice_)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI* PFN_D3D11CreateDeviceAndSwapChain_)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static PFN_D3D11CreateDevice_ g_origCreateDevice = nullptr;
static PFN_D3D11CreateDeviceAndSwapChain_ g_origCreateDeviceSC = nullptr;
static HRESULT WINAPI HookD3D11CreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* f, UINT nf, UINT sdk, ID3D11Device** d, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** c) {
    UINT fl2 = fl | D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = g_origCreateDevice(a, t, sw, fl2, f, nf, sdk, d, got, c);
    if (FAILED(hr)) { Log("[d3ddbg] CreateDevice with DEBUG failed hr=0x%08X - retrying without", (unsigned)hr); hr = g_origCreateDevice(a, t, sw, fl, f, nf, sdk, d, got, c); }
    else Log("[d3ddbg] game D3D11CreateDevice: DEBUG layer ON (flags 0x%X -> 0x%X)", fl, fl2);
    return hr;
}
static HRESULT WINAPI HookD3D11CreateDeviceSC(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE sw, UINT fl, const D3D_FEATURE_LEVEL* f, UINT nf, UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** d, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** c) {
    UINT fl2 = fl | D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = g_origCreateDeviceSC(a, t, sw, fl2, f, nf, sdk, sd, sc, d, got, c);
    if (FAILED(hr)) { Log("[d3ddbg] CreateDeviceAndSwapChain with DEBUG failed hr=0x%08X - retrying without", (unsigned)hr); hr = g_origCreateDeviceSC(a, t, sw, fl, f, nf, sdk, sd, sc, d, got, c); }
    else Log("[d3ddbg] D3D11CreateDeviceAndSwapChain: DEBUG layer ON (flags 0x%X -> 0x%X)", fl, fl2);
    return hr;
}
// g_infoQ declared at top
static std::unordered_map<int, uint32_t> g_dbgSeen;
static uint32_t g_dbgTotal = 0;
static void DebugLayerAttach(ID3D11Device* dev) {
    if (!kD3DDebugLayer || g_infoQ || !dev) return;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&g_infoQ)) || !g_infoQ) { Log("[d3ddbg] no ID3D11InfoQueue on the game device (debug layer NOT active)"); return; }
    g_infoQ->SetMessageCountLimit(8192);
    g_infoQ->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, FALSE);   // log, don't int3 the game
    g_infoQ->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, FALSE);
    g_infoQ->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_WARNING, FALSE);
    D3D11_MESSAGE_SEVERITY deny[] = { D3D11_MESSAGE_SEVERITY_INFO, D3D11_MESSAGE_SEVERITY_MESSAGE };
    D3D11_INFO_QUEUE_FILTER flt{}; flt.DenyList.NumSeverities = 2; flt.DenyList.pSeverityList = deny;
    g_infoQ->AddStorageFilterEntries(&flt);
    g_infoQ->SetMuteDebugOutput(TRUE);
    Log("[d3ddbg] InfoQueue attached — logging CORRUPTION / ERROR / WARNING messages");
}
static void DebugLayerDrain(const char* when) {
    if (!g_infoQ) return;
    UINT64 n = g_infoQ->GetNumStoredMessagesAllowedByRetrievalFilter();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_infoQ->GetMessage(i, nullptr, &len)) || len == 0 || len > 65536) continue;
        std::vector<uint8_t> buf(len);
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
        if (FAILED(g_infoQ->GetMessage(i, m, &len))) continue;
        ++g_dbgTotal;
        uint32_t cnt = ++g_dbgSeen[(int)m->ID];
        if (cnt <= 3 || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)
            Log("[d3ddbg] %s %s id=%d #%u: %.*s", when,
                m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : m->Severity == D3D11_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING",
                (int)m->ID, cnt, (int)(m->DescriptionByteLength > 0 ? m->DescriptionByteLength - 1 : 0), m->pDescription);
    }
    g_infoQ->ClearStoredMessages();
}
// ------------------------------------------------------------------ init
static void InitProbe() {
    OpenLog();
    { HANDLE hb = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr); if (hb) CloseHandle(hb); }
    Log("[init] P5RPROBE v22 - [rtcensus]: every render target that takes draws, in bind order, so P5R's real UI target names itself. Plus v21 XR-swapchain release across resizes. PID=%lu", GetCurrentProcessId());
    { char exe[MAX_PATH]{}; GetModuleFileNameA(nullptr, exe, MAX_PATH); Log("[init] exe=%s base=%p", exe, (void*)GetModuleHandleA(nullptr)); }
    AddVectoredExceptionHandler(1, VehLogger);
    Log("[init] EARLY native hook install: no startup sleep; capturing deferred contexts before P5R creates them");
    Log("%s", ResSpoof::report);
    { const MH_STATUS mh = MH_Initialize(); if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) { Log("[init] MH_Initialize FAILED"); return; } }
    P5RInput::Install(HookFn);
    HMODULE user32=GetModuleHandleW(L"user32.dll");
    if(user32) {
        HookFn((void*)GetProcAddress(user32,"SetCursorPos"),(void*)P5RMenu::BlockGameCursorWarp,
            (void**)&P5RMenu::originalSetCursorPos,"Menu::SetCursorPos");
        HookFn((void*)GetProcAddress(user32,"SetPhysicalCursorPos"),(void*)P5RMenu::BlockGamePhysicalWarp,
            (void**)&P5RMenu::originalSetPhysicalCursorPos,"Menu::SetPhysicalCursorPos");
        HookFn((void*)GetProcAddress(user32,"ClipCursor"),(void*)P5RMenu::BlockGameCursorClip,
            (void**)&P5RMenu::originalClipCursor,"Menu::ClipCursor");
    }
    if (kD3DDebugLayer) {
        HMODULE d3d = GetModuleHandleA("d3d11.dll"); if (!d3d) d3d = LoadLibraryA("d3d11.dll");
        void* pCD = d3d ? (void*)GetProcAddress(d3d, "D3D11CreateDevice") : nullptr;
        void* pCDSC = d3d ? (void*)GetProcAddress(d3d, "D3D11CreateDeviceAndSwapChain") : nullptr;
        if (pCD) HookFn(pCD, (void*)HookD3D11CreateDevice, (void**)&g_origCreateDevice, "D3D11CreateDevice (debug flag)");
        if (pCDSC) HookFn(pCDSC, (void*)HookD3D11CreateDeviceSC, (void**)&g_origCreateDeviceSC, "D3D11CreateDeviceAndSwapChain (debug flag)");
    }

    HWND hwnd = CreateWindowExA(0, "STATIC", "p5r_probe_dummy", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 3,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
    if (FAILED(hr) || !sc) { Log("[init] dummy D3D11CreateDeviceAndSwapChain FAILED hr=0x%08X", (unsigned)hr); if (hwnd) DestroyWindow(hwnd); return; }

    { void** vt = *(void***)sc;
      g_initialPresentTarget = vt[VT_SC_Present];
      g_resizeCodeTarget     = vt[VT_SC_ResizeBuffers];
      HookFn(vt[VT_SC_Present],       (void*)HookPresent,       (void**)&g_origPresent,       "SC::Present");
      HookFn(vt[VT_SC_ResizeBuffers], (void*)HookResizeBuffers, (void**)&g_origResizeBuffers, "SC::ResizeBuffers");
      HookFn(vt[VT_SC_SetFullscreenState], (void*)HookSetFullscreenState, (void**)&g_origSetFullscreenState, "SC::SetFullscreenState");
      IDXGISwapChain1* sc1 = nullptr;
      if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
          void** vt1 = *(void***)sc1;
          g_initialPresent1Target = vt1[VT_SC_Present1];
          HookFn(vt1[VT_SC_Present1], (void*)HookPresent1, (void**)&g_origPresent1, "SC::Present1");
          sc1->Release();
      } else Log("[init] no IDXGISwapChain1 on dummy — Present1 not hooked");
      // v7: snapshot the hooked entries' first bytes (JMP included) — the stall check compares
      // against these to prove/disprove detour rewriting.
      bool b0 = SafeRead16(g_initialPresentTarget, g_presentBytesBoot);
      bool b1 = g_initialPresent1Target ? SafeRead16(g_initialPresent1Target, g_present1BytesBoot) : false;
      bool b2 = SafeRead16(g_resizeCodeTarget, g_resizeBytesBoot);
      g_bootBytesValid = b0 && b2;
      Log("[init] detour byte snapshots: present=%d present1=%d resize=%d", (int)b0, (int)b1, (int)b2);
    }
    // v7: swapchain-creation visibility — hook the real factory (parent of the dummy chain). Any
    // swapchain the game (or a proxy above us) creates at the title transition gets logged and,
    // if game-sized, auto-adopted (device/context refresh + late Present hooks).
    { IDXGIFactory* fac = nullptr;
      if (SUCCEEDED(sc->GetParent(__uuidof(IDXGIFactory), (void**)&fac)) && fac) {
          void** fvt = *(void***)fac;
          HookFn(fvt[VT_FAC_CreateSwapChain], (void*)HookFacCreateSwapChain,
                 (void**)&g_origFacCreateSwapChain, "FAC::CreateSwapChain");
          IDXGIFactory2* fac2 = nullptr;
          if (SUCCEEDED(fac->QueryInterface(__uuidof(IDXGIFactory2), (void**)&fac2)) && fac2) {
              void** f2vt = *(void***)fac2;
              HookFn(f2vt[VT_FAC2_CreateSwapChainForHwnd], (void*)HookFacCreateSwapChainForHwnd,
                     (void**)&g_origFacCreateSwapChainForHwnd, "FAC2::CreateSwapChainForHwnd");
              fac2->Release();
          } else Log("[init] no IDXGIFactory2 — CreateSwapChainForHwnd not hooked");
          fac->Release();
      } else Log("[init] GetParent(IDXGIFactory) FAILED — creation hooks not installed");
    }
    // v9: deferred-draw visibility WITHOUT racing the game's context creation. P5R renders almost
    // everything through deferred contexts it creates during our init window; hooking
    // DEV::CreateDeferredContext first (to catch them) put boot into the config that crashes at
    // frame 1 (RIP=0 — v7a `7a1773b4` and v8 `63667b4f`, identical signature, mutex in place).
    // Instead: our own dummy deferred context exposes the SAME d3d11 code addresses the game's
    // contexts execute — hook those here, single-threaded, and let IsDeferredContext classify the
    // game's context objects lazily on first sight.
    {
        ID3D11DeviceContext* dummyDef = nullptr;
        if (dev && SUCCEEDED(dev->CreateDeferredContext(0, &dummyDef)) && dummyDef) {
            HookContextMethods(dummyDef, true);
            dummyDef->Release();
            Log("[init] DCTX code hooks installed via dummy deferred context (lazy classification live)");
        } else Log("[init] dummy CreateDeferredContext FAILED — deferred visibility depends on DEV hook only");
    }
    // DEV::CreateDeferredContext stays hooked for registration/logging of late-created contexts,
    // but LAST — the proven-booting order (v7b `737f6d58`).
    if (dev) {
        void** dvt = *(void***)dev;
        HookFn(dvt[5], (void*)HookCreateTex2D, (void**)&g_origCreateTex2D, "DEV::CreateTexture2D (rendersize recon)");
        HookFn(dvt[VT_DEV_CreateDeferredContext], (void*)HookCreateDeferredContext,
               (void**)&g_origCreateDeferredContext, "DEV::CreateDeferredContext");
    }
    Log("[init] P5RPROBE v22 - [rtcensus]: every render target that takes draws, in bind order, so P5R's real UI target names itself. Plus v21 XR-swapchain release across resizes. PID=%lu");
    HANDLE controller = CreateThread(nullptr, 0, RenderPathControllerThread, nullptr, 0, nullptr);
    if (controller) CloseHandle(controller);
    HANDLE diag = CreateThread(nullptr, 0, StallDiagThread, nullptr, 0, nullptr);
    if (diag) CloseHandle(diag);

    if (ctx) ctx->Release(); if (dev) dev->Release(); if (sc) sc->Release(); if (hwnd) DestroyWindow(hwnd);
}
static DWORD WINAPI InitThread(LPVOID) { InitProbe(); return 0; }

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(hInst); ResSpoof::InstallAtAttach(); CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr); }
    return TRUE;
}
