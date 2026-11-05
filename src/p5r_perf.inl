// [PERF] Where does the frame time go? Measured, not guessed. Every 120 frames:
//  - GPU time of the frame (timestamp queries around Present-to-Present on the immediate context)
//  - CPU time inside our draw hooks (all threads summed), of which the mirror's right-eye replay
//  - CPU time inside our state hooks (render targets, constant-buffer uploads, maps)
//  - CPU time in the game's ExecuteCommandList, our Present work, xrWaitFrame, and the real Present
// Numpad 5/6/7 switch stereo / wide visibility / HUD layer live so each cost is isolated in one run.
struct PerfCounter { std::atomic<uint64_t> ticks{0}; std::atomic<uint32_t> calls{0}; };
static PerfCounter g_perfDraw, g_perfMirror, g_perfState, g_perfExec, g_perfPresentCall, g_perfOurPresent, g_perfXrWait, g_perfSubmit;
// [THREADPROF] Run 11: after batching the GPU sits 53-75% busy at ~60 fps, so the frame is now held by
// something else. Per thread: total CPU (every thread of the process) and the part spent inside our
// hooks (outermost scope only, so nested scopes are not counted twice).
struct PerfThread { std::atomic<DWORD> tid{0}; std::atomic<uint64_t> ours{0}, driver{0}; std::atomic<uint32_t> draws{0}; };
static PerfThread g_perfThreads[64];
static thread_local PerfThread* t_perfThread = nullptr;
static thread_local int t_perfDepth = 0;
static PerfThread* PerfThreadSlot() {
    if (t_perfThread) return t_perfThread;
    const DWORD me = GetCurrentThreadId();
    for (auto& t : g_perfThreads) { DWORD expect = 0; if (t.tid.load() == me || t.tid.compare_exchange_strong(expect, me)) { t_perfThread = &t; return &t; } }
    static PerfThread overflow; t_perfThread = &overflow; return &overflow;
}
struct PerfScope {
    PerfCounter& c; LARGE_INTEGER t0;
    explicit PerfScope(PerfCounter& counter) : c(counter) { ++t_perfDepth; QueryPerformanceCounter(&t0); }
    ~PerfScope() {
        LARGE_INTEGER t1; QueryPerformanceCounter(&t1); const uint64_t d = (uint64_t)(t1.QuadPart - t0.QuadPart);
        c.ticks.fetch_add(d, std::memory_order_relaxed); c.calls.fetch_add(1, std::memory_order_relaxed);
        if (--t_perfDepth == 0) PerfThreadSlot()->ours.fetch_add(d, std::memory_order_relaxed);
    }
};
// [CPUBREAK] Run 12: the scene-recording worker spends 10-17 ms/frame inside our hooks, but that
// includes the driver recording the game's OWN calls (its draw, map, unmap, target binds). Split it:
// GameCall times the game's own call inside a hook; PerfPart times one part of our work.
static PerfCounter g_perfUpdHazard, g_perfUpdCam, g_perfUpdNote, g_perfUpdGame, g_perfSnap;
static std::atomic<uint64_t> g_updCalls{0}, g_updBytes{0};
static PerfCounter g_perfGameDrv, g_perfPreDraw, g_perfHud, g_perfRecord, g_perfFlush, g_perfUnmapHook, g_perfMapHook, g_perfUpdHook, g_perfRTHook, g_perfSetCBHook;
struct PerfPart {
    PerfCounter& c; LARGE_INTEGER t0;
    explicit PerfPart(PerfCounter& counter) : c(counter) { QueryPerformanceCounter(&t0); }
    ~PerfPart() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); c.ticks.fetch_add((uint64_t)(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed); }
};
struct GameCall {
    LARGE_INTEGER t0;
    GameCall() { QueryPerformanceCounter(&t0); }
    ~GameCall() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); const uint64_t d = (uint64_t)(t1.QuadPart - t0.QuadPart);
        g_perfGameDrv.ticks.fetch_add(d, std::memory_order_relaxed); PerfThreadSlot()->driver.fetch_add(d, std::memory_order_relaxed); }
};
static double PerfTicksToMs(uint64_t t) { static LARGE_INTEGER fq{}; if (!fq.QuadPart) QueryPerformanceFrequency(&fq); return 1000.0 * (double)t / (double)fq.QuadPart; }

// GPU frame time: a ring of disjoint + 2 timestamps, read 3 frames late without stalling
struct GpuSlot { ID3D11Query* disjoint = nullptr; ID3D11Query* begin = nullptr; ID3D11Query* end = nullptr; bool open = false, pending = false; };
static GpuSlot g_gpuSlots[4];
static int g_gpuCur = -1;
static double g_gpuSumMs = 0; static uint32_t g_gpuSamples = 0;
static void PerfGpuFrameBoundary() {
    if (!g_dev || !g_ctxImm) return;
    if (g_gpuCur >= 0) { GpuSlot& s = g_gpuSlots[g_gpuCur]; if (s.open) { g_ctxImm->End(s.end); g_ctxImm->End(s.disjoint); s.open = false; s.pending = true; } }
    for (auto& s : g_gpuSlots) {   // collect finished slots
        if (!s.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{}; UINT64 b = 0, e = 0;
        if (g_ctxImm->GetData(s.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (g_ctxImm->GetData(s.begin, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (g_ctxImm->GetData(s.end, &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        s.pending = false;
        if (!dj.Disjoint && dj.Frequency && e > b) { g_gpuSumMs += 1000.0 * (double)(e - b) / (double)dj.Frequency; ++g_gpuSamples; }
    }
    int next = -1;
    for (int i = 0; i < 4; ++i) { int k = (g_gpuCur + 1 + i) & 3; if (!g_gpuSlots[k].pending && !g_gpuSlots[k].open) { next = k; break; } }
    if (next < 0) { g_gpuCur = -1; return; }
    GpuSlot& s = g_gpuSlots[next];
    if (!s.disjoint) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; D3D11_QUERY_DESC qt{ D3D11_QUERY_TIMESTAMP, 0 };
        if (FAILED(g_dev->CreateQuery(&qd, &s.disjoint)) || FAILED(g_dev->CreateQuery(&qt, &s.begin)) || FAILED(g_dev->CreateQuery(&qt, &s.end))) { g_gpuCur = -1; return; }
    }
    g_ctxImm->Begin(s.disjoint); g_ctxImm->End(s.begin); s.open = true; g_gpuCur = next;
}

// GPU busy % straight from the NVIDIA driver (NVML), and how busy the render thread / process are.
typedef int (*PFN_nvmlInit)(); typedef int (*PFN_nvmlGetHandle)(unsigned, void**);
struct NvmlUtil { unsigned gpu, memory; }; typedef int (*PFN_nvmlGetUtil)(void*, NvmlUtil*);
static PFN_nvmlGetUtil g_nvmlUtil = nullptr; static void* g_nvmlDev = nullptr; static bool g_nvmlTried = false;
static int PerfGpuBusy() {
    if (!g_nvmlTried) {
        g_nvmlTried = true;
        HMODULE m = LoadLibraryW(L"nvml.dll");
        if (!m) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat_s(p, L"\\nvml.dll"); m = LoadLibraryW(p); }
        auto init = m ? (PFN_nvmlInit)GetProcAddress(m, "nvmlInit_v2") : nullptr;
        auto handle = m ? (PFN_nvmlGetHandle)GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2") : nullptr;
        auto util = m ? (PFN_nvmlGetUtil)GetProcAddress(m, "nvmlDeviceGetUtilizationRates") : nullptr;
        if (init && handle && util && init() == 0 && handle(0, &g_nvmlDev) == 0) g_nvmlUtil = util;
        Log("[perf] NVML GPU utilisation %s", g_nvmlUtil ? "available" : "NOT available");
    }
    NvmlUtil u{};
    return (g_nvmlUtil && g_nvmlUtil(g_nvmlDev, &u) == 0) ? (int)u.gpu : -1;
}
static uint64_t FileTimeTicks(const FILETIME& a) { return ((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime; }   // 100 ns units
static void PerfThreadReport(double frames) {
    struct Seen { HANDLE h; uint64_t last; };
    static std::map<DWORD, Seen> s_seen;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    struct Row { DWORD tid; double cpu, ours, drv; };
    std::vector<Row> rows; std::map<DWORD, bool> alive;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        alive[te.th32ThreadID] = true;
        auto it = s_seen.find(te.th32ThreadID);
        if (it == s_seen.end()) {
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            it = s_seen.emplace(te.th32ThreadID, Seen{ h, 0 }).first;
        }
        FILETIME c0, e0, k0, u0;
        if (!GetThreadTimes(it->second.h, &c0, &e0, &k0, &u0)) continue;
        const uint64_t t = FileTimeTicks(k0) + FileTimeTicks(u0);
        const uint64_t last = it->second.last; it->second.last = t;
        if (!last) continue;
        rows.push_back({ te.th32ThreadID, (t - last) / 10000.0 / frames, 0.0, 0.0 });
    }
    CloseHandle(snap);
    for (auto it = s_seen.begin(); it != s_seen.end();) { if (!alive.count(it->first)) { CloseHandle(it->second.h); it = s_seen.erase(it); } else ++it; }
    for (auto& t : g_perfThreads) { const DWORD tid = t.tid.load(); if (!tid) continue;
        const double ours = PerfTicksToMs(t.ours.exchange(0)) / frames, drv = PerfTicksToMs(t.driver.exchange(0)) / frames;
        for (auto& r : rows) if (r.tid == tid) { r.ours = ours; r.drv = drv; } }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu > b.cpu; });
    char line[700]; int n = snprintf(line, sizeof(line), "[threads] busiest (CPU ms/frame; in our hooks; of that the game's own D3D calls):");
    for (size_t i = 0; i < rows.size() && i < 6 && n < 600; ++i)
        n += snprintf(line + n, sizeof(line) - n, " | tid %lu%s %.1f (hooks %.1f, game calls %.1f)", (unsigned long)rows[i].tid,
            rows[i].tid == GetCurrentThreadId() ? " [present]" : "", rows[i].cpu, rows[i].ours, rows[i].drv);
    Log("%s", line);
}
static void PerfReport(uint64_t f, bool stereo, bool wide, bool hud) {
    static LARGE_INTEGER s_last{}; static uint64_t s_lastFrame = 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (!s_last.QuadPart) { s_last = now; s_lastFrame = f; return; }
    if (f - s_lastFrame < 120) return;
    const double frames = (double)(f - s_lastFrame), wallMs = PerfTicksToMs((uint64_t)(now.QuadPart - s_last.QuadPart));
    auto per = [&](PerfCounter& c) { return PerfTicksToMs(c.ticks.exchange(0)) / frames; };
    auto calls = [&](PerfCounter& c) { return c.calls.exchange(0) / frames; };
    const double drawCalls = calls(g_perfDraw);
    const double gpu = g_gpuSamples ? g_gpuSumMs / g_gpuSamples : -1;
    // render thread (this thread) and whole-process CPU, per frame
    static uint64_t s_thr = 0, s_proc = 0;
    FILETIME c0, e0, k0, u0; uint64_t thr = 0, proc = 0;
    if (GetThreadTimes(GetCurrentThread(), &c0, &e0, &k0, &u0)) thr = FileTimeTicks(k0) + FileTimeTicks(u0);
    if (GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0)) proc = FileTimeTicks(k0) + FileTimeTicks(u0);
    const double thrMs = s_thr ? (thr - s_thr) / 10000.0 / frames : -1, procMs = s_proc ? (proc - s_proc) / 10000.0 / frames : -1;
    s_thr = thr; s_proc = proc;
    const int gpuBusy = PerfGpuBusy();
    Log("[perf] f%llu fps=%.1f frame=%.1fms | GPU busy=%d%% | render thread CPU=%.1fms/frame | process CPU=%.1fms/frame (all cores) | GPU span=%.1fms | real Present=%.1fms | our Present work=%.1fms (xrWaitFrame=%.1f submit=%.1f) | ExecuteCommandList=%.1fms"
        " | draw hooks CPU=%.1fms/frame over %.0f draws (mirror replay=%.1fms) | state hooks CPU=%.1fms | stereo=%d wide=%d hud=%d",
        (unsigned long long)f, 1000.0 * frames / wallMs, wallMs / frames, gpuBusy, thrMs, procMs, gpu, per(g_perfPresentCall), per(g_perfOurPresent), per(g_perfXrWait), per(g_perfSubmit),
        per(g_perfExec), per(g_perfDraw), drawCalls, per(g_perfMirror), per(g_perfState), (int)stereo, (int)wide, (int)hud);
    calls(g_perfMirror); calls(g_perfState); calls(g_perfExec); calls(g_perfPresentCall); calls(g_perfOurPresent); calls(g_perfXrWait); calls(g_perfSubmit);
    PerfThreadReport(frames);
    {   auto ms = [&](PerfCounter& c) { return PerfTicksToMs(c.ticks.exchange(0)) / frames; };
        const double drv = ms(g_perfGameDrv), pre = ms(g_perfPreDraw), hud = ms(g_perfHud), rec = ms(g_perfRecord), fl = ms(g_perfFlush);
        const double un = ms(g_perfUnmapHook), mp = ms(g_perfMapHook), up = ms(g_perfUpdHook), rt = ms(g_perfRTHook), cb = ms(g_perfSetCBHook);
        Log("[cpu] all threads, ms/frame: game's own D3D calls %.1f | ours: mirror pre-draw %.1f, HUD pre/post %.1f, record right eye %.1f, replay right eye %.1f |"
            " hooks incl. their game call: Unmap %.1f, Map %.1f, UpdateSubresource %.1f, OMSetRenderTargets %.1f (incl. replay), Set CBs %.1f",
            drv, pre, hud, rec, fl, un, mp, up, rt, cb);
        const double calls = g_updCalls.exchange(0) / frames, bytes = g_updBytes.exchange(0) / frames;
        Log("[cpu] UpdateSubresource split, ms/frame: game's own upload %.1f | preserve pending right-eye data %.1f (of which snapshots %.1f) | camera/twin handling %.1f | remember bytes %.1f | %.0f calls/frame, %.0f bytes avg",
            ms(g_perfUpdGame), ms(g_perfUpdHazard), ms(g_perfSnap), ms(g_perfUpdCam), ms(g_perfUpdNote), calls, calls > 0 ? bytes / calls : 0.0); }
    g_gpuSumMs = 0; g_gpuSamples = 0; s_last = now; s_lastFrame = f;
}

// Numpad 9: automatic isolation run, 15 s per phase, logged. Nothing to read or time in the headset.
static int g_perfPhase = -1; static ULONGLONG g_perfPhaseStart = 0;
static void PerfSequence(bool keyPressed, bool& wantWide, bool& wantStereo) {
    const ULONGLONG now = GetTickCount64();
    if (keyPressed && g_perfPhase < 0) { g_perfPhase = 0; g_perfPhaseStart = now; Log("[perf] SEQUENCE start: phase 0 = everything on (15 s)"); }
    if (g_perfPhase < 0 || now - g_perfPhaseStart < 15000) return;
    ++g_perfPhase; g_perfPhaseStart = now;
    switch (g_perfPhase) {
    case 1: wantWide = false; Log("[perf] SEQUENCE phase 1 = wide visibility OFF (15 s)"); break;
    case 2: wantWide = true; wantStereo = false; Log("[perf] SEQUENCE phase 2 = stereo OFF, wide visibility ON (15 s)"); break;
    case 3: wantStereo = true; Log("[perf] SEQUENCE phase 3 = everything restored (15 s)"); break;
    default: Log("[perf] SEQUENCE done"); g_perfPhase = -1; break;
    }
}
