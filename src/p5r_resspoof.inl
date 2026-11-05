// [RESSPOOF] Sharpness. Each eye image is the game's window image, 3840x2119 (16:9 minus the title bar),
// stretched over a ~120x122 degree, roughly SQUARE headset view: ~32 px/deg across but only ~17 px/deg
// down. ME1 lesson (memory me1-render-above-display-res): the game clamps its requested resolution to
// the monitor rect read with GetMonitorInfoW; spoof that one call and it renders above the display.
// P5R imports the same calls (GetMonitorInfoW, MonitorFromWindow, GetSystemMetrics, AdjustWindowRect,
// GetClientRect). The requested size is P5R's own ScreenSetting.xml (resolution_width/height). The
// spoof arms only when that request is larger than the real primary monitor; otherwise every hook is a
// pass-through. Windowed mode: Windows also clamps a window to the screen through WM_GETMINMAXINFO,
// so DefWindowProc's answer is widened to fit the requested client size. Installed at DLL attach
// (the game reads the monitor before any later hook would exist).
namespace ResSpoof {
static int reqW = 0, reqH = 0;           // ScreenSetting.xml request
static int realW = 0, realH = 0;         // real primary monitor
static bool armed = false;
static char report[512] = "";
static std::atomic<uint32_t> monCalls{0}, monSpoofed{0}, minmaxRaised{0};
typedef BOOL (WINAPI* PFN_GetMonitorInfoW)(HMONITOR, LPMONITORINFO);
typedef LRESULT (WINAPI* PFN_DefWindowProcW)(HWND, UINT, WPARAM, LPARAM);
static PFN_GetMonitorInfoW origGetMonitorInfoW = nullptr;
static PFN_DefWindowProcW origDefWindowProcW = nullptr, origDefWindowProcA = nullptr;

static int XmlInt(const char* s, const char* key) {
    const char* p = strstr(s, key);
    if (!p) return 0;
    p = strchr(p, '"');
    return p ? atoi(p + 1) : 0;
}
static bool ReadRequest() {
    char appdata[MAX_PATH]; if (!GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) return false;
    char pattern[MAX_PATH]; snprintf(pattern, sizeof(pattern), "%s\\SEGA\\P5R\\Steam\\*", appdata);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char path[MAX_PATH]; snprintf(path, sizeof(path), "%s\\SEGA\\P5R\\Steam\\%s\\ScreenSetting.xml", appdata, fd.cFileName);
        FILE* f = fopen(path, "rb"); if (!f) continue;
        char buf[2048] = {}; fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
        const int w = XmlInt(buf, "resolution_width="), hh = XmlInt(buf, "resolution_height=");
        if (w > 0 && hh > 0) { reqW = w; reqH = hh; ok = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}
static bool IsPrimary(const MONITORINFO* mi) { return mi && (mi->dwFlags & MONITORINFOF_PRIMARY); }
static BOOL WINAPI HookGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    const BOOL r = origGetMonitorInfoW(m, mi);
    monCalls.fetch_add(1, std::memory_order_relaxed);
    if (r && armed && IsPrimary(mi) && mi->cbSize >= sizeof(MONITORINFO)) {
        // grow the primary rect from its origin so the requested window (plus frame) fits
        const int w = (std::max)(realW, reqW + 64), h = (std::max)(realH, reqH + 128);
        mi->rcMonitor.right = mi->rcMonitor.left + w; mi->rcMonitor.bottom = mi->rcMonitor.top + h;
        mi->rcWork = mi->rcMonitor;
        monSpoofed.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}
static void RaiseMinMax(UINT msg, LPARAM l) {
    if (msg != WM_GETMINMAXINFO || !armed || !l) return;
    MINMAXINFO* mm = (MINMAXINFO*)l;
    const LONG needW = reqW + 128, needH = reqH + 256;   // client + caption/borders
    if (mm->ptMaxTrackSize.x < needW) mm->ptMaxTrackSize.x = needW;
    if (mm->ptMaxTrackSize.y < needH) mm->ptMaxTrackSize.y = needH;
    if (mm->ptMaxSize.x < needW) mm->ptMaxSize.x = needW;
    if (mm->ptMaxSize.y < needH) mm->ptMaxSize.y = needH;
    minmaxRaised.fetch_add(1, std::memory_order_relaxed);
}
static LRESULT WINAPI HookDefWindowProcW(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const LRESULT r = origDefWindowProcW(h, msg, w, l); RaiseMinMax(msg, l); return r;
}
static LRESULT WINAPI HookDefWindowProcA(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const LRESULT r = origDefWindowProcA(h, msg, w, l); RaiseMinMax(msg, l); return r;
}
// [ASPECT] Run 2 at 3840x3840: square window, but the engine kept its 3D targets 3840x2160 and
// letterboxed them (composite viewport y=840 h=2160) -> "squashed vertically" in the headset.
// The size routine (P5R+0x1A1910, inlined again at +0x1A19A0) reads the window W,H and does:
//     h' = W * [9/16];  if (h' < H) H = h' (letterbox)  else W = H * [16/9] (pillarbox)
// The community ultrawide fix (MonoHearted/p5rpc.ultrawide, read only) rewrites the 16/9 global,
// which only covers WIDER windows; a taller window, Hinny tha GOAT, takes the 9/16 branch (run 3: still 2160).
// Both constants are SHARED with other game code (9/16: 9 code refs, 16/9: 6), so they are not
// touched. Instead the 4 multiply instructions inside the two size routines are repointed at the
// mod's own pair { k = H/W, a = W/H } (in memory allocated within +-2 GB of the exe), updated at
// every SetResolution. Then h' = H (not < H) and W = H * a = W: the 3D view is the window's exact
// shape. A 16:9 window gives k = 9/16, a = 16/9: identical to the stock game.
typedef void (__fastcall* PFN_SetResolution)(void* dest, int width, int height);
static PFN_SetResolution origSetResolution = nullptr;
static std::atomic<uint32_t> setResCalls{0};
static float* aspectPair = nullptr;   // [0] = k (H/W), [1] = a (W/H)
static int aspectPatched = 0;
static uint8_t* FindPattern(uint8_t* base, size_t size, const char* sig, size_t from = 0) {
    int pat[64]; int n = 0;
    for (const char* q = sig; *q && n < 64;) {
        while (*q == ' ') ++q; if (!*q) break;
        if (q[0] == '?') { pat[n++] = -1; q += (q[1] == '?') ? 2 : 1; }
        else { pat[n++] = (int)strtoul(q, nullptr, 16); q += 2; }
    }
    for (size_t i = from; i + n <= size; ++i) {
        int k = 0; for (; k < n; ++k) if (pat[k] >= 0 && base[i + k] != (uint8_t)pat[k]) break;
        if (k == n) return base + i;
    }
    return nullptr;
}
static void SetAspectPair(int w, int h) {
    if (!aspectPair || w <= 0 || h <= 0) return;
    aspectPair[0] = (float)h / (float)w * 1.0001f;   // h' = W*k lands at (not below) H: pillarbox branch, no letterbox
    aspectPair[1] = (float)w / (float)h;
}
static void __fastcall HookSetResolution(void* dest, int width, int height) {
    setResCalls.fetch_add(1, std::memory_order_relaxed);
    SetAspectPair(width, height);
    origSetResolution(dest, width, height);
}
static void* AllocNear(uint8_t* base) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t a = ((uintptr_t)base - gran) & ~(gran - 1), lo = (uintptr_t)base - 0x70000000ull; a > lo && a > gran; a -= gran)
        if (void* p = VirtualAlloc((void*)a, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return p;
    return nullptr;
}
// Repoint `mulss xmm0, [rip+disp32]` (F3 0F 59 05 disp32) at `ins` to `target`.
static bool Repoint(uint8_t* ins, const float* target) {
    if (ins[0] != 0xF3 || ins[1] != 0x0F || ins[2] != 0x59 || ins[3] != 0x05) return false;
    const long long disp = (long long)((const uint8_t*)target - (ins + 8));
    if (disp > INT32_MAX || disp < INT32_MIN) return false;
    DWORD old = 0;
    if (!VirtualProtect(ins + 4, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(ins + 4) = (int32_t)disp;
    VirtualProtect(ins + 4, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ins, 8);
    return true;
}
static bool InstallAspect(char* why, size_t cap) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)exe; auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + dos->e_lfanew);
    uint8_t* base = (uint8_t*)exe;
    const char* kSizeSig = "F3 0F 59 05 ?? ?? ?? ?? F3 48 ?? ?? ?? 3B ?? 72 ?? 0F 57 ?? F3 48 ?? ?? ?? F3 0F 59 05";
    uint8_t* sites[4] = {}; int nSites = 0; uint8_t* setRes = nullptr;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* s0 = base + sec->VirtualAddress; const size_t sz = sec->Misc.VirtualSize;
        for (size_t from = 0; nSites < 4;) {
            uint8_t* m = FindPattern(s0, sz, kSizeSig, from);
            if (!m) break;
            sites[nSites++] = m; from = (size_t)(m - s0) + 1;
        }
        if (!setRes) setRes = FindPattern(s0, sz, "85 D2 7E ?? 45 85 C0 7E ?? 89 51 1C 44 89 41 20");
    }
    if (nSites != 2 || !setRes) { snprintf(why, cap, "signatures: %d size routines (want 2), SetResolution %p: not patching", nSites, setRes); return false; }
    // both routines must read the stock 9/16 and 16/9 constants
    for (int i = 0; i < 2; ++i) {
        const float* k = (const float*)(sites[i] + 8 + *(int32_t*)(sites[i] + 4));
        const float* a = (const float*)(sites[i] + 25 + 8 + *(int32_t*)(sites[i] + 25 + 4));
        if (fabsf(*k - 0.5625f) > 1e-5f || fabsf(*a - 1.7777778f) > 1e-4f) { snprintf(why, cap, "size routine +0x%llX reads %.4f / %.4f, not 9/16 / 16/9: not patching", (unsigned long long)(sites[i] - base), *k, *a); return false; }
    }
    aspectPair = (float*)AllocNear(base);
    if (!aspectPair) { snprintf(why, cap, "no memory within 2 GB of the exe: not patching"); return false; }
    SetAspectPair(reqW, reqH);
    if (MH_CreateHook(setRes, (void*)HookSetResolution, (void**)&origSetResolution) != MH_OK || MH_EnableHook(setRes) != MH_OK) { snprintf(why, cap, "SetResolution hook failed: not patching"); return false; }
    for (int i = 0; i < 2; ++i) { aspectPatched += Repoint(sites[i], &aspectPair[0]); aspectPatched += Repoint(sites[i] + 25, &aspectPair[1]); }
    snprintf(why, cap, "size routines +0x%llX/+0x%llX: %d/4 multiplies repointed to k=%.4f a=%.4f (shared 9/16 and 16/9 untouched), SetResolution +0x%llX hooked",
        (unsigned long long)(sites[0] - base), (unsigned long long)(sites[1] - base), aspectPatched, aspectPair[0], aspectPair[1], (unsigned long long)(setRes - base));
    return aspectPatched == 4;
}
// DllMain, attach. No logging here (the log opens later); the result is kept in `report`.
static void InstallAtAttach() {
    POINT origin{ 0, 0 };
    HMONITOR prim = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(mi) };
    if (prim && GetMonitorInfoW(prim, &mi)) { realW = mi.rcMonitor.right - mi.rcMonitor.left; realH = mi.rcMonitor.bottom - mi.rcMonitor.top; }
    const bool haveReq = ReadRequest();
    // 2026-10-06: square render works but the game's 2D menus/HUD become square too (unusable), so the
    // the default went back to 16:9. Arm only for a non-16:9 request, so a 16:9 window is exactly the stock game.
    armed = haveReq && realW > 0 && (reqW > realW || reqH > realH) && reqW * 9 != reqH * 16;
    if (armed) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 && MH_Initialize() == MH_OK) {
            void* gmi = (void*)GetProcAddress(user32, "GetMonitorInfoW");
            void* dwpW = (void*)GetProcAddress(user32, "DefWindowProcW");
            void* dwpA = (void*)GetProcAddress(user32, "DefWindowProcA");
            bool ok = gmi && MH_CreateHook(gmi, (void*)HookGetMonitorInfoW, (void**)&origGetMonitorInfoW) == MH_OK && MH_EnableHook(gmi) == MH_OK;
            if (dwpW && MH_CreateHook(dwpW, (void*)HookDefWindowProcW, (void**)&origDefWindowProcW) == MH_OK) MH_EnableHook(dwpW);
            if (dwpA && dwpA != dwpW && MH_CreateHook(dwpA, (void*)HookDefWindowProcA, (void**)&origDefWindowProcA) == MH_OK) MH_EnableHook(dwpA);
            if (!ok) armed = false;
        } else armed = false;
    }
    char aspectWhy[200] = "not needed (16:9 request)";
    if (haveReq && reqW * 9 != reqH * 16) {   // a non-16:9 window: the 3D view must follow it
        if (MH_Initialize() == MH_OK || true) InstallAspect(aspectWhy, sizeof(aspectWhy));
    }
    snprintf(report, sizeof(report), "[resspoof] ScreenSetting.xml requests %dx%d%s, primary monitor %dx%d -> %s | [aspect] %s",
        reqW, reqH, haveReq ? "" : " (not found)", realW, realH,
        armed ? "SPOOF ARMED (game may render above the monitor)" : "pass-through (request fits the monitor)", aspectWhy);
}
}
