// [GPUPROF] Per-pass GPU time for one live frame, measured with D3D11 timestamp queries. A timestamp
// is ended at every render-target change on every context (deferred lists record them; the GPU writes
// them as it executes) and around every mirrored right-eye replay. After the frame, timestamps are
// sorted on the GPU timeline: the gap after each pass-start timestamp is that pass's GPU time,
// labelled by the target it renders into. Captures itself (live world, busy frame), a few per run.
struct GpuProfMark { ID3D11Query* q; uint32_t w, h, fmt, dsv, ctx, draws; uint8_t kind; };   // kind 0 pass start, 1 mirror begin, 2 mirror end
static std::mutex g_gpMx;
static std::vector<GpuProfMark> g_gpMarks;
static std::vector<ID3D11Query*> g_gpPool;
static std::atomic<uint32_t> g_gpPoolNext{0};
static std::atomic<bool> g_gpCapturing{false};
static ID3D11Query* g_gpDisjoint = nullptr;
static int g_gpState = 0;          // 0 idle, 1 capturing, 2 waiting for results
static int g_gpWait = 0, g_gpCaptures = 0;
static uint64_t g_gpNextFrame = 0;
static int g_gpDiag = 0;           // Numpad . / auto: 1 next capture current, 2 next capture per-draw replay, 3 per-draw capture running
static const char* g_gpLabel = "auto";
static thread_local int t_gpLastMark = -1;
static ID3D11Query* GpuProfQuery() {
    const uint32_t i = g_gpPoolNext.fetch_add(1, std::memory_order_relaxed);
    return i < g_gpPool.size() ? g_gpPool[i] : nullptr;
}
static void GpuProfMarkAt(ID3D11DeviceContext* c, uint8_t kind, uint32_t w, uint32_t h, uint32_t fmt, bool dsv) {
    ID3D11Query* q = GpuProfQuery();
    if (!q) return;
    c->End(q);
    CtxScene* s = CtxSceneSlot(c);
    std::lock_guard<std::mutex> lk(g_gpMx);
    if (kind == 0) t_gpLastMark = (int)g_gpMarks.size();
    g_gpMarks.push_back({ q, w, h, fmt, (uint32_t)dsv, s ? (uint32_t)(s - g_ctxScene) : 99u, 0, kind });
}
static void GpuProfOnRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!g_gpCapturing.load(std::memory_order_relaxed)) return;
    ID3D11View* v = (n && rtvs && rtvs[0]) ? (ID3D11View*)rtvs[0] : (ID3D11View*)dsv;
    uint32_t w = 0, h = 0, fmt = 0;
    if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r);
        if (r) { ID3D11Texture2D* t = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); w = d.Width; h = d.Height; fmt = d.Format; t->Release(); } r->Release(); } }
    GpuProfMarkAt(c, 0, w, h, fmt, dsv != nullptr);
}
static void GpuProfOnDraw() {
    if (!g_gpCapturing.load(std::memory_order_relaxed) || t_gpLastMark < 0) return;
    std::lock_guard<std::mutex> lk(g_gpMx);
    if (t_gpLastMark < (int)g_gpMarks.size()) ++g_gpMarks[t_gpLastMark].draws;
}
static void GpuProfMirror(ID3D11DeviceContext* c, bool begin) {
    if (!g_gpCapturing.load(std::memory_order_relaxed)) return;
    GpuProfMarkAt(c, begin ? 1 : 2, 0, 0, 0, false);
}
// Present thread (immediate context): drive capture and readback.
static void GpuProfPresent(uint64_t f, bool liveWorld, uint32_t drawsLastWindow, bool diagKey) {
    if (!g_dev || !g_ctxImm) return;
    if (g_gpPool.empty()) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_TIMESTAMP, 0 };
        g_gpPool.resize(24576, nullptr);
        for (auto& q : g_gpPool) if (FAILED(g_dev->CreateQuery(&qd, &q))) q = nullptr;
        D3D11_QUERY_DESC dj{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }; g_dev->CreateQuery(&dj, &g_gpDisjoint);
        g_gpNextFrame = f + 1800;   // first capture ~15-30 s after load
    }
    if (diagKey && g_gpDiag == 0) { g_gpDiag = 1; Log("[gpuprof] Numpad .: A/B capture queued (A current, B old per-draw right-eye replay)"); }
    if (g_gpState == 1) {   // end of the captured frame
        g_gpCapturing.store(false);
        if (g_gpDiag == 3) { g_mirForceDirect.store(false); g_gpDiag = 0; }
        if (ID3D11Query* q = GpuProfQuery()) { g_ctxImm->End(q); std::lock_guard<std::mutex> lk(g_gpMx); g_gpMarks.push_back({ q, 0, 0, 0, 0, 0, 0, 0 }); }
        g_ctxImm->End(g_gpDisjoint);
        g_gpState = 2; g_gpWait = 0;
        return;
    }
    if (g_gpState == 2) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (g_ctxImm->GetData(g_gpDisjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { if (++g_gpWait < 60) return; Log("[gpuprof] results not ready, dropped"); g_gpState = 0; return; }
        std::vector<GpuProfMark> marks; { std::lock_guard<std::mutex> lk(g_gpMx); marks.swap(g_gpMarks); }
        struct T { uint64_t t; size_t i; }; std::vector<T> ts; ts.reserve(marks.size());
        for (size_t i = 0; i < marks.size(); ++i) { UINT64 v = 0; if (marks[i].q && g_ctxImm->GetData(marks[i].q, &v, sizeof(v), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && v) ts.push_back({ v, i }); }
        g_gpState = 0;
        if (dj.Disjoint || !dj.Frequency || ts.size() < 3) { Log("[gpuprof] capture invalid (disjoint=%d marks=%zu)", (int)dj.Disjoint, ts.size()); return; }
        std::sort(ts.begin(), ts.end(), [](const T& a, const T& b) { return a.t < b.t; });
        const double toMs = 1000.0 / (double)dj.Frequency;
        const double frameMs = (ts.back().t - ts.front().t) * toMs;
        // pass time: gap after each pass-start mark until the next pass-start mark (mirror marks are inside passes)
        struct Key { uint32_t w, h, fmt, dsv; bool operator<(const Key& o) const { return std::tie(w, h, fmt, dsv) < std::tie(o.w, o.h, o.fmt, o.dsv); } };
        // Command lists execute one after another, so on the sorted GPU timeline the pass in effect
        // is the latest pass-start mark. Gaps after a mirror-begin mark are that pass's right-eye
        // replay; every other gap is the game's own (left-eye) work in that pass.
        struct Cost { double left = 0, right = 0; uint32_t draws = 0; };
        std::map<Key, Cost> byTarget;
        Key cur{ 0, 0, 0, 0 };
        double leftMs = 0, rightMs = 0;
        for (size_t k = 0; k + 1 < ts.size(); ++k) {
            const GpuProfMark& m = marks[ts[k].i];
            const double gap = (ts[k + 1].t - ts[k].t) * toMs;
            if (m.kind == 0) { cur = { m.w, m.h, m.fmt, m.dsv }; byTarget[cur].draws += m.draws; }
            Cost& c = byTarget[cur];
            if (m.kind == 1) { c.right += gap; rightMs += gap; } else { c.left += gap; leftMs += gap; }
        }
        std::vector<std::pair<Key, Cost>> rows(byTarget.begin(), byTarget.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.left + a.second.right > b.second.left + b.second.right; });
        Log("[gpuprof] [%s] f%llu GPU frame %.2f ms over %zu marks: game/left-eye work %.2f ms, right-eye replay %.2f ms. Top targets (WxH fmt dsv : total ms = left + right, draws):",
            g_gpLabel, (unsigned long long)f, frameMs, ts.size(), leftMs, rightMs);
        int shown = 0;
        for (const auto& r : rows) { if (shown++ >= 20) break;
            const double total = r.second.left + r.second.right;
            Log("[gpuprof]   %ux%u fmt=%u dsv=%u : %.2f ms (%.0f%%) = %.2f + %.2f, %u draws", r.first.w, r.first.h, r.first.fmt, r.first.dsv,
                total, 100.0 * total / frameMs, r.second.left, r.second.right, r.second.draws); }
        return;
    }
    // idle: start a capture on a busy live-world frame, at most 3 per run, >= 20 s apart
    // after the first auto capture, the next two scheduled captures are the A/B pair (no key needed)
    if (g_gpDiag == 0 && g_gpCaptures == 1 && f >= g_gpNextFrame && liveWorld && drawsLastWindow > 1500) { g_gpDiag = 1; Log("[gpuprof] auto A/B capture (A current, B old per-draw right-eye replay)"); }
    const bool diagWant = g_gpDiag == 1 || g_gpDiag == 2;
    if (((g_gpCaptures < 3 && f >= g_gpNextFrame) || diagWant) && liveWorld && drawsLastWindow > 1500) {
        g_gpLabel = g_gpDiag == 1 ? "A current" : g_gpDiag == 2 ? "B per-draw replay" : "auto";
        if (g_gpDiag == 1) g_gpDiag = 2;
        else if (g_gpDiag == 2) { g_gpDiag = 3; g_mirForceDirect.store(true); }
        g_gpPoolNext.store(0); { std::lock_guard<std::mutex> lk(g_gpMx); g_gpMarks.clear(); }
        g_ctxImm->Begin(g_gpDisjoint);
        if (ID3D11Query* q = GpuProfQuery()) { g_ctxImm->End(q); std::lock_guard<std::mutex> lk(g_gpMx); g_gpMarks.push_back({ q, 0, 0, 0, 0, 0, 0, 0 }); }   // frame start (before the first target bind)
        g_gpCapturing.store(true); g_gpState = 1; ++g_gpCaptures; g_gpNextFrame = f + 2400;
        Log("[gpuprof] capturing frame %llu (%s)", (unsigned long long)f, g_gpLabel);
    }
}
