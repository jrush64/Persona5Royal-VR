// [BATCH] Perf run 9 A/B: binding the right-eye twin targets and back around EVERY mirrored draw
// (~2,000 target switches per pass) made the right-eye replay 3-8x the cost of the left eye's
// identical draws and slowed the left eye too (GPU frame 19.98 ms -> 10.07 ms without the switch).
// Batching: after the game's draw, the right-eye draw is RECORDED (its full pipeline state, with the
// right-eye inputs already resolved) and the pass's right-eye draws are replayed together with ONE
// target switch, when the game changes targets, finishes the command list, presents, or is about to
// touch something a recorded draw still depends on. Constant/vertex/index buffers the game rewrites
// while draws are pending are first snapshotted, so every replayed draw sees exactly the data its left-eye draw saw. Numpad + toggles.
// Run 10: snapshotting by GPU copy (~2,100 CopyResource per frame between the game's own draws)
// stalled the pipeline: left-eye scene pass 2.9 -> 15 ms, batched frame slower than per-draw. The
// mod already sees every constant-buffer write on the CPU, so a snapshot is now the last written
// bytes uploaded into a pooled DYNAMIC buffer with Map(DISCARD) - the game's own upload path, no GPU
// copy. The GPU copy remains only for buffers whose bytes were never seen (vertex/index buffers).
static void GpuProfMirror(ID3D11DeviceContext* c, bool begin);
static std::atomic<bool> g_mirBatch{true};
enum { BW_PASS, BW_TARGETS, BW_TEXWRITE, BW_BUFSRV, BW_NOPOOL, BW_COPY, BW_CLEAR, BW_DISPATCH, BW_END, BW_FULL, BW_DIRECT, BW_COUNT };
static const char* const kBatchWhy[BW_COUNT] = { "pass", "targets", "tex-write", "view-buf-write", "no-pool", "copy", "clear", "dispatch", "end", "full", "direct" };
static std::atomic<uint32_t> g_bWhy[BW_COUNT];
static std::atomic<uint32_t> g_bRecorded{0}, g_bReplayed{0}, g_bFlushes{0}, g_bSnaps{0}, g_bSnapGpu{0}, g_bDirect{0};
static constexpr UINT kBatchBufBinds = D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
static constexpr UINT kPoolDynamic = 0x80000000u;   // pool key flag: DYNAMIC (CPU-written) snapshot buffer

// ---- snapshot buffer pool: hooks only take/return; the render thread creates ----
static SRWLOCK g_bPoolLock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, std::vector<ID3D11Buffer*>> g_bPoolFree;
static std::unordered_map<uint64_t, uint32_t> g_bPoolWant;
static uint32_t g_bPoolCount = 0; static uint64_t g_bPoolBytes = 0;
static uint64_t BPoolKey(UINT bytes, UINT bind) { return ((uint64_t)bind << 32) | bytes; }
static ID3D11Buffer* BPoolTake(uint64_t key) {
    ID3D11Buffer* b = nullptr;
    AcquireSRWLockExclusive(&g_bPoolLock);
    auto it = g_bPoolFree.find(key);
    if (it != g_bPoolFree.end() && !it->second.empty()) { b = it->second.back(); it->second.pop_back(); }
    else ++g_bPoolWant[key];
    ReleaseSRWLockExclusive(&g_bPoolLock);
    return b;
}
static void BPoolGive(uint64_t key, ID3D11Buffer* b) {
    AcquireSRWLockExclusive(&g_bPoolLock); g_bPoolFree[key].push_back(b); ReleaseSRWLockExclusive(&g_bPoolLock);
}
static void BatchPoolBuild() {   // render thread, frame boundary, outside every hook
    std::unordered_map<uint64_t, uint32_t> want;
    AcquireSRWLockExclusive(&g_bPoolLock); want.swap(g_bPoolWant); ReleaseSRWLockExclusive(&g_bPoolLock);
    if (want.empty() || !g_dev) return;
    uint32_t madeNow = 0;
    for (const auto& w : want) {
        const UINT bytes = (UINT)(w.first & 0xffffffffu), bind = (UINT)(w.first >> 32);
        for (uint32_t i = 0; i < w.second && i < 256 && madeNow < 1024; ++i) {
            if (g_bPoolCount >= 16384 || g_bPoolBytes + bytes > (256ull << 20)) return;
            D3D11_BUFFER_DESC d{}; d.ByteWidth = bytes; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind & ~kPoolDynamic;
            if (bind & kPoolDynamic) { d.Usage = D3D11_USAGE_DYNAMIC; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; }
            ID3D11Buffer* b = nullptr;
            if (FAILED(g_dev->CreateBuffer(&d, nullptr, &b)) || !b) { Log("[batch] pool buffer %u bytes bind=0x%X create FAILED", bytes, bind); return; }
            BPoolGive(w.first, b); ++g_bPoolCount; g_bPoolBytes += bytes; ++madeNow;
        }
    }
}
static void BatchPoolReset() {
    AcquireSRWLockExclusive(&g_bPoolLock);
    for (auto& kv : g_bPoolFree) for (ID3D11Buffer* b : kv.second) b->Release();
    g_bPoolFree.clear(); g_bPoolWant.clear(); g_bPoolCount = 0; g_bPoolBytes = 0;
    ReleaseSRWLockExclusive(&g_bPoolLock);
}

// ---- one recorded right-eye draw: every COM pointer holds a reference until the flush ----
struct BRef { ID3D11Buffer* buf; uint64_t poolKey; };   // poolKey != 0: pooled snapshot, returned not released
static constexpr uint16_t kNoRef = 0xFFFF;
struct BDraw {
    uint8_t kind, nVB, nVp, nSc; bool cb1;
    UINT a, b, d, e; INT base;
    ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo;
    uint16_t vb[8]; UINT stride[8], off[8];
    uint16_t ib; DXGI_FORMAT ibFmt; UINT ibOff;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps;
    uint16_t vcb[14], pcb[14]; UINT vFirst[14], vNum[14], pFirst[14], pNum[14];
    ID3D11ShaderResourceView* vsrv[16]; ID3D11ShaderResourceView* psrv[32];
    ID3D11SamplerState* vsmp[16]; ID3D11SamplerState* psmp[16];
    ID3D11RasterizerState* rs; D3D11_VIEWPORT vp[4]; D3D11_RECT sc[4];
    ID3D11BlendState* bs; FLOAT bf[4]; UINT sm; ID3D11DepthStencilState* dss; UINT sref;
};
struct BCall { uint8_t kind; UINT a, b, d, e; INT base; };   // kind 0 DrawIndexed(a,b,base) 1 Draw(a,b) 2 DrawIndexedInstanced(a,b,d,base,e) 3 DrawInstanced(a,b,d,e)
struct Batch {
    ID3D11DeviceContext* ctx = nullptr; ID3D11DeviceContext1* c1 = nullptr; bool c1Tried = false;
    UINT nRtv = 0; ID3D11RenderTargetView* trtv[8] = {}; ID3D11DepthStencilView* tdsv = nullptr;
    std::vector<BDraw> draws; std::vector<BRef> refs; std::unordered_map<ID3D11Buffer*, uint16_t> live;
    std::unordered_map<ID3D11Buffer*, std::vector<uint8_t>> bytes;   // last full contents written on this context (CBs)
    std::unordered_set<ID3D11Buffer*> used;   // [CPUFIX] buffers a recorded right-eye draw has read: only these are remembered
};
static Batch g_batch[16];
static thread_local bool t_bBusy = false;   // inside a flush or one of our own snapshot copies
static Batch* BatchOf(ID3D11DeviceContext* c) {
    if (!c) return nullptr;
    if (c == g_ctxImm) { if (GetCurrentThreadId() != g_presentTid.load(std::memory_order_relaxed)) return nullptr; }
    else if (!IsDeferredContext(c)) return nullptr;
    CtxScene* s = CtxSceneSlot(c);
    return s ? &g_batch[s - g_ctxScene] : nullptr;
}
static uint16_t BRefOf(Batch& B, ID3D11Buffer* b) {
    if (!b) return kNoRef;
    auto it = B.live.find(b);
    if (it != B.live.end()) return it->second;
    b->AddRef(); B.refs.push_back({ b, 0 });
    if (B.used.size() > 65536) B.used.clear();
    B.used.insert(b);
    const uint16_t i = (uint16_t)(B.refs.size() - 1); B.live.emplace(b, i); return i;
}
static ID3D11Buffer* BRefBuf(const Batch& B, uint16_t i) { return i == kNoRef ? nullptr : B.refs[i].buf; }
template <class T> static void BRel(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void BDrawRelease(BDraw& d) {
    BRel(d.il); BRel(d.vs); BRel(d.ps); BRel(d.rs); BRel(d.bs); BRel(d.dss);
    for (auto& v : d.vsrv) BRel(v); for (auto& v : d.psrv) BRel(v);
    for (auto& s : d.vsmp) BRel(s); for (auto& s : d.psmp) BRel(s);
}
static void BatchRelease(Batch& B) {
    for (auto& d : B.draws) BDrawRelease(d);
    for (auto& r : B.refs) { if (r.poolKey) BPoolGive(r.poolKey, r.buf); else r.buf->Release(); }
    for (auto& v : B.trtv) BRel(v); BRel(B.tdsv);
    B.draws.clear(); B.refs.clear(); B.live.clear(); B.nRtv = 0;
}
static void BCopyRes(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src);   // after the copy hooks' trampolines
static void BUnmap(ID3D11DeviceContext* c, ID3D11Resource* r);                            // after the Unmap trampolines
// Bind the slot to `c` (a recycled address never inherits another context's draws or bytes).
static void BatchAdopt(Batch& B, ID3D11DeviceContext* c) {
    if (B.ctx == c) return;
    BatchRelease(B); B.bytes.clear(); B.used.clear(); B.ctx = c; B.c1 = nullptr; B.c1Tried = false;
}
// The full contents just written to `buf` on `c` (only constant buffers; caller passes ByteWidth).
static void BatchNote(ID3D11DeviceContext* c, ID3D11Resource* buf, const void* data, UINT size) {
    if (!buf || !data || !size || size > 16384 || !g_mirBatch.load(std::memory_order_relaxed)) return;
    Batch* B = BatchOf(c);
    if (!B) return;
    BatchAdopt(*B, c);
    if (!B->used.count((ID3D11Buffer*)buf)) return;   // never read by a right-eye draw: nothing to preserve
    auto& v = B->bytes[(ID3D11Buffer*)buf]; v.assign((const uint8_t*)data, (const uint8_t*)data + size);
}
// Contents may have changed outside this context's hooks (another list ran, or a new list began).
static void BatchForgetAll(ID3D11DeviceContext* c) { Batch* B = BatchOf(c); if (B && B->ctx == c) B->bytes.clear(); }

// ---- full save/restore of the state a replay touches ----
struct BSaved {
    ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo; ID3D11Buffer* vb[8]; UINT st[8], of[8];
    ID3D11Buffer* ib; DXGI_FORMAT ibf; UINT ibo;
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds;
    ID3D11Buffer* vcb[14]; ID3D11Buffer* pcb[14]; UINT vf[14], vn[14], pf[14], pn[14];
    ID3D11ShaderResourceView* vsrv[16]; ID3D11ShaderResourceView* psrv[32];
    ID3D11SamplerState* vsmp[16]; ID3D11SamplerState* psmp[16];
    ID3D11RasterizerState* rs; D3D11_VIEWPORT vp[16]; UINT nVp; D3D11_RECT sc[16]; UINT nSc;
    ID3D11BlendState* bs; FLOAT bf[4]; UINT sm; ID3D11DepthStencilState* dss; UINT sref;
    ID3D11RenderTargetView* rtv[8]; ID3D11DepthStencilView* dsv; ID3D11UnorderedAccessView* uav[8];
};
static bool BAnyFirst(ID3D11Buffer* const* b, const UINT* first) { for (int i = 0; i < 14; ++i) if (b[i] && first[i]) return true; return false; }
static void BSetCBs(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, bool pixel, ID3D11Buffer* const* b, const UINT* first, const UINT* num, bool offsets) {
    if (offsets && c1) { if (pixel) c1->PSSetConstantBuffers1(0, 14, b, first, num); else c1->VSSetConstantBuffers1(0, 14, b, first, num); }
    else if (pixel) OrigPSSetCB(c)(c, 0, 14, b);
    else OrigVSSetCB(c)(c, 0, 14, b);
}
static void BGetCBs(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, bool pixel, ID3D11Buffer** b, UINT* first, UINT* num) {
    memset(first, 0, 14 * sizeof(UINT)); memset(num, 0, 14 * sizeof(UINT));
    if (c1) { if (pixel) c1->PSGetConstantBuffers1(0, 14, b, first, num); else c1->VSGetConstantBuffers1(0, 14, b, first, num); }
    else if (pixel) c->PSGetConstantBuffers(0, 14, b);
    else c->VSGetConstantBuffers(0, 14, b);
}
static void BSave(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, BSaved& s) {
    memset(&s, 0, sizeof(s));
    c->IAGetInputLayout(&s.il); c->IAGetPrimitiveTopology(&s.topo); c->IAGetVertexBuffers(0, 8, s.vb, s.st, s.of);
    c->IAGetIndexBuffer(&s.ib, &s.ibf, &s.ibo);
    c->VSGetShader(&s.vs, nullptr, nullptr); c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr); c->HSGetShader(&s.hs, nullptr, nullptr); c->DSGetShader(&s.ds, nullptr, nullptr);
    BGetCBs(c, c1, false, s.vcb, s.vf, s.vn); BGetCBs(c, c1, true, s.pcb, s.pf, s.pn);
    c->VSGetShaderResources(0, 16, s.vsrv); c->PSGetShaderResources(0, 32, s.psrv);
    c->VSGetSamplers(0, 16, s.vsmp); c->PSGetSamplers(0, 16, s.psmp);
    c->RSGetState(&s.rs); s.nVp = 16; c->RSGetViewports(&s.nVp, s.vp); s.nSc = 16; c->RSGetScissorRects(&s.nSc, s.sc);
    c->OMGetBlendState(&s.bs, s.bf, &s.sm); c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->OMGetRenderTargetsAndUnorderedAccessViews(8, s.rtv, &s.dsv, 0, 8, s.uav);
}
static void BRestore(ID3D11DeviceContext* c, ID3D11DeviceContext1* c1, BSaved& s) {
    c->IASetInputLayout(s.il); c->IASetPrimitiveTopology(s.topo); c->IASetVertexBuffers(0, 8, s.vb, s.st, s.of);
    c->IASetIndexBuffer(s.ib, s.ibf, s.ibo);
    c->VSSetShader(s.vs, nullptr, 0); c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0); c->HSSetShader(s.hs, nullptr, 0); c->DSSetShader(s.ds, nullptr, 0);
    BSetCBs(c, c1, false, s.vcb, s.vf, s.vn, BAnyFirst(s.vcb, s.vf)); BSetCBs(c, c1, true, s.pcb, s.pf, s.pn, BAnyFirst(s.pcb, s.pf));
    c->VSSetShaderResources(0, 16, s.vsrv); c->PSSetShaderResources(0, 32, s.psrv);
    c->VSSetSamplers(0, 16, s.vsmp); c->PSSetSamplers(0, 16, s.psmp);
    c->RSSetState(s.rs); c->RSSetViewports(s.nVp, s.vp); c->RSSetScissorRects(s.nSc, s.sc);
    c->OMSetBlendState(s.bs, s.bf, s.sm); c->OMSetDepthStencilState(s.dss, s.sref);
    UINT nRtv = 0; for (UINT i = 0; i < 8; ++i) if (s.rtv[i]) nRtv = i + 1;
    bool anyUav = false; for (UINT i = nRtv; i < 8; ++i) if (s.uav[i]) anyUav = true;
    if (anyUav) { UINT keep[8]; for (auto& k : keep) k = (UINT)-1;
        c->OMSetRenderTargetsAndUnorderedAccessViews(nRtv, s.rtv, s.dsv, nRtv, 8 - nRtv, s.uav + nRtv, keep); }
    else OrigOMSetRTs(c)(c, nRtv, s.rtv, s.dsv);
    BRel(s.il); for (auto& b : s.vb) BRel(b); BRel(s.ib); BRel(s.vs); BRel(s.ps); BRel(s.gs); BRel(s.hs); BRel(s.ds);
    for (auto& b : s.vcb) BRel(b); for (auto& b : s.pcb) BRel(b);
    for (auto& v : s.vsrv) BRel(v); for (auto& v : s.psrv) BRel(v); for (auto& v : s.vsmp) BRel(v); for (auto& v : s.psmp) BRel(v);
    BRel(s.rs); BRel(s.bs); BRel(s.dss); for (auto& v : s.rtv) BRel(v); BRel(s.dsv); for (auto& v : s.uav) BRel(v);
}

// ---- replay the pending right-eye draws of `c` (one target switch), then restore the game's state ----
static void BatchFlush(ID3D11DeviceContext* c, int why) {
    if (t_bBusy) return;
    Batch* B = BatchOf(c);
    if (!B || B->draws.empty()) return;
    PerfPart perfFlush(g_perfFlush);
    if (B->ctx != c) { BatchRelease(*B); return; }   // context address recycled: never replay into another list
    t_bBusy = true;
    GpuProfMirror(c, true);
    static thread_local BSaved S; BSave(c, B->c1, S);
    OrigOMSetRTs(c)(c, B->nRtv, B->trtv, B->tdsv);
    c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    const BDraw* p = nullptr;
    ID3D11Buffer *pvb[8] = {}, *pib = nullptr, *pv[14] = {}, *pp[14] = {};
    for (const BDraw& d : B->draws) {
        ID3D11Buffer *vb[8], *ib = BRefBuf(*B, d.ib), *v[14], *pc[14];
        for (int i = 0; i < 8; ++i) vb[i] = BRefBuf(*B, d.vb[i]);
        for (int i = 0; i < 14; ++i) { v[i] = BRefBuf(*B, d.vcb[i]); pc[i] = BRefBuf(*B, d.pcb[i]); }
        if (!p || p->il != d.il) c->IASetInputLayout(d.il);
        if (!p || p->topo != d.topo) c->IASetPrimitiveTopology(d.topo);
        if (!p || memcmp(pvb, vb, sizeof(vb)) || memcmp(p->stride, d.stride, sizeof(d.stride)) || memcmp(p->off, d.off, sizeof(d.off)))
            c->IASetVertexBuffers(0, 8, vb, d.stride, d.off);
        if (!p || pib != ib || p->ibFmt != d.ibFmt || p->ibOff != d.ibOff) c->IASetIndexBuffer(ib, d.ibFmt, d.ibOff);
        if (!p || p->vs != d.vs) c->VSSetShader(d.vs, nullptr, 0);
        if (!p || p->ps != d.ps) c->PSSetShader(d.ps, nullptr, 0);
        if (!p || memcmp(pv, v, sizeof(v)) || d.cb1 || p->cb1) BSetCBs(c, B->c1, false, v, d.vFirst, d.vNum, d.cb1);
        if (!p || memcmp(pp, pc, sizeof(pc)) || d.cb1 || p->cb1) BSetCBs(c, B->c1, true, pc, d.pFirst, d.pNum, d.cb1);
        if (!p || memcmp(p->vsrv, d.vsrv, sizeof(d.vsrv))) c->VSSetShaderResources(0, 16, d.vsrv);
        if (!p || memcmp(p->psrv, d.psrv, sizeof(d.psrv))) c->PSSetShaderResources(0, 32, d.psrv);
        if (!p || memcmp(p->vsmp, d.vsmp, sizeof(d.vsmp))) c->VSSetSamplers(0, 16, d.vsmp);
        if (!p || memcmp(p->psmp, d.psmp, sizeof(d.psmp))) c->PSSetSamplers(0, 16, d.psmp);
        if (!p || p->rs != d.rs) c->RSSetState(d.rs);
        if (!p || p->nVp != d.nVp || memcmp(p->vp, d.vp, d.nVp * sizeof(D3D11_VIEWPORT))) c->RSSetViewports(d.nVp, d.vp);
        if (!p || p->nSc != d.nSc || memcmp(p->sc, d.sc, d.nSc * sizeof(D3D11_RECT))) c->RSSetScissorRects(d.nSc, d.sc);
        if (!p || p->bs != d.bs || memcmp(p->bf, d.bf, sizeof(d.bf)) || p->sm != d.sm) c->OMSetBlendState(d.bs, d.bf, d.sm);
        if (!p || p->dss != d.dss || p->sref != d.sref) c->OMSetDepthStencilState(d.dss, d.sref);
        HeadGpuBeforeDraw(c, 1);
        switch (d.kind) {
            case 0: OrigDrawIndexed(c)(c, d.a, d.b, d.base); break;
            case 1: OrigDraw(c)(c, d.a, d.b); break;
            case 2: OrigDrawIdxInst(c)(c, d.a, d.b, d.d, d.base, d.e); break;
            default: OrigDrawInst(c)(c, d.a, d.b, d.d, d.e); break;
        }
        memcpy(pvb, vb, sizeof(vb)); pib = ib; memcpy(pv, v, sizeof(v)); memcpy(pp, pc, sizeof(pc)); p = &d;
    }
    BRestore(c, B->c1, S);
    GpuProfMirror(c, false);
    g_bReplayed.fetch_add((uint32_t)B->draws.size(), std::memory_order_relaxed);
    g_bFlushes.fetch_add(1, std::memory_order_relaxed);
    if (why >= 0 && why < BW_COUNT) g_bWhy[why].fetch_add(1, std::memory_order_relaxed);
    BatchRelease(*B);
    t_bBusy = false;
}
static void BatchFlushEnd(ID3D11DeviceContext* c) { BatchFlush(c, BW_END); }

// ---- record the right-eye twin of the draw the game just issued (replaces the per-draw replay) ----
static bool BatchRecord(ID3D11DeviceContext* c, MirDraw& m, const BCall& call) {
    PerfPart pp(g_perfRecord);
    if (!g_mirBatch.load(std::memory_order_relaxed) || g_mirForceDirect.load(std::memory_order_relaxed) || m.anyUav || t_bBusy) return false;
    Batch* B = BatchOf(c);
    if (!B) return false;
    BatchAdopt(*B, c);
    {   // geometry/tessellation stages are not recorded: those draws keep the per-draw path
        ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
        c->GSGetShader(&gs, nullptr, nullptr); c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
        const bool extra = gs || hs || ds; BRel(gs); BRel(hs); BRel(ds);
        if (extra) return false;
    }
    D3D11_VIEWPORT vps[16]; UINT nVp = 16; c->RSGetViewports(&nVp, vps);
    D3D11_RECT scs[16]; UINT nSc = 16; c->RSGetScissorRects(&nSc, scs);
    if (nVp > 4 || nSc > 4) return false;
    if (B->draws.empty() && nVp && m.trtv[0]) {   // [FRAMING] which rectangle of which target the eye passes draw into (first 24 distinct)
        static std::mutex s_mx; static std::vector<std::array<int, 6>> s_seen;
        ID3D11Resource* r = nullptr; m.rtv[0] ? m.rtv[0]->GetResource(&r) : (void)0;
        if (r) { D3D11_TEXTURE2D_DESC td{}; ((ID3D11Texture2D*)r)->GetDesc(&td); r->Release();
            const std::array<int, 6> k{ (int)td.Width, (int)td.Height, (int)vps[0].TopLeftX, (int)vps[0].TopLeftY, (int)vps[0].Width, (int)vps[0].Height };
            std::lock_guard<std::mutex> lk(s_mx);
            if (s_seen.size() < 24 && std::find(s_seen.begin(), s_seen.end(), k) == s_seen.end()) {
                s_seen.push_back(k);
                Log("[framing] eye pass target %dx%d fmt=%u viewport x=%d y=%d w=%d h=%d", k[0], k[1], (unsigned)td.Format, k[2], k[3], k[4], k[5]);
            } }
    }
    if (!B->draws.empty() && (B->nRtv != (UINT)m.nRtv || memcmp(B->trtv, m.trtv, sizeof(B->trtv)) || B->tdsv != m.tdsv)) BatchFlush(c, BW_TARGETS);
    if (B->draws.size() >= 2048 || B->refs.size() >= 60000) BatchFlush(c, BW_FULL);
    if (!B->c1Tried) {
        B->c1Tried = true; ID3D11DeviceContext1* c1 = nullptr;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1)) && c1) { B->c1 = c1; c1->Release(); }   // same object as c
    }
    if (B->draws.empty()) {
        B->nRtv = (UINT)m.nRtv;
        for (int i = 0; i < 8; ++i) { B->trtv[i] = m.trtv[i]; if (B->trtv[i]) B->trtv[i]->AddRef(); }
        B->tdsv = m.tdsv; if (B->tdsv) B->tdsv->AddRef();
    }
    B->draws.emplace_back(); BDraw& d = B->draws.back(); memset(&d, 0, sizeof(d));
    d.kind = call.kind; d.a = call.a; d.b = call.b; d.d = call.d; d.e = call.e; d.base = call.base;
    c->IAGetInputLayout(&d.il); c->IAGetPrimitiveTopology(&d.topo);
    {
        ID3D11Buffer* vb[8] = {}; c->IAGetVertexBuffers(0, 8, vb, d.stride, d.off);
        for (int i = 0; i < 8; ++i) { d.vb[i] = BRefOf(*B, vb[i]); if (vb[i]) { vb[i]->Release(); d.nVB = (uint8_t)(i + 1); } }
        ID3D11Buffer* ib = nullptr; c->IAGetIndexBuffer(&ib, &d.ibFmt, &d.ibOff); d.ib = BRefOf(*B, ib); BRel(ib);
    }
    c->VSGetShader(&d.vs, nullptr, nullptr); c->PSGetShader(&d.ps, nullptr, nullptr);
    {   // right-eye constant buffers (camera twins) exactly as the per-draw path binds them
        ID3D11Buffer *vcb[14] = {}, *pcb[14] = {}, *vT[14], *pT[14];
        BGetCBs(c, B->c1, false, vcb, d.vFirst, d.vNum); BGetCBs(c, B->c1, true, pcb, d.pFirst, d.pNum);
        memcpy(vT, vcb, sizeof(vcb)); memcpy(pT, pcb, sizeof(pcb));
        bool vcCh = false, pcCh = false; MirSwapCBs(vT, 14, vcCh); MirSwapCBs(pT, 14, pcCh);
        if (vcCh || pcCh) ++g_mirCamSwaps;
        d.cb1 = BAnyFirst(vcb, d.vFirst) || BAnyFirst(pcb, d.pFirst);
        for (int i = 0; i < 14; ++i) { d.vcb[i] = BRefOf(*B, vT[i]); d.pcb[i] = BRefOf(*B, pT[i]); BRel(vcb[i]); BRel(pcb[i]); }
    }
    {   // right-eye inputs: twin views replace the originals (the twins are borrowed: take our own ref)
        ID3D11ShaderResourceView *vs[16] = {}, *ps[32] = {};
        c->VSGetShaderResources(0, 16, vs); c->PSGetShaderResources(0, 32, ps);
        MirSwapSRVs(vs, d.vsrv, 16); MirSwapSRVs(ps, d.psrv, 32);
        for (int i = 0; i < 16; ++i) if (d.vsrv[i] != vs[i]) { d.vsrv[i]->AddRef(); vs[i]->Release(); }
        for (int i = 0; i < 32; ++i) if (d.psrv[i] != ps[i]) { d.psrv[i]->AddRef(); ps[i]->Release(); }
    }
    c->VSGetSamplers(0, 16, d.vsmp); c->PSGetSamplers(0, 16, d.psmp);
    c->RSGetState(&d.rs); d.nVp = (uint8_t)nVp; memcpy(d.vp, vps, nVp * sizeof(D3D11_VIEWPORT)); d.nSc = (uint8_t)nSc; memcpy(d.sc, scs, nSc * sizeof(D3D11_RECT));
    c->OMGetBlendState(&d.bs, d.bf, &d.sm); c->OMGetDepthStencilState(&d.dss, &d.sref);
    ++g_mirDraws; g_bRecorded.fetch_add(1, std::memory_order_relaxed);
    MirRelease(m);
    t_mirCtx = nullptr; t_mirStage = "done"; MirLeave();
    return true;
}
// The draw hooks' mirror step: record into the batch, or (UAVs, GS/tessellation, batching off)
// replay pending draws first and then this draw the old per-draw way, keeping the order.
template <class F> static void MirrorDraw(ID3D11DeviceContext* c, MirDraw& m, const BCall& call, F draw) {
    if (BatchRecord(c, m, call)) return;
    if (g_mirBatch.load(std::memory_order_relaxed)) g_bDirect.fetch_add(1, std::memory_order_relaxed);
    BatchFlush(c, BW_DIRECT);
    MirrorPostDraw(c, m, draw);
}

// ---- hazards: anything about to change what a pending draw reads or what its targets hold ----
static bool BSnapshot(ID3D11DeviceContext* c, Batch& B, ID3D11Buffer* b) {
    auto it = B.live.find(b);
    if (it == B.live.end()) return true;            // no pending draw reads it
    PerfPart perfSnap(g_perfSnap);
    D3D11_BUFFER_DESC d{}; b->GetDesc(&d);
    const UINT bind = d.BindFlags & kBatchBufBinds;
    if (!bind || (d.BindFlags & ~kBatchBufBinds) || d.MiscFlags) return false;
    uint64_t key; ID3D11Buffer* copy = nullptr;
    auto known = B.bytes.find(b);
    if (bind == D3D11_BIND_CONSTANT_BUFFER && known != B.bytes.end() && known->second.size() == d.ByteWidth) {
        // the bytes the pending draws read are known: upload them like the game does (no GPU copy)
        key = BPoolKey(d.ByteWidth, bind | kPoolDynamic);
        copy = BPoolTake(key);
        if (!copy) return false;                    // pool grows on the render thread; replay now instead
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(OrigMap(c)(c, copy, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) || !m.pData) { BPoolGive(key, copy); return false; }
        memcpy(m.pData, known->second.data(), d.ByteWidth); BUnmap(c, copy);
    } else {
        key = BPoolKey(d.ByteWidth, bind);
        copy = BPoolTake(key);
        if (!copy) return false;
        t_bBusy = true; BCopyRes(c, copy, b); t_bBusy = false;
        g_bSnapGpu.fetch_add(1, std::memory_order_relaxed);
    }
    BRef& r = B.refs[it->second]; r.buf->Release(); r.buf = copy; r.poolKey = key;
    B.live.erase(it);                               // later draws bind the new contents under a new slot
    g_bSnaps.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Before `r` is written on `c`: pending draws that read it get its current contents preserved, and
// its remembered bytes (and its right-eye twin's, rewritten right after) are dropped until noted again.
static void BatchHazard(ID3D11DeviceContext* c, ID3D11Resource* r, int texWhy) {
    if (!r || t_bBusy) return;
    Batch* B = BatchOf(c);
    if (!B || B->ctx != c || (B->draws.empty() && B->bytes.empty())) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) { BatchFlush(c, texWhy); return; }
    ID3D11Buffer* b = (ID3D11Buffer*)r;
    ID3D11Buffer* tw = CamBufRightIfCam(b);
    if (!B->draws.empty()) {
        D3D11_BUFFER_DESC d{}; b->GetDesc(&d);
        if (d.BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_STREAM_OUTPUT)) BatchFlush(c, BW_BUFSRV);   // may be read through a view
        else if (!BSnapshot(c, *B, b) || (tw && !BSnapshot(c, *B, tw))) BatchFlush(c, BW_NOPOOL);
    }
    B->bytes.erase(b); if (tw) B->bytes.erase(tw);
}
static void BatchHazardCopy(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (t_bBusy) return;
    Batch* B = BatchOf(c);
    if (!B || B->draws.empty()) return;
    D3D11_RESOURCE_DIMENSION ds = D3D11_RESOURCE_DIMENSION_UNKNOWN, ss = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (dst) dst->GetType(&ds); if (src) src->GetType(&ss);
    if (ds != D3D11_RESOURCE_DIMENSION_BUFFER || (src && ss != D3D11_RESOURCE_DIMENSION_BUFFER)) { BatchFlush(c, BW_COPY); return; }
    BatchHazard(c, dst, BW_COPY);
}
static void BatchLogStats(uint64_t f) {
    char why[256]; int n = 0;
    for (int i = 0; i < BW_COUNT; ++i) { uint32_t v = g_bWhy[i].exchange(0); if (v && n < 230) n += snprintf(why + n, sizeof(why) - n, " %s=%u", kBatchWhy[i], v); }
    why[n] = 0;
    const uint32_t rec = g_bRecorded.exchange(0), rep = g_bReplayed.exchange(0), fl = g_bFlushes.exchange(0);
    uint32_t pool; uint64_t bytes; AcquireSRWLockShared(&g_bPoolLock); pool = g_bPoolCount; bytes = g_bPoolBytes; ReleaseSRWLockShared(&g_bPoolLock);
    Log("[batch] f%llu 120f: %s | recorded=%u replayed=%u flushes=%u (%.1f draws per target switch) snapshots=%u (gpu copies %u) per-draw=%u pool=%u buffers %.1f MB | flush reasons:%s",
        (unsigned long long)f, g_mirBatch.load() ? "BATCHED" : "per-draw (Numpad + off)", rec, rep, fl, fl ? (double)rep / fl : 0.0,
        g_bSnaps.exchange(0), g_bSnapGpu.exchange(0), g_bDirect.exchange(0), pool, bytes / 1048576.0, n ? why : " none");
}
