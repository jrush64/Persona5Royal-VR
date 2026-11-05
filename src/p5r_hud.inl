// [HUD] P5R's HUD (date, objectives, minimap, prompts, speech bubbles) is drawn into the final
// colour target over the full eye image, so with the headset fill it sits in the lens edges. HUD
// draws are pulled toward the image centre by scaling their viewport (and scissor) about the target
// centre. The mirror replays each draw with the same context state, so both eyes match.
// Classification: indexed draw into a backbuffer-sized target, no depth buffer, main camera not
// bound. [hudscan] logs the per-context draw structure of one frame every 600 frames (plus a
// quarter-res BMP of the final image) so the classification can be checked from evidence.
struct HudEvt { uint8_t ctx, indexed, dsv, cam, shrunk; uint32_t count, w, h, fmt; };
static std::atomic<bool> g_hudScan{false};
static std::mutex g_hudMx;
static std::vector<HudEvt> g_hudEvts;
static std::atomic<float> g_hudScale{1.0f};   // run 2: OFF — viewport shrink hit the scene composite, not the HUD
static std::atomic<uint32_t> g_hudShrunk{0};
struct HudState { bool on = false; bool redirect = false; bool skip = false; ID3D11BlendState* blend = nullptr; FLOAT blendFactor[4] = {}; UINT sampleMask = 0; bool blendSwapped = false; uint32_t srvCount = 0; uint8_t srvSlot[4] = {}; ID3D11ShaderResourceView* srv[4] = {}; ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr; ID3D11DepthStencilState* dss = nullptr; UINT stencilRef = 0; bool dssSwapped = false; uint8_t cov = 0; void* pxT0 = nullptr; void* pxVs = nullptr; uint32_t pxVerts = 0; uint8_t pxBlend = 0; uint32_t pxTw = 0, pxTh = 0; bool barCand = false; UINT nvp = 0, nsc = 0; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]; };

// [UISEQ] immediate-context event sequence for one frame: render target, blend, PS input, and a
// GPU snapshot of the backbuffer each time the draw "kind" changes, so the HUD pass and the scene
// composite can be told apart from evidence (order, blending and what each pass samples).
static void HudDumpBmp(ID3D11Texture2D* tex, const char* name);
static ID3D11BlendState* UiCoverageBlend(ID3D11BlendState* from);
// [UICOVER] 2026-10-06: menu panels/solid UI pieces are drawn with blending OFF and P5R's shaders
// output alpha 0 there (the flat game never uses alpha), so on the panel they had zero opacity:
// whole rectangles vanished (top-right banner, "Thieves Den" box) and the IM phone was see-through.
// A blend state cannot force alpha 1 when the shader writes 0, so coverage is marked in a stencil
// buffer the HUD target owns: every opaque UI draw redirected with no depth buffer of its own gets
// our stencil bound and writes 1 there; before the panel is submitted, one full-screen pass sets
// alpha = 1 wherever stencil == 1. Blended UI keeps its own coverage alpha ([UIALPHA]).
static ID3D11Texture2D* g_hudDepthTex = nullptr;
static std::atomic<ID3D11DepthStencilView*> g_hudDSV{nullptr};
static ID3D11DepthStencilState* g_dssMark = nullptr;   // stencil always -> replace 1, no depth
static ID3D11DepthStencilState* g_dssTest = nullptr;   // stencil == 1, no writes
static ID3D11BlendState* g_blendAlphaOnly = nullptr;
static ID3D11RasterizerState* g_rsNoCull = nullptr;
static ID3D11VertexShader* g_fixVS = nullptr;
static ID3D11PixelShader* g_fixPS = nullptr;
// [KEYCOVER] 2026-10-07 run: the IM dump shows the game's COLOUR is right but its alpha is scratch: P5R
// draws masks into alpha (colour-off ZERO/ONE draws, MIN/SUBTRACT alpha ops) and then paints colour
// through them with DEST_ALPHA. On the panel that scratch alpha became the opacity: IM text and the top
// of the red phone see-through, rectangles vanishing, a black blob left behind after closing.
// Coverage now comes from colour instead: the panel is cleared to a key colour each frame; any pixel
// whose colour is no longer exactly the key was drawn. Undrawn -> fully transparent; drawn -> opaque
// in menus, the game's own alpha on the field (minimap etc. keep their translucency).
// alpha 1: the game reads dest alpha (DEST_ALPHA blends: map Joker portrait, map roads) and its own
// scene image starts opaque - our panel must start the same or those draws come out dark/missing
static constexpr float kHudKey[4] = { 3.f / 255.f, 5.f / 255.f, 4.f / 255.f, 1.f };
static ID3D11PixelShader* g_keyPS[2] = {};            // [0] field: keep alpha, [1] menu: alpha 1
static ID3D11Texture2D* g_hudCopy = nullptr;
static ID3D11ShaderResourceView* g_hudCopySRV = nullptr;
// [COVREPLAY] 2026-10-07: the objectives box (top right) was a solid dark rectangle. Cause: [UIALPHA]
// rewrote the alpha blend of "over" draws, and P5R uses alpha as a STENCIL (brush-stroke shapes in
// alpha, then the dark fill painted through it with DEST_ALPHA) - with the mask rewritten the fill
// covered the whole rectangle. Now every game draw keeps the game's own blend (colour = the flat
// game exactly); on the field, each UI draw is drawn a second time into a private coverage target
// that records only how opaque it is. Menus need no replay: drawn = opaque ([KEYCOVER]).
enum : uint8_t { CovNone = 0, CovOver = 1, CovSolid = 2, CovMax = 3 };
static ID3D11Texture2D* g_covTex = nullptr;
static std::atomic<ID3D11RenderTargetView*> g_covRTV{nullptr};
static ID3D11ShaderResourceView* g_covSRV = nullptr;
static ID3D11BlendState* g_covBlend[4] = {};
static ID3D11DepthStencilState* g_dssOff = nullptr;
static std::atomic<uint32_t> g_covReplays{0};
struct CovGroup { uint32_t key[9]; uint32_t n; };
static std::mutex g_covGrpMx; static CovGroup g_covGrp[40]; static int g_covGrpN = 0;
static uint8_t CovKindFor(ID3D11DeviceContext* c);
static void CovCensus(ID3D11DeviceContext* c, uint8_t kind) {
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm);
    D3D11_BLEND_DESC d{}; if (bs) { bs->GetDesc(&d); bs->Release(); }
    const auto& r = d.RenderTarget[0];
    const uint32_t k[9] = { (uint32_t)r.BlendEnable, (uint32_t)r.SrcBlend, (uint32_t)r.DestBlend, (uint32_t)r.BlendOp, (uint32_t)r.SrcBlendAlpha, (uint32_t)r.DestBlendAlpha, (uint32_t)r.BlendOpAlpha, (uint32_t)r.RenderTargetWriteMask, (uint32_t)kind };
    std::lock_guard<std::mutex> lk(g_covGrpMx);
    for (int i = 0; i < g_covGrpN; ++i) if (!memcmp(g_covGrp[i].key, k, sizeof(k))) { ++g_covGrp[i].n; return; }
    if (g_covGrpN < 40) { memcpy(g_covGrp[g_covGrpN].key, k, sizeof(k)); g_covGrp[g_covGrpN++].n = 1; }
}
static uint8_t CovKindFor(ID3D11DeviceContext* c) {
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm);
    D3D11_BLEND_DESC d{}; if (bs) { bs->GetDesc(&d); bs->Release(); } else return CovSolid;
    const auto& r = d.RenderTarget[0];
    if (!(r.RenderTargetWriteMask & 7)) return CovNone;                               // alpha-only mask draw
    if (!r.BlendEnable) return CovSolid;
    if (r.SrcBlend == D3D11_BLEND_ZERO && r.DestBlend == D3D11_BLEND_ONE) return CovNone;   // colour untouched
    auto da = [](D3D11_BLEND b) { return b == D3D11_BLEND_DEST_ALPHA || b == D3D11_BLEND_INV_DEST_ALPHA; };
    if (da(r.SrcBlend) || da(r.DestBlend)) return CovSolid;                          // fill through the game's mask
    if (r.DestBlend == D3D11_BLEND_INV_SRC_ALPHA && r.BlendOp == D3D11_BLEND_OP_ADD) return CovOver;
    return CovMax;
}
struct CovSave { ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr; ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT sm = 0; ID3D11DepthStencilState* dss = nullptr; UINT ref = 0; };
// [TWINKEY] 10-11: exact panel alpha. The twin target starts WHITE (the panel starts at the near-black
// key) and receives every field/event panel draw again, with the game's own blend (alpha masks evolve
// identically, the alpha channel starts equal) and the same depth/stencil tests without writes. Per pixel
// P1 = C + K1(1-a), P2 = C + K2(1-a) -> a and the premultiplied colour C, exactly as the game blended.
static SRWLOCK g_noWriteLock = SRWLOCK_INIT;
static ID3D11DepthStencilState* g_nwFrom[64] = {}; static ID3D11DepthStencilState* g_nwTo[64] = {}; static int g_nwN = 0;
static ID3D11DepthStencilState* NoWriteDss(ID3D11DepthStencilState* from) {
    AcquireSRWLockShared(&g_noWriteLock);
    for (int i = 0; i < g_nwN; ++i) if (g_nwFrom[i] == from) { auto* t = g_nwTo[i]; ReleaseSRWLockShared(&g_noWriteLock); return t; }
    ReleaseSRWLockShared(&g_noWriteLock);
    D3D11_DEPTH_STENCIL_DESC d{};
    if (from) from->GetDesc(&d);
    else { d.DepthEnable = TRUE; d.DepthFunc = D3D11_COMPARISON_LESS; d.StencilEnable = FALSE; d.StencilReadMask = d.StencilWriteMask = 0xFF;
        d.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS }; d.BackFace = d.FrontFace; }
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; d.StencilWriteMask = 0;
    ID3D11DepthStencilState* to = nullptr; if (FAILED(g_dev->CreateDepthStencilState(&d, &to))) return nullptr;
    AcquireSRWLockExclusive(&g_noWriteLock);
    if (g_nwN < 64) { if (from) from->AddRef(); g_nwFrom[g_nwN] = from; g_nwTo[g_nwN++] = to; }
    ReleaseSRWLockExclusive(&g_noWriteLock);
    return to;
}
static bool HudCoverBegin(ID3D11DeviceContext* c, const HudState& hs, CovSave& sv) {
    if (!hs.redirect || !hs.cov) return false;
    ID3D11RenderTargetView* twin = g_covRTV.load(std::memory_order_acquire);
    if (!twin) return false;
    c->OMGetRenderTargets(1, &sv.rtv, &sv.dsv); c->OMGetBlendState(&sv.bs, sv.bf, &sv.sm); c->OMGetDepthStencilState(&sv.dss, &sv.ref);
    ID3D11DepthStencilState* nw = NoWriteDss(sv.dss);
    OrigOMSetRTs(c)(c, 1, &twin, sv.dsv);
    if (nw) c->OMSetDepthStencilState(nw, sv.ref);
    g_covReplays.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static void HudCoverEnd(ID3D11DeviceContext* c, CovSave& sv) {
    OrigOMSetRTs(c)(c, 1, &sv.rtv, sv.dsv); c->OMSetBlendState(sv.bs, sv.bf, sv.sm); c->OMSetDepthStencilState(sv.dss, sv.ref);
    if (sv.rtv) sv.rtv->Release(); if (sv.dsv) sv.dsv->Release(); if (sv.bs) sv.bs->Release(); if (sv.dss) sv.dss->Release();
}
static std::atomic<uint32_t> g_uiCoverMarks{0};
// [MENUCENSUS2] every UI draw moved to the panel while a menu is open, grouped by how it blends
struct UiDrawGroup { uint32_t key[10]; uint32_t n; };
static std::mutex g_uiGrpMx; static UiDrawGroup g_uiGrp[48]; static int g_uiGrpN = 0;
static void UiCensus(ID3D11DeviceContext* c, bool dsvBound, bool covered) {
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm; c->OMGetBlendState(&bs, bf, &sm);
    D3D11_BLEND_DESC d{}; if (bs) { bs->GetDesc(&d); bs->Release(); } else { d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; d.RenderTarget[0].RenderTargetWriteMask = 15; }
    const auto& r = d.RenderTarget[0];
    const uint32_t k[10] = { (uint32_t)r.BlendEnable, (uint32_t)r.SrcBlend, (uint32_t)r.DestBlend, (uint32_t)r.BlendOp, (uint32_t)r.SrcBlendAlpha, (uint32_t)r.DestBlendAlpha, (uint32_t)r.BlendOpAlpha, (uint32_t)r.RenderTargetWriteMask, (uint32_t)dsvBound, (uint32_t)covered };
    std::lock_guard<std::mutex> lk(g_uiGrpMx);
    for (int i = 0; i < g_uiGrpN; ++i) if (!memcmp(g_uiGrp[i].key, k, sizeof(k))) { ++g_uiGrp[i].n; return; }
    if (g_uiGrpN < 48) { memcpy(g_uiGrp[g_uiGrpN].key, k, sizeof(k)); g_uiGrp[g_uiGrpN++].n = 1; }
}
static bool UiCoverInit() {
    if (g_fixPS) return true;
    if (!g_dev) return false;
    D3D11_DEPTH_STENCIL_DESC m{}; m.DepthEnable = FALSE; m.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; m.DepthFunc = D3D11_COMPARISON_ALWAYS;
    m.StencilEnable = TRUE; m.StencilReadMask = 0xFF; m.StencilWriteMask = 0xFF;
    m.FrontFace = { D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS }; m.BackFace = m.FrontFace;
    D3D11_DEPTH_STENCIL_DESC t = m; t.StencilWriteMask = 0;
    t.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_EQUAL }; t.BackFace = t.FrontFace;
    D3D11_BLEND_DESC b{}; b.RenderTarget[0].BlendEnable = FALSE; b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
    D3D11_RASTERIZER_DESC r{}; r.FillMode = D3D11_FILL_SOLID; r.CullMode = D3D11_CULL_NONE; r.DepthClipEnable = TRUE;
    const char* src =
        "float4 vs(uint i : SV_VertexID) : SV_Position { float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
        "float4 ps() : SV_Target { return float4(0, 0, 0, 1); }"
        "Texture2D t : register(t0);"
        "Texture2D cv : register(t1);"
        "cbuffer DimCap : register(b0) { float4 dimCap; };"   // [EVTDIM] x = max opacity of near-black panel pixels during events (1 = off)
        "float4 key(float4 p, float menuA) { float4 c = t.Load(int3(p.xy, 0)); float3 d = abs(c.rgb - float3(3, 5, 4) / 255.0);"
        "  if (max(d.r, max(d.g, d.b)) < 0.5 / 255.0) return float4(0, 0, 0, 0);"
        "  if (menuA > 0) return float4(c.rgb, 1);"
        "  float3 k = float3(3, 5, 4) / 255.0; float3 w = cv.Load(int3(p.xy, 0)).rgb;"   // [TWINKEY] P1 = c (key start), P2 = w (white start)
        "  float3 t = saturate((w - c.rgb) / (1 - k));"                                   // (1 - a) per channel
        "  float a = saturate(1 - (t.r + t.g + t.b) / 3);"
        "  float3 col = saturate(c.rgb - k * (1 - a));"                                   // premultiplied colour
        "  if (a >= 0.78) { col = saturate(col / a); a = 1; }"                            // near-solid UI (text box ~0.8) solid; dims keep their own strength
        "  return float4(col, a); }"
        "float4 psField(float4 p : SV_Position) : SV_Target { return key(p, 0); }"
        "float4 psMenu(float4 p : SV_Position) : SV_Target { return key(p, 1); }";
    ID3DBlob *vb = nullptr, *pb = nullptr, *kb0 = nullptr, *kb1 = nullptr;
    bool ok = SUCCEEDED(g_dev->CreateDepthStencilState(&m, &g_dssMark)) && SUCCEEDED(g_dev->CreateDepthStencilState(&t, &g_dssTest))
        && SUCCEEDED(g_dev->CreateBlendState(&b, &g_blendAlphaOnly)) && SUCCEEDED(g_dev->CreateRasterizerState(&r, &g_rsNoCull))
        && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr))
        && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr))
        && SUCCEEDED(g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_fixVS))
        && SUCCEEDED(g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_fixPS))
        && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "psField", "ps_4_0", 0, 0, &kb0, nullptr))
        && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "psMenu", "ps_4_0", 0, 0, &kb1, nullptr))
        && SUCCEEDED(g_dev->CreatePixelShader(kb0->GetBufferPointer(), kb0->GetBufferSize(), nullptr, &g_keyPS[0]))
        && SUCCEEDED(g_dev->CreatePixelShader(kb1->GetBufferPointer(), kb1->GetBufferSize(), nullptr, &g_keyPS[1]));
    if (vb) vb->Release(); if (pb) pb->Release(); if (kb0) kb0->Release(); if (kb1) kb1->Release();
    if (ok) {   // [COVREPLAY] coverage blends: alpha channel only
        D3D11_BLEND_DESC cb{}; auto& r = cb.RenderTarget[0];
        r.BlendEnable = TRUE; r.SrcBlend = D3D11_BLEND_ZERO; r.DestBlend = D3D11_BLEND_ONE; r.BlendOp = D3D11_BLEND_OP_ADD; r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        ok = SUCCEEDED(g_dev->CreateBlendState(&cb, &g_covBlend[CovOver]));
        r.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        ok = ok && SUCCEEDED(g_dev->CreateBlendState(&cb, &g_covBlend[CovSolid]));
        r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ONE; r.BlendOpAlpha = D3D11_BLEND_OP_MAX;
        ok = ok && SUCCEEDED(g_dev->CreateBlendState(&cb, &g_covBlend[CovMax]));
        D3D11_DEPTH_STENCIL_DESC o{}; o.DepthEnable = FALSE; o.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; o.DepthFunc = D3D11_COMPARISON_ALWAYS; o.StencilEnable = FALSE;
        ok = ok && SUCCEEDED(g_dev->CreateDepthStencilState(&o, &g_dssOff));
    }
    Log("[keycover] key-colour coverage pass %s", ok ? "ready" : "FAILED to build (panel keeps the game's alpha)");
    return ok;
}
// Present thread, once per frame, before the panel is dumped / handed to the headset: [KEYCOVER].
static std::atomic<uint32_t> g_keyCoverRuns{0};
static ID3D11Buffer* g_dimCapCB = nullptr;
static std::atomic<float> g_dimCapNow{1.f};   // [EVTDIM] set per frame by the present thread
static void UiCoverFixup(ID3D11Texture2D* tex, ID3D11RenderTargetView* rtv, bool menu) {
    if (!tex || !rtv || !g_keyPS[menu ? 1 : 0] || !g_ctxImm || !g_dev || (!menu && !g_covSRV)) return;
    D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);
    if (g_hudCopy) { D3D11_TEXTURE2D_DESC cd{}; g_hudCopy->GetDesc(&cd);
        if (cd.Width != td.Width || cd.Height != td.Height || cd.Format != td.Format) { g_hudCopySRV->Release(); g_hudCopySRV = nullptr; g_hudCopy->Release(); g_hudCopy = nullptr; } }
    if (!g_hudCopy) {
        D3D11_TEXTURE2D_DESC cd = td; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.CPUAccessFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT;
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rtv->GetDesc(&rd);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = rd.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateTexture2D(&cd, nullptr, &g_hudCopy)) || !g_hudCopy) { g_hudCopy = nullptr; return; }
        if (FAILED(g_dev->CreateShaderResourceView(g_hudCopy, &sd, &g_hudCopySRV)) || !g_hudCopySRV) { g_hudCopy->Release(); g_hudCopy = nullptr; g_hudCopySRV = nullptr; return; }
        Log("[keycover] panel copy %ux%u fmt=%u view fmt=%u", td.Width, td.Height, (unsigned)td.Format, (unsigned)rd.Format);
    }
    const UINT w = td.Width, h = td.Height;
    ID3D11DeviceContext* c = g_ctxImm;
    c->CopyResource(g_hudCopy, tex);
    g_keyCoverRuns.fetch_add(1, std::memory_order_relaxed);
    P5RMenu::internalCall = true;   // our own draw: game hooks pass it through untouched
    ID3D11RenderTargetView* oRtv[8] = {}; ID3D11DepthStencilView* oDsv = nullptr; c->OMGetRenderTargets(8, oRtv, &oDsv);
    ID3D11DepthStencilState* oDss = nullptr; UINT oRef = 0; c->OMGetDepthStencilState(&oDss, &oRef);
    ID3D11BlendState* oBs = nullptr; FLOAT oBf[4]; UINT oSm = 0; c->OMGetBlendState(&oBs, oBf, &oSm);
    ID3D11RasterizerState* oRs = nullptr; c->RSGetState(&oRs);
    D3D11_VIEWPORT oVp[16]; UINT nVp = 16; c->RSGetViewports(&nVp, oVp);
    ID3D11VertexShader* oVs = nullptr; c->VSGetShader(&oVs, nullptr, nullptr);
    ID3D11PixelShader* oPs = nullptr; c->PSGetShader(&oPs, nullptr, nullptr);
    ID3D11GeometryShader* oGs = nullptr; c->GSGetShader(&oGs, nullptr, nullptr);
    ID3D11InputLayout* oIl = nullptr; c->IAGetInputLayout(&oIl);
    D3D11_PRIMITIVE_TOPOLOGY oTopo; c->IAGetPrimitiveTopology(&oTopo);
    ID3D11ShaderResourceView* oSrv[2] = {}; c->PSGetShaderResources(0, 2, oSrv);
    OrigOMSetRTs(c)(c, 1, &rtv, nullptr);
    c->OMSetDepthStencilState(nullptr, 0); const FLOAT bf[4] = {}; c->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    c->RSSetState(g_rsNoCull); D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 }; c->RSSetViewports(1, &vp);
    c->VSSetShader(g_fixVS, nullptr, 0); c->PSSetShader(g_keyPS[menu ? 1 : 0], nullptr, 0); c->GSSetShader(nullptr, nullptr, 0);
    if (!g_dimCapCB) { D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; g_dev->CreateBuffer(&bd, nullptr, &g_dimCapCB); }
    ID3D11Buffer* oCb = nullptr; c->PSGetConstantBuffers(0, 1, &oCb);
    if (g_dimCapCB) { const float v[4] = { menu ? 1.f : g_dimCapNow.load(std::memory_order_relaxed), 0, 0, 0 }; c->UpdateSubresource(g_dimCapCB, 0, nullptr, v, 0, 0); c->PSSetConstantBuffers(0, 1, &g_dimCapCB); }
    ID3D11ShaderResourceView* in[2] = { g_hudCopySRV, g_covSRV }; c->PSSetShaderResources(0, 2, in);
    c->IASetInputLayout(nullptr); c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);
    c->PSSetShaderResources(0, 2, oSrv); for (auto* v : oSrv) if (v) v->Release();
    c->PSSetConstantBuffers(0, 1, &oCb); if (oCb) oCb->Release();
    OrigOMSetRTs(c)(c, 8, oRtv, oDsv); c->OMSetDepthStencilState(oDss, oRef); c->OMSetBlendState(oBs, oBf, oSm);
    c->RSSetState(oRs); c->RSSetViewports(nVp, oVp); c->VSSetShader(oVs, nullptr, 0); c->PSSetShader(oPs, nullptr, 0); c->GSSetShader(oGs, nullptr, 0);
    c->IASetInputLayout(oIl); c->IASetPrimitiveTopology(oTopo);
    for (auto& v : oRtv) if (v) v->Release(); if (oDsv) oDsv->Release(); if (oDss) oDss->Release(); if (oBs) oBs->Release(); if (oRs) oRs->Release();
    if (oVs) oVs->Release(); if (oPs) oPs->Release(); if (oGs) oGs->Release(); if (oIl) oIl->Release();
    P5RMenu::internalCall = false;
}
static void HudDumpAlpha(ID3D11Texture2D* tex, const char* rgbName, const char* alphaName);
struct UiSeq { void* rt; uint32_t rtW, rtH, rtFmt, dsv, indexed, blendOn, src, dst, srcA, dstA, mask; void* srv0; uint32_t srvW, srvH, srvFmt; uint32_t n, lo, hi, snap; };
static std::vector<UiSeq> g_uiSeq;            // render thread only (immediate context)
static ID3D11Texture2D* g_uiSnap[8] = {};
static int g_uiSnapUsed = 0;
static std::atomic<bool> g_uiSnapArmed{false};
static bool g_uiSnapDone = false;
static void UiSeqRecord(ID3D11DeviceContext* c, bool indexed, UINT count, CtxScene* s) {
    UiSeq e{};
    e.rt = s->rtTex.load(std::memory_order_relaxed); e.rtW = s->rtW.load(); e.rtH = s->rtH.load(); e.rtFmt = s->rtFmt.load(); e.dsv = s->rtDsv.load(); e.indexed = indexed;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm);
    if (bs) { D3D11_BLEND_DESC d{}; bs->GetDesc(&d); const auto& r = d.RenderTarget[0];
        e.blendOn = r.BlendEnable; e.src = r.SrcBlend; e.dst = r.DestBlend; e.srcA = r.SrcBlendAlpha; e.dstA = r.DestBlendAlpha; e.mask = r.RenderTargetWriteMask; bs->Release(); }
    else e.mask = 15;
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); e.srv0 = r;
        if (r) { ID3D11Texture2D* tx = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx)) && tx) { D3D11_TEXTURE2D_DESC td{}; tx->GetDesc(&td); e.srvW = td.Width; e.srvH = td.Height; e.srvFmt = td.Format; tx->Release(); } r->Release(); }
        v->Release(); }
    e.n = 1; e.lo = e.hi = count; e.snap = 0xFFFFFFFF;
    if (!g_uiSeq.empty()) {
        UiSeq& p = g_uiSeq.back();
        // srv0 identity is ignored for the HUD's atlas churn; only size/format distinguish passes
        if (p.rt == e.rt && p.dsv == e.dsv && p.indexed == e.indexed && p.blendOn == e.blendOn && p.src == e.src && p.dst == e.dst
            && p.srcA == e.srcA && p.dstA == e.dstA && p.mask == e.mask && p.srvW == e.srvW && p.srvH == e.srvH && p.srvFmt == e.srvFmt) {
            ++p.n; p.lo = (std::min)(p.lo, (uint32_t)count); p.hi = (std::max)(p.hi, (uint32_t)count); return; }
    }
    // a new kind of draw begins: snapshot the backbuffer BEFORE it (shows what earlier passes left)
    if (g_uiSnapArmed.load(std::memory_order_relaxed) && g_uiSnapUsed < 8 && g_uiSnap[g_uiSnapUsed] && g_curBB && g_origCopyRes) {
        g_origCopyRes(c, g_uiSnap[g_uiSnapUsed], g_curBB); e.snap = (uint32_t)g_uiSnapUsed++; }
    if (g_uiSeq.size() < 400) g_uiSeq.push_back(e);
}
static void UiSeqPresent(uint64_t f, IDXGISwapChain* sc, bool scanned) {
    if (scanned) {
        Log("[uiseq] f%llu immediate-context sequence (%zu groups; bb=%p): rt WxH fmt dsv idx blend(src,dst/srcA,dstA mask) psSrv0 WxH fmt : draws (min..max) [snap]",
            (unsigned long long)f, g_uiSeq.size(), (void*)g_curBB);
        for (const auto& e : g_uiSeq)
            Log("[uiseq]   rt=%p%s %ux%u fmt=%u dsv=%u idx=%u blend=%u(%u,%u/%u,%u m%X) srv0=%p %ux%u fmt=%u : %u (%u..%u)%s",
                e.rt, e.rt == (void*)g_curBB ? "(BB)" : "", e.rtW, e.rtH, e.rtFmt, e.dsv, e.indexed, e.blendOn, e.src, e.dst, e.srcA, e.dstA, e.mask,
                e.srv0, e.srvW, e.srvH, e.srvFmt, e.n, e.lo, e.hi,
                e.snap != 0xFFFFFFFF ? (e.snap == 0 ? " [snap0 before]" : e.snap == 1 ? " [snap1 before]" : e.snap == 2 ? " [snap2 before]" : e.snap == 3 ? " [snap3 before]" : e.snap == 4 ? " [snap4 before]" : e.snap == 5 ? " [snap5 before]" : e.snap == 6 ? " [snap6 before]" : " [snap7 before]") : "");
        g_uiSeq.clear();
        if (g_uiSnapArmed.exchange(false)) {
            for (int i = 0; i < g_uiSnapUsed; ++i) { char name[64]; snprintf(name, sizeof(name), "p5r_uiseq_%d.bmp", i); HudDumpBmp(g_uiSnap[i], name); }
            Log("[uiseq] wrote %d snapshots p5r_uiseq_N.bmp (backbuffer before group N)", g_uiSnapUsed);
            for (auto& t : g_uiSnap) if (t) { t->Release(); t = nullptr; }
            g_uiSnapUsed = 0; g_uiSnapDone = true;
        }
    }
}
// Arm snapshots once, for a gameplay frame with a busy HUD and no game menu.
static void UiSeqArmSnapshots(IDXGISwapChain* sc, uint32_t lastImmDraws) {
    if (g_uiSnapDone || g_uiSnapArmed.load() || lastImmDraws < 500 || g_screenView.load() || !sc || !g_dev) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d{}; bb->GetDesc(&d); bb->Release();
    d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
    for (auto& t : g_uiSnap) if (!t && FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) t = nullptr;
    g_uiSnapUsed = 0; g_uiSnapArmed.store(true);
    Log("[uiseq] snapshots ARMED for the next frame (last frame immediate draws=%u)", lastImmDraws);
}
// [HUDLAYER v2] Measured (runs 3-5): the immediate-context "HUD pass" only carries title/loading
// overlays (its capture was empty in gameplay, and the immediate composite reads ONLY the scene image,
// PS t0 = 3840x2160 fmt 27). The gameplay HUD is drawn on a DEFERRED context straight into that scene
// image after the 3D: ~400 blended, depth-less draws ([hudscan] ctx1 fmt=27 dsv=0 : 402). So each
// such draw is redirected into our own transparent HUD target (cleared every Present) and is not
// mirrored. The eye images keep only the 3D world; the HUD target goes to the headset as a quad.
static std::atomic<bool> g_hudLayerOn{true};
static std::atomic<bool> g_hudXrLive{false};             // set at Present from the XR state
static std::atomic<void*> g_sceneFinalRT{nullptr};       // scene image read by the immediate composite (t0)
static std::atomic<uint32_t> g_hudRtvFmtWanted{0};       // RTV format the game uses on that image
static ID3D11Texture2D* g_hudTex = nullptr;              // our HUD target (render thread owns)
static std::atomic<ID3D11RenderTargetView*> g_hudRTV{nullptr};
static std::vector<IUnknown*> g_hudGraveyard;            // replaced on resize; never freed under a worker
static std::atomic<bool> g_hudDrawnThisFrame{false};
// [WPNOTE] 10-07: map menus lose the city-map roads and Joker's portrait flashes; WORLDPIC drops ~7
// draws/frame there. Record each distinct dropped picture (texture, vertex count) and save it once.
struct WpNote { ID3D11Texture2D* tx; uint32_t w, h, fmt, count, n; void* vs; uint32_t blend; char task[32]; bool dumped; };
static std::mutex g_wpMx;
static std::vector<WpNote> g_wpNotes;
static void WorldPicNote(ID3D11DeviceContext* c, UINT count, void* vs, bool blend, const char* task) {
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    if (!v) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release();
    ID3D11Texture2D* tx = nullptr;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx); r->Release(); }
    if (!tx) return;
    std::lock_guard<std::mutex> lk(g_wpMx);
    for (auto& w : g_wpNotes) if (w.tx == tx && w.count == count) { ++w.n; tx->Release(); return; }
    if (g_wpNotes.size() >= 24) { tx->Release(); return; }
    D3D11_TEXTURE2D_DESC d{}; tx->GetDesc(&d);
    WpNote w{ tx, d.Width, d.Height, (uint32_t)d.Format, count, 1, vs, (uint32_t)blend, {}, false };
    snprintf(w.task, sizeof(w.task), "%s", task ? task : "none"); for (char* p = w.task; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
    g_wpNotes.push_back(w);   // keeps the reference
}
// [PXPROBE] 10-09: which panel draw paints the event letterbox. For one frame, after every panel draw,
// copy two panel pixels (top bar, bottom-left bar) into a strip; at Present the strip is read back and
// every draw that changed either pixel is logged with its texture / vertex count / shaders.
static std::atomic<bool> g_pxArmed{false};
// [BARPRED] 10-12: the cutscene bars (and the leftover strip) are untextured shapes living only in the top and
// bottom screen strips; the dialog box / date / name tags all reach into the middle. In events, every
// untextured panel draw is first rasterised invisibly with a scissor over the middle 76% of the screen inside
// an occlusion predicate; the real draw (and its twin) run predicated: no middle pixel = a bar piece = skipped.
// GPU-side, same frame, no readback.
struct BarSave { ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT sm = 0; ID3D11DepthStencilState* dss = nullptr; UINT ref = 0; ID3D11RasterizerState* rs = nullptr; UINT nsc = 0; D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; };
static ID3D11Predicate* g_barPred = nullptr;
static ID3D11BlendState* g_blendNoWrite = nullptr;
static std::atomic<uint32_t> g_barTests{0};
static SRWLOCK g_rsLock = SRWLOCK_INIT; static ID3D11RasterizerState* g_rsFrom[32] = {}; static ID3D11RasterizerState* g_rsTo[32] = {}; static int g_rsN = 0;
static ID3D11RasterizerState* ScissorRs(ID3D11RasterizerState* from) {
    AcquireSRWLockShared(&g_rsLock);
    for (int i = 0; i < g_rsN; ++i) if (g_rsFrom[i] == from) { auto* t = g_rsTo[i]; ReleaseSRWLockShared(&g_rsLock); return t; }
    ReleaseSRWLockShared(&g_rsLock);
    D3D11_RASTERIZER_DESC d{}; if (from) from->GetDesc(&d); else { d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_BACK; d.DepthClipEnable = TRUE; }
    d.ScissorEnable = TRUE;
    ID3D11RasterizerState* to = nullptr; if (FAILED(g_dev->CreateRasterizerState(&d, &to))) return nullptr;
    AcquireSRWLockExclusive(&g_rsLock);
    if (g_rsN < 32) { if (from) from->AddRef(); g_rsFrom[g_rsN] = from; g_rsTo[g_rsN++] = to; }
    ReleaseSRWLockExclusive(&g_rsLock);
    return to;
}
static bool BarTestBegin(ID3D11DeviceContext* c, const HudState& hs, BarSave& sv) {
    if (!hs.redirect || !hs.barCand || !g_hudTex || !g_dev) return false;
    if (!g_barPred) { D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 }; if (FAILED(g_dev->CreatePredicate(&qd, &g_barPred))) { g_barPred = nullptr; return false; } }
    if (!g_blendNoWrite) { D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = 0; if (FAILED(g_dev->CreateBlendState(&bd, &g_blendNoWrite))) return false; }
    c->OMGetBlendState(&sv.bs, sv.bf, &sv.sm); c->OMGetDepthStencilState(&sv.dss, &sv.ref); c->RSGetState(&sv.rs);
    sv.nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetScissorRects(&sv.nsc, sv.sc);
    ID3D11RasterizerState* rs = ScissorRs(sv.rs);
    if (!rs || !g_dssOff) { if (sv.bs) sv.bs->Release(); if (sv.dss) sv.dss->Release(); if (sv.rs) sv.rs->Release(); return false; }
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    const D3D11_RECT mid{ 0, (LONG)(d.Height * 12 / 100), (LONG)d.Width, (LONG)(d.Height * 88 / 100) };
    c->OMSetBlendState(g_blendNoWrite, sv.bf, 0xFFFFFFFF); c->OMSetDepthStencilState(g_dssOff, 0); c->RSSetState(rs); c->RSSetScissorRects(1, &mid);
    c->Begin(g_barPred);
    g_barTests.fetch_add(1, std::memory_order_relaxed);
    return true;
}
static void BarTestEnd(ID3D11DeviceContext* c, BarSave& sv) {
    c->End(g_barPred);
    c->OMSetBlendState(sv.bs, sv.bf, sv.sm); c->OMSetDepthStencilState(sv.dss, sv.ref); c->RSSetState(sv.rs); c->RSSetScissorRects(sv.nsc, sv.sc);
    if (sv.bs) sv.bs->Release(); if (sv.dss) sv.dss->Release(); if (sv.rs) sv.rs->Release();
    c->SetPredication(g_barPred, FALSE);   // following draws are skipped when no middle sample passed
}
static void BarPredClear(ID3D11DeviceContext* c) { c->SetPredication(nullptr, FALSE); }
// [CHOICEDIM] 10-09 probe: during Joker's choice cut-in one textured 4-vertex draw darkens the mid-left
// panel pixel to near black (key -> 5C020201) - the choice dim, which on the panel became dark boxes
// over the bar and text box. The probe names its texture; draws with that texture are skipped in events.
static std::atomic<void*> g_dimTex{nullptr};
static std::atomic<uint32_t> g_dimSkips{0};
static std::atomic<uint32_t> g_pxN{0};
static ID3D11Texture2D* g_pxStrip = nullptr;   // 1024 x 2, HUD format
struct PxMeta { void* t0; void* vs; uint32_t verts; uint8_t blend; uint32_t tw, th; };
static PxMeta g_pxMeta[1024];
static void PxProbeAfterDraw(ID3D11DeviceContext* c, const HudState& hs) {
    if (!g_pxArmed.load(std::memory_order_relaxed) || !g_pxStrip || !g_hudTex) return;
    const uint32_t i = g_pxN.fetch_add(1, std::memory_order_relaxed);
    if (i >= 1024) return;
    g_pxMeta[i] = { hs.pxT0, hs.pxVs, hs.pxVerts, hs.pxBlend, hs.pxTw, hs.pxTh };
    D3D11_TEXTURE2D_DESC d{}; g_hudTex->GetDesc(&d);
    const UINT tx = d.Width / 2, ty = d.Height * 2 / 100, bx = d.Width * 88 / 100, by = d.Height * 15 / 100;   // top bar / inside Joker's choice portrait (top right)
    D3D11_BOX top{ tx, ty, 0, tx + 1, ty + 1, 1 }, bot{ bx, by, 0, bx + 1, by + 1, 1 };
    c->CopySubresourceRegion(g_pxStrip, 0, i, 0, 0, g_hudTex, 0, &top);
    c->CopySubresourceRegion(g_pxStrip, 0, i, 1, 0, g_hudTex, 0, &bot);
}
// [EVTUI] 10-09: during events the panel carries opaque letterbox bars (likely the "overlay" to remove).
// Group the panel draws of events by (PS t0 texture, vertex count, VS, blend), log them and save each
// texture once, so the bar draw is identified by evidence before anything is removed.
struct EvtUiNote { ID3D11Texture2D* tx; uint32_t w, h, fmt, count, n; void* vs; uint32_t blend; bool dumped; };
static std::mutex g_evtUiMx; static std::vector<EvtUiNote> g_evtUi;
static void EventUiNote(ID3D11DeviceContext* c, UINT count, void* vs, bool blend) {
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    ID3D11Texture2D* tx = nullptr;
    if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release(); if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx); r->Release(); } }
    std::lock_guard<std::mutex> lk(g_evtUiMx);
    for (auto& e : g_evtUi) if (e.tx == tx && e.count == count && e.vs == vs) { ++e.n; if (tx) tx->Release(); return; }
    if (g_evtUi.size() >= 40) { if (tx) tx->Release(); return; }
    D3D11_TEXTURE2D_DESC d{}; if (tx) tx->GetDesc(&d);
    g_evtUi.push_back({ tx, d.Width, d.Height, (uint32_t)d.Format, count, 1, vs, (uint32_t)blend, false });
}
// [WORLDPIC] Run 14 dumps: while a menu is open the game paints its own FLAT, dimmed picture of the
// world as a full-screen menu layer (map menu, Text Log, Save). Redirected with the menu art, that
// flat world sat on the panel, Hinny tha GOAT, in front of the frozen stereo world = a double, depth-less world.
// While the world is frozen, any would-be UI draw that samples a large RENDERED image (a render
// target >= 1280 wide; menu art comes from non-RT textures) is not drawn at all.
static std::atomic<bool> g_worldFrozen{false};
static std::atomic<bool> g_worldLiveRaw{true};   // [FREEZEHOLD] the game is drawing the world this frame (frozen may still be held)
static constexpr bool g_worldPicRule = true;   // run 16: exact scene-image-size rule (was falsified as "RT >= 1280")
// [MENUCENSUS] run 15: identify the flat world picture from evidence. While the world is frozen,
// every draw headed for the panel is grouped by (PS t0 texture, vertex shader, blend); the first two
// freeze episodes log the groups and save each group's t0 texture as a picture.
struct MenuGroup { void* tex; void* vs; uint32_t w, h, fmt, bind, blend, dsv, indexed, n; };
static std::mutex g_menuCensusMx;
static std::vector<MenuGroup> g_menuGroups;
static std::vector<ID3D11Texture2D*> g_menuGroupTex;   // refs held until dumped (render thread releases)
static std::atomic<bool> g_menuCensusArmed{false};
static void MenuCensusRecord(ID3D11DeviceContext* c, bool indexed, void* vs, bool blend, bool dsv) {
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    ID3D11Texture2D* tx = nullptr; D3D11_TEXTURE2D_DESC d{};
    if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r);
        if (r) { if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx)) && tx) tx->GetDesc(&d); r->Release(); }
        v->Release(); }
    std::lock_guard<std::mutex> lk(g_menuCensusMx);
    for (auto& g : g_menuGroups) if (g.tex == (void*)tx && g.vs == vs && g.blend == (uint32_t)blend && g.dsv == (uint32_t)dsv) { ++g.n; if (tx) tx->Release(); return; }
    if (g_menuGroups.size() >= 48) { if (tx) tx->Release(); return; }
    g_menuGroups.push_back({ (void*)tx, vs, d.Width, d.Height, (uint32_t)d.Format, d.BindFlags, (uint32_t)blend, (uint32_t)dsv, (uint32_t)indexed, 1 });
    g_menuGroupTex.push_back(tx);   // keeps the reference (may be null)
}
static void MenuCensusDump(int episode) {
    std::vector<MenuGroup> groups; std::vector<ID3D11Texture2D*> texs;
    { std::lock_guard<std::mutex> lk(g_menuCensusMx); groups.swap(g_menuGroups); texs.swap(g_menuGroupTex); }
    Log("[menucensus] episode %d: %zu groups of panel-bound draws while frozen (60 frames):", episode, groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& g = groups[i];
        Log("[menucensus]   #%zu t0=%p %ux%u fmt=%u bind=0x%X%s vs=%p blend=%u dsv=%u idx=%u draws=%u",
            i, g.tex, g.w, g.h, g.fmt, g.bind, (g.bind & D3D11_BIND_RENDER_TARGET) ? "(RT)" : "", g.vs, g.blend, g.dsv, g.indexed, g.n);
        // save every texture that is not a tiny atlas, so the flat world picture is identified by eye
        const bool rgba8 = g.fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || g.fmt == DXGI_FORMAT_R8G8B8A8_UNORM || g.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            || g.fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || g.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || g.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // 4 bytes/pixel only (block formats would overrun)
        if (texs[i] && rgba8 && g.w >= 256 && g.h >= 256) {
            char a[64], b[64]; snprintf(a, sizeof(a), "p5r_menutex_e%d_%02zu.bmp", episode, i); snprintf(b, sizeof(b), "p5r_menutex_e%d_%02zu_alpha.bmp", episode, i);
            HudDumpAlpha(texs[i], a, b);
        }
    }
    for (auto* t : texs) if (t) t->Release();
}
static std::atomic<uint32_t> g_worldPicSkips{0};
static bool SamplesWorldPicture(ID3D11DeviceContext* c) {
    // Run 16 census (map menu + save, saved as pictures): the flat world copy is PS t0 = a 1920x1080
    // RGBA8 RENDER TARGET = half the 3840x2160 scene image (a fresh texture per menu). Menu art comes
    // from 96x96 / 260x260 render targets and BC7 atlases, so match the scene-image size exactly.
    void* scene = g_sceneFinalRT.load(std::memory_order_relaxed);
    if (!scene) return false;
    D3D11_TEXTURE2D_DESC sd{};
    { ID3D11Texture2D* st = nullptr; if (FAILED(((ID3D11Resource*)scene)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&st)) || !st) return false; st->GetDesc(&sd); st->Release(); }
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    if (!v) return false;
    bool found = false;
    ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release();
    ID3D11Texture2D* tx = nullptr;
    if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx)) && tx) {
        D3D11_TEXTURE2D_DESC d{}; tx->GetDesc(&d);
        const bool rt = (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
        const bool half = d.Width == sd.Width / 2 && d.Height == sd.Height / 2;
        const bool full = d.Width == sd.Width && d.Height == sd.Height;
        found = rt && (half || full) && (void*)r != scene;
        tx->Release();
    }
    if (r) r->Release();
    return found;
}
// [MENUVR] a game menu is open and shown on the docked panel over the stereo world (not flat).
static std::atomic<bool> g_menuInVr{false};
static std::atomic<uint32_t> g_menuOpaqueRedirects{0}, g_menuModelRedirects{0};
// [MENUVR] run 10: the menu's 3D models (Joker, Personas) are OPAQUE depth-tested draws into the scene
// image with their own vertex shaders. Learn every vertex shader that draws into the scene image
// during settled gameplay; inside a menu, anything drawn with a shader never seen in gameplay is
// menu content and goes to the panel (with the game's depth buffer still bound).
static std::atomic<void*> g_playVS[96];
static std::atomic<uint32_t> g_playVSCount{0};
// Run 11 showed the menu panel WHITE with its lower part missing after menu-only shaders
// were moved. Moving them is now a live toggle (Numpad 1, default OFF) and Numpad 2 dumps the panel
// image (colour + alpha) and both eye images, so the cause is read from pixels, not counters.
static std::atomic<bool> g_menuModelsOn{false};
// Run 12: with menu-only draws left in the world, the LEFT eye went black in menus (the right eye's
// mirror copy never got them). Default now removes them from the world: both eyes keep the world
// behind the menu. Numpad 1 cycles remove -> panel -> leave.
static std::atomic<int> g_menuOnlyMode{0};
static std::atomic<bool> g_hudDumpReq{false};

static std::atomic<uint64_t> g_hudFrameNo{0}, g_lastMenuFrameNo{0};
static bool IsPlayVS(void* vs) {
    const uint32_t n = (std::min)(g_playVSCount.load(std::memory_order_acquire), 96u);
    for (uint32_t i = 0; i < n; ++i) if (g_playVS[i].load(std::memory_order_relaxed) == vs) return true;
    return false;
}
static void LearnPlayVS(void* vs) {
    if (!vs || IsPlayVS(vs)) return;
    static SRWLOCK s_lock = SRWLOCK_INIT;   // workers learn concurrently; keep the list append-only
    AcquireSRWLockExclusive(&s_lock);
    if (!IsPlayVS(vs)) {
        const uint32_t i = g_playVSCount.load(std::memory_order_relaxed);
        if (i < 96) { g_playVS[i].store(vs, std::memory_order_relaxed); g_playVSCount.store(i + 1, std::memory_order_release); }
    }
    ReleaseSRWLockExclusive(&s_lock);
}
struct MenuMiss { void* vs; uint8_t blend, dsv; uint32_t n; };
static std::mutex g_menuMissMx;
static MenuMiss g_menuMiss[16];
static int g_menuMissCount = 0;
static void NoteMenuMiss(void* vs, bool blend, bool dsv) {
    std::lock_guard<std::mutex> lk(g_menuMissMx);
    for (int i = 0; i < g_menuMissCount; ++i) if (g_menuMiss[i].vs == vs && g_menuMiss[i].blend == blend && g_menuMiss[i].dsv == dsv) { ++g_menuMiss[i].n; return; }
    if (g_menuMissCount < 16) g_menuMiss[g_menuMissCount++] = { vs, (uint8_t)blend, (uint8_t)dsv, 1 };
}
static std::atomic<uint32_t> g_hudRedirects{0}, g_hudCandidates{0}, g_hudSkippedNoBlend{0}, g_hudSkippedDepth{0};
// Run 6: P5R alternates scene images frame to frame, so one remembered pointer matched only some
// frames. Keep every distinct image the composite has read recently.
static std::atomic<void*> g_sceneImgs[4];
static std::atomic<uint32_t> g_sceneImgNext{0};
static void RememberSceneImage(void* r) {
    for (auto& s : g_sceneImgs) if (s.load(std::memory_order_relaxed) == r) return;
    g_sceneImgs[g_sceneImgNext.fetch_add(1, std::memory_order_relaxed) & 3].store(r, std::memory_order_relaxed);
}
// Run 7: objectives box, minimap and speech bubbles are drawn with the depth TEST on (~190/frame
// were "depth-skipped"), so depth state cannot separate them from 3D transparents. Classify by
// vertex shader instead: learn the 2D sprite VS from draws that are certainly HUD (blended, no
// depth buffer bound, into the scene image), then treat any blended draw into the scene image that
// uses one of those shaders as HUD, whatever its depth state.
static std::atomic<void*> g_hudVS[16];
static std::atomic<uint32_t> g_hudVSCount{0};
static bool IsHudVS(void* vs) {
    if (!vs) return false;
    const uint32_t n = (std::min)(g_hudVSCount.load(std::memory_order_acquire), 16u);
    for (uint32_t i = 0; i < n; ++i) if (g_hudVS[i].load(std::memory_order_relaxed) == vs) return true;
    return false;
}
static void LearnHudVS(void* vs) {
    if (!vs || IsHudVS(vs)) return;
    uint32_t i = g_hudVSCount.load(std::memory_order_relaxed);
    if (i >= 16) return;
    g_hudVS[i].store(vs, std::memory_order_relaxed);
    g_hudVSCount.store(i + 1, std::memory_order_release);
    Log("[hudlayer] learned HUD vertex shader #%u = %p", i, vs);
}
static bool IsSceneImage(void* r) {
    if (!r) return false;
    for (auto& s : g_sceneImgs) if (s.load(std::memory_order_relaxed) == r) return true;
    return false;
}
// [FLASHTRACE] 10-15, LOG ONLY: the flash is the HUD panel briefly holding a full-screen world copy (alpha 0.04 -> 1.0
// for 2-5 frames, HUD~world correlation 0.7). Name the draw: log every redirected event draw whose signature
// (PS t0 resource, vertex shader, vertex count) was not seen in the previous 8 frames. Changes no routing.
static std::atomic<uint64_t> g_ftKey[4096], g_ftLast[4096];
static std::atomic<uint32_t> g_ftLogged{0};
static void FlashTraceNote(ID3D11DeviceContext* c, UINT count, void* vs, bool blend, bool dsv) {
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    ID3D11Resource* r = nullptr; if (v) { v->GetResource(&r); v->Release(); }
    const uint64_t f = g_hudFrameNo.load(std::memory_order_relaxed);
    uint64_t key = ((uint64_t)(uintptr_t)r * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uintptr_t)vs * 0xC2B2AE3D27D4EB4Full) ^ (uint64_t)count;
    if (!key) key = 1;
    uint32_t i = (uint32_t)(key >> 40) & 4095; bool isNew = false;
    for (int p = 0; p < 8;) {
        uint64_t k = g_ftKey[i].load(std::memory_order_relaxed);
        if (k == 0) { uint64_t e = 0; if (!g_ftKey[i].compare_exchange_strong(e, key)) continue; k = key; isNew = true; }
        if (k == key) { if (!isNew) isNew = g_ftLast[i].load(std::memory_order_relaxed) + 8 < f; g_ftLast[i].store(f, std::memory_order_relaxed); break; }
        i = (i + 1) & 4095; ++p;
    }
    if (isNew && g_ftLogged.fetch_add(1, std::memory_order_relaxed) < 4000) {
        D3D11_TEXTURE2D_DESC d{}; bool tex = false;
        if (r) { ID3D11Texture2D* t2 = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2) { t2->GetDesc(&d); t2->Release(); tex = true; } }
        Log("[flashtrace] f%llu new draw: t0=%p %ux%u fmt=%u bind=0x%x isRT=%d sceneImg=%d verts=%u vs=%p blend=%d dsv=%d",
            (unsigned long long)f, (void*)r, tex ? d.Width : 0, tex ? d.Height : 0, tex ? (unsigned)d.Format : 0u, tex ? (unsigned)d.BindFlags : 0u,
            (int)(tex && (d.BindFlags & D3D11_BIND_RENDER_TARGET) != 0), (int)IsSceneImage(r), (unsigned)count, vs, (int)blend, (int)dsv);
    }
    if (r) r->Release();
}
static bool HudLayerActive() {
    return g_hudLayerOn.load(std::memory_order_relaxed) && g_hudXrLive.load(std::memory_order_relaxed) && !g_screenView.load(std::memory_order_relaxed);
}
// [OVERLAY] P5R's second UI path (measured runs 3-5): on the immediate context it draws dialogs
// (save, backlog, title/loading) over the backbuffer, then copies the backbuffer into a 3840x2119
// RGBA16F layer with one draw (PS t0 = backbuffer). A later pass reads that layer into the image.
// While the panel is live: clear the backbuffer before the first of those draws (so the copy holds
// only the UI), keep our own copy for the panel, and give every pass that READS the layer a zero
// texture so the eyes never carry it. The reader is logged the first times it is found.
static constexpr bool g_overlayEnabled = false;
static std::atomic<void*> g_overlayLayerRT{nullptr};
static ID3D11Texture2D* g_ovTex = nullptr;
static ID3D11ShaderResourceView* g_ovZeroSRV = nullptr;
static std::atomic<bool> g_ovCaptured{false}, g_ovClearedThisFrame{false};
static std::atomic<uint32_t> g_ovCaptures{0}, g_ovStrips{0}, g_ovReaderLogs{0};
static std::atomic<uint64_t> g_ovLastStripFrame{0};
static void OverlayStrip(ID3D11DeviceContext* c, CtxScene* s, bool indexed, HudState& hs) {
    void* layer = g_overlayLayerRT.load(std::memory_order_relaxed);
    if (!layer || !g_ovZeroSRV || s->rtTex.load(std::memory_order_relaxed) == layer) return;
    // the reader is a full-screen pass: skip 3D (depth-bound) and small post targets cheaply
    if (s->rtDsv.load(std::memory_order_relaxed) || s->rtW.load(std::memory_order_relaxed) < 1024) return;
    ID3D11ShaderResourceView* views[16] = {}; c->PSGetShaderResources(0, 16, views);
    for (int i = 0; i < 16; ++i) {
        if (!views[i]) continue;
        ID3D11Resource* r = nullptr; views[i]->GetResource(&r);
        if (r && (void*)r == layer && hs.srvCount < 4) {
            hs.srvSlot[hs.srvCount] = (uint8_t)i; hs.srv[hs.srvCount++] = views[i]; views[i] = nullptr;
            c->PSSetShaderResources(i, 1, &g_ovZeroSRV);
        }
        if (r) r->Release();
        if (views[i]) views[i]->Release();
    }
    if (!hs.srvCount) return;
    g_ovStrips.fetch_add(1, std::memory_order_relaxed);
    g_ovLastStripFrame.store(g_hudFrameNo.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (g_ovReaderLogs.fetch_add(1) < 4) {
        ID3D11VertexShader* v = nullptr; c->VSGetShader(&v, nullptr, nullptr);
        Log("[overlay] reader found: %s ctx, target %ux%u fmt=%u dsv=%d indexed=%d, layer at PS t%u, vs=%p",
            c == g_ctxImm ? "immediate" : "deferred", s->rtW.load(), s->rtH.load(), s->rtFmt.load(), (int)s->rtDsv.load(), (int)indexed, hs.srvSlot[0], (void*)v);
        if (v) v->Release();
    }
}
static void OverlayImmediate(ID3D11DeviceContext* c, bool indexed, CtxScene* s) {
    void* rt = s->rtTex.load(std::memory_order_relaxed);
    if (!g_curBB) return;
    if (!indexed && rt == (void*)g_curBB && !g_ovClearedThisFrame.exchange(true, std::memory_order_relaxed)) {
        ID3D11RenderTargetView* rtv = nullptr; c->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) { const float zero[4] = { 0, 0, 0, 0 }; PickDef(c, g_origDefClearRTV, g_origClearRTV)(c, rtv, zero); rtv->Release(); }
    }
    if (indexed || s->rtFmt.load() != DXGI_FORMAT_R16G16B16A16_FLOAT || s->rtW.load() != g_bbW || s->rtH.load() != g_bbH) return;
    ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
    void* src = nullptr; if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); src = r; if (r) r->Release(); v->Release(); }
    if (src != (void*)g_curBB) return;
    g_overlayLayerRT.store(rt, std::memory_order_relaxed);
    D3D11_TEXTURE2D_DESC b{}; ((ID3D11Texture2D*)g_curBB)->GetDesc(&b);
    if (g_ovTex) { D3D11_TEXTURE2D_DESC a{}; g_ovTex->GetDesc(&a); if (a.Width != b.Width || a.Height != b.Height || a.Format != b.Format) { g_ovTex->Release(); g_ovTex = nullptr; } }
    if (!g_ovTex) {
        D3D11_TEXTURE2D_DESC d = b; d.BindFlags = 0; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT; d.MipLevels = 1; d.ArraySize = 1;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_ovTex))) { g_ovTex = nullptr; return; }
        Log("[overlay] copy texture %ux%u fmt=%u created", d.Width, d.Height, (unsigned)d.Format);
    }
    if (!g_ovZeroSRV) {
        D3D11_TEXTURE2D_DESC d{}; d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        uint16_t zero[4] = {}; D3D11_SUBRESOURCE_DATA init{ zero, 8, 8 };
        ID3D11Texture2D* z = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, &init, &z)) && z) { g_dev->CreateShaderResourceView(z, nullptr, &g_ovZeroSRV); z->Release(); }
    }
    g_origCopyRes(c, g_ovTex, (ID3D11Texture2D*)g_curBB);
    g_ovCaptured.store(true, std::memory_order_relaxed);
    g_ovCaptures.fetch_add(1, std::memory_order_relaxed);
}
static void HudLayerPre(ID3D11DeviceContext* c, bool indexed, UINT count, CtxScene* s, HudState& hs) {
    if (!g_hudLayerOn.load(std::memory_order_relaxed)) return;
    void* rt = s->rtTex.load(std::memory_order_relaxed);
    // run 13: the overlay path never had a reader (0 strips) and its capture was empty in gameplay
    // and menus - disabled, it only cost a per-draw SRV scan and a backbuffer clear.
    if (g_overlayEnabled && HudLayerActive()) {
        if (c == g_ctxImm) OverlayImmediate(c, indexed, s);
        OverlayStrip(c, s, indexed, hs);
    }
    if (c == g_ctxImm) {
        // learn the scene image from the final composite (indexed draw into the backbuffer, PS t0)
        if (indexed && g_curBB && rt == (void*)g_curBB) {
            ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
            if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); if (r) { g_sceneFinalRT.store(r, std::memory_order_relaxed); RememberSceneImage(r); r->Release(); } v->Release(); }
        }
        return;
    }
    if (!HudLayerActive()) return;
    if (!IsSceneImage(rt)) return;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm);
    BOOL blend = FALSE; if (bs) { D3D11_BLEND_DESC d{}; bs->GetDesc(&d); blend = d.RenderTarget[0].BlendEnable; bs->Release(); }
    ID3D11VertexShader* vsObj = nullptr; c->VSGetShader(&vsObj, nullptr, nullptr);
    void* vs = vsObj; if (vsObj) vsObj->Release();
    const bool dsvBound = s->rtDsv.load(std::memory_order_relaxed);
    const bool menu = g_menuInVr.load(std::memory_order_relaxed);
    // settled gameplay (no menu for 60+ frames): remember every shader that draws here
    if (!menu && g_hudFrameNo.load(std::memory_order_relaxed) > g_lastMenuFrameNo.load(std::memory_order_relaxed) + 60) LearnPlayVS(vs);
    const int menuOnlyMode = g_menuOnlyMode.load(std::memory_order_relaxed);
    const bool menuOnlyAny = menu && vs && g_playVSCount.load(std::memory_order_relaxed) > 0 && !IsPlayVS(vs);
    if (menuOnlyAny && menuOnlyMode == 0) { hs.skip = true; g_menuModelRedirects.fetch_add(1, std::memory_order_relaxed); return; }
    const bool menuOnly = menuOnlyAny && menuOnlyMode == 1;
    if (!blend) {
        // post passes write opaque; inside a menu, opaque sprites (menu backgrounds/panels) drawn with
        // the HUD's own vertex shaders, and menu-only shaders (3D menu models), go to the panel too
        if (!(menu && (IsHudVS(vs) || menuOnly))) { g_hudSkippedNoBlend.fetch_add(1, std::memory_order_relaxed); if (menu) NoteMenuMiss(vs, false, dsvBound); return; }
        if (menuOnly) g_menuModelRedirects.fetch_add(1, std::memory_order_relaxed);
        else g_menuOpaqueRedirects.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!dsvBound) LearnHudVS(vs);                                       // certainly 2D overlay
    else if (menuOnly) g_menuModelRedirects.fetch_add(1, std::memory_order_relaxed);
    else if (!IsHudVS(vs)) {                                                 // 3D transparents stay
        ID3D11DepthStencilState* ds = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&ds, &ref);
        BOOL depth = TRUE; if (ds) { D3D11_DEPTH_STENCIL_DESC d{}; ds->GetDesc(&d); depth = d.DepthEnable; ds->Release(); }
        if (depth) { g_hudSkippedDepth.fetch_add(1, std::memory_order_relaxed); if (menu) NoteMenuMiss(vs, true, true); return; }
    }
    // run 15: "RT >= 1280" also matched the map menu's own art (panel 100% clear) - falsified, off.
    const bool closing = g_hudFrameNo.load(std::memory_order_relaxed) <= g_lastMenuFrameNo.load(std::memory_order_relaxed) + 45;   // close animation tail
    if (g_worldPicRule && (g_worldFrozen.load(std::memory_order_relaxed) || menu || closing || GfdCam::EventRunning()) && count <= 6 && SamplesWorldPicture(c)) {   // [EVTWORLDPIC] 10-15: the conversation flash = this same flat scene-size copy drawn to the panel during events (7/7 onsets in the live trace)   // [WPNOTE] 10-07: only the full-screen quad (4 verts) is the world picture; map roads/Joker meshes merely leave it bound in t0
        hs.skip = true; g_worldPicSkips.fetch_add(1, std::memory_order_relaxed);
        WorldPicNote(c, count, vs, blend, GfdCam::ScreenTask());   // [WPNOTE]
        return;
    }
    if (g_worldFrozen.load(std::memory_order_relaxed) && g_menuCensusArmed.load(std::memory_order_relaxed)) MenuCensusRecord(c, indexed, vs, blend, dsvBound);
    g_hudCandidates.fetch_add(1, std::memory_order_relaxed);
    ID3D11RenderTargetView* hud = g_hudRTV.load(std::memory_order_acquire);
    if (!hud) {   // the render thread builds the target once it knows the game's view format
        ID3D11RenderTargetView* v = nullptr; c->OMGetRenderTargets(1, &v, nullptr);
        if (v) { D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d); g_hudRtvFmtWanted.store((uint32_t)d.Format, std::memory_order_relaxed); v->Release(); }
        return;
    }
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, hs.rtv, &hs.dsv);
    OrigOMSetRTs(c)(c, 1, &hud, hs.dsv);   // keep the game's depth/stencil (same size): stencil masks still apply
    if (GfdCam::EventRunning()) EventUiNote(c, count, vs, blend);   // [EVTUI] which draw is the letterbox
    if (GfdCam::EventRunning()) FlashTraceNote(c, count, vs, blend, dsvBound);   // [FLASHTRACE] log only
    if (void* dim = g_dimTex.load(std::memory_order_relaxed); false && dim && GfdCam::EventRunning()) {   // [CHOICEDIM] OFF 10-10: the texture it caught is also the text box background (text box went blank)
        ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v); bool isDim = false;
        if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release(); isDim = r == dim; if (r) r->Release(); }
        if (isDim) { OrigOMSetRTs(c)(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, hs.rtv, hs.dsv); for (auto& rv : hs.rtv) if (rv) { rv->Release(); rv = nullptr; } if (hs.dsv) { hs.dsv->Release(); hs.dsv = nullptr; }
            hs.skip = true; g_dimSkips.fetch_add(1, std::memory_order_relaxed); return; }
    }
    if (g_pxArmed.load(std::memory_order_relaxed)) {   // [PXPROBE] remember what this draw is; Post copies the probe pixels
        ID3D11ShaderResourceView* v = nullptr; c->PSGetShaderResources(0, 1, &v);
        if (v) { ID3D11Resource* r = nullptr; v->GetResource(&r); v->Release(); hs.pxT0 = r;
            if (r) { ID3D11Texture2D* t2 = nullptr; if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2) { D3D11_TEXTURE2D_DESC td{}; t2->GetDesc(&td); hs.pxTw = td.Width; hs.pxTh = td.Height; t2->Release(); } r->Release(); } }
        hs.pxVs = vs; hs.pxVerts = count; hs.pxBlend = (uint8_t)blend;
    }
    if (menu) UiCensus(c, hs.dsv != nullptr, false);   // [MENUCENSUS2]
    // [COVREPLAY] the game's own blend is kept (its alpha is a stencil it reads back); coverage is replayed on the field
    if (!menu) { hs.cov = 1; }   // [TWINKEY] every field/event panel draw (mask draws too) goes to the twin
    if (false) {   // [BARPRED] disabled: the dedicated letterbox callback owns removal; preserve every other UI draw
        ID3D11ShaderResourceView* v0 = nullptr; c->PSGetShaderResources(0, 1, &v0);
        hs.barCand = (v0 == nullptr); if (v0) v0->Release();
    }
    hs.redirect = true;
    g_hudRedirects.fetch_add(1, std::memory_order_relaxed);
    g_hudDrawnThisFrame.store(true, std::memory_order_relaxed);
}
// [UIALPHA] Run 17: P5R's UI blending does not build coverage in the alpha channel (the game ignores
// alpha), so on the panel anything not over an opaque backdrop was faint or partly missing (Joker's
// portrait in the map menu). Redirected UI draws get the same colour blend with alpha forced to
// standard "over" (ONE, INV_SRC_ALPHA, write enabled): alpha = coverage, colour premultiplied.
static SRWLOCK g_uiBlendLock = SRWLOCK_INIT;
static ID3D11BlendState* g_uiBlendFrom[64] = {};
static ID3D11BlendState* g_uiBlendTo[64] = {};
static int g_uiBlendCount = 0;
static ID3D11BlendState* UiCoverageBlend(ID3D11BlendState* from) {
    AcquireSRWLockShared(&g_uiBlendLock);
    for (int i = 0; i < g_uiBlendCount; ++i) if (g_uiBlendFrom[i] == from) { ID3D11BlendState* to = g_uiBlendTo[i]; ReleaseSRWLockShared(&g_uiBlendLock); return to; }
    ReleaseSRWLockShared(&g_uiBlendLock);
    D3D11_BLEND_DESC d{};
    if (from) from->GetDesc(&d);
    else { d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD; d.RenderTarget[0].RenderTargetWriteMask = 15; }
    auto& r = d.RenderTarget[0];
    // Run 18: white squares behind the date box / minimap during NPC chatter. Mask draws (no RGB
    // writes) and non-standard blends were also forced to write alpha, which made faint white HUD
    // art solid. Only plain "over" blending and opaque colour writes get coverage alpha; every other
    // draw keeps the game's own blend state (nullptr is cached for it).
    const bool writesColour = (r.RenderTargetWriteMask & (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE)) != 0;
    const bool standardOver = r.BlendEnable && r.BlendOp == D3D11_BLEND_OP_ADD && r.DestBlend == D3D11_BLEND_INV_SRC_ALPHA
                              && (r.SrcBlend == D3D11_BLEND_SRC_ALPHA || r.SrcBlend == D3D11_BLEND_ONE);
    if (!writesColour || (r.BlendEnable && !standardOver)) {
        AcquireSRWLockExclusive(&g_uiBlendLock);
        bool known = false; for (int i = 0; i < g_uiBlendCount; ++i) if (g_uiBlendFrom[i] == from) known = true;
        if (!known && g_uiBlendCount < 64) { if (from) from->AddRef(); g_uiBlendFrom[g_uiBlendCount] = from; g_uiBlendTo[g_uiBlendCount++] = nullptr; }
        ReleaseSRWLockExclusive(&g_uiBlendLock);
        return nullptr;
    }
    if (!r.BlendEnable) {   // opaque UI (menu panels): write colour as is, alpha = 1
        r.BlendEnable = TRUE; r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_ZERO; r.BlendOp = D3D11_BLEND_OP_ADD;
        r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else { r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; r.BlendOpAlpha = D3D11_BLEND_OP_ADD; }
    r.RenderTargetWriteMask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    d.IndependentBlendEnable = FALSE;
    ID3D11BlendState* to = nullptr;
    if (FAILED(g_dev->CreateBlendState(&d, &to)) || !to) return nullptr;
    AcquireSRWLockExclusive(&g_uiBlendLock);
    for (int i = 0; i < g_uiBlendCount; ++i) if (g_uiBlendFrom[i] == from) { ID3D11BlendState* have = g_uiBlendTo[i]; ReleaseSRWLockExclusive(&g_uiBlendLock); to->Release(); return have; }
    if (g_uiBlendCount < 64) { if (from) from->AddRef(); g_uiBlendFrom[g_uiBlendCount] = from; g_uiBlendTo[g_uiBlendCount++] = to; }
    else { ReleaseSRWLockExclusive(&g_uiBlendLock); to->Release(); return nullptr; }
    ReleaseSRWLockExclusive(&g_uiBlendLock);
    return to;
}
static void HudLayerPost(ID3D11DeviceContext* c, HudState& hs) {
    if (hs.blendSwapped) { c->OMSetBlendState(hs.blend, hs.blendFactor, hs.sampleMask); if (hs.blend) hs.blend->Release(); hs.blend = nullptr; hs.blendSwapped = false; }
    for (uint32_t k = 0; k < hs.srvCount; ++k) { c->PSSetShaderResources(hs.srvSlot[k], 1, &hs.srv[k]); hs.srv[k]->Release(); hs.srv[k] = nullptr; }
    hs.srvCount = 0;
    if (hs.dssSwapped) { c->OMSetDepthStencilState(hs.dss, hs.stencilRef); if (hs.dss) hs.dss->Release(); hs.dss = nullptr; hs.dssSwapped = false; }
    if (!hs.redirect) return;
    PxProbeAfterDraw(c, hs);
    OrigOMSetRTs(c)(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, hs.rtv, hs.dsv);
    for (auto& v : hs.rtv) if (v) { v->Release(); v = nullptr; }
    if (hs.dsv) { hs.dsv->Release(); hs.dsv = nullptr; }
    hs.redirect = false;
}
// Present thread: (re)build the HUD target to match the scene image.
static void HudLayerEnsureTarget() {
    void* fin = g_sceneFinalRT.load(std::memory_order_relaxed);
    const uint32_t fmt = g_hudRtvFmtWanted.load(std::memory_order_relaxed);
    if (!fin || !fmt || !g_dev) return;
    ID3D11Texture2D* scene = nullptr;
    if (FAILED(((ID3D11Resource*)fin)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&scene)) || !scene) return;
    D3D11_TEXTURE2D_DESC d{}; scene->GetDesc(&d); scene->Release();
    if (g_hudTex) {
        D3D11_TEXTURE2D_DESC o{}; g_hudTex->GetDesc(&o);
        if (o.Width == d.Width && o.Height == d.Height && g_hudRTV.load()) return;
        // size changed: retire the old target (a worker may still hold the view this frame)
        if (ID3D11RenderTargetView* old = g_hudRTV.exchange(nullptr)) g_hudGraveyard.push_back(old);
        g_hudGraveyard.push_back(g_hudTex); g_hudTex = nullptr;
    }
    d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_hudTex)) || !g_hudTex) { g_hudTex = nullptr; return; }
    D3D11_RENDER_TARGET_VIEW_DESC viewDesc{}; viewDesc.Format = (DXGI_FORMAT)fmt; viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_dev->CreateRenderTargetView(g_hudTex, &viewDesc, &rtv)) || !rtv) { g_hudTex->Release(); g_hudTex = nullptr; return; }
    PickDef(g_ctxImm, g_origDefClearRTV, g_origClearRTV)(g_ctxImm, rtv, kHudKey);   // [KEYCOVER]
    {   // [UICOVER] stencil the size of the HUD target
        if (ID3D11DepthStencilView* old = g_hudDSV.exchange(nullptr)) g_hudGraveyard.push_back(old);
        if (g_hudDepthTex) { g_hudGraveyard.push_back(g_hudDepthTex); g_hudDepthTex = nullptr; }
        D3D11_TEXTURE2D_DESC sd = d; sd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; sd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ID3D11DepthStencilView* dsv = nullptr;
        if (UiCoverInit() && SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &g_hudDepthTex)) && g_hudDepthTex && SUCCEEDED(g_dev->CreateDepthStencilView(g_hudDepthTex, nullptr, &dsv)) && dsv) {
            PickDef(g_ctxImm, g_origDefClearDSV, g_origClearDSV)(g_ctxImm, dsv, D3D11_CLEAR_STENCIL | D3D11_CLEAR_DEPTH, 1.f, 0);
            g_hudDSV.store(dsv, std::memory_order_release);
        }
    }
    {   // [COVREPLAY] coverage target the size of the HUD target
        if (ID3D11RenderTargetView* old = g_covRTV.exchange(nullptr)) g_hudGraveyard.push_back(old);
        if (g_covSRV) { g_hudGraveyard.push_back(g_covSRV); g_covSRV = nullptr; }
        if (g_covTex) { g_hudGraveyard.push_back(g_covTex); g_covTex = nullptr; }
        D3D11_TEXTURE2D_DESC cd = d; cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;   // [TWINKEY] same texture format as the panel
        ID3D11RenderTargetView* cr = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC tsv{}; tsv.Format = (DXGI_FORMAT)fmt; tsv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; tsv.Texture2D.MipLevels = 1;
        if (SUCCEEDED(g_dev->CreateTexture2D(&cd, nullptr, &g_covTex)) && g_covTex && SUCCEEDED(g_dev->CreateRenderTargetView(g_covTex, &viewDesc, &cr)) && cr
            && SUCCEEDED(g_dev->CreateShaderResourceView(g_covTex, &tsv, &g_covSRV))) {
            const float w[4] = { 1, 1, 1, 1 }; PickDef(g_ctxImm, g_origDefClearRTV, g_origClearRTV)(g_ctxImm, cr, w);
            g_covRTV.store(cr, std::memory_order_release);
        } else Log("[covreplay] coverage target FAILED");
    }
    g_hudRTV.store(rtv, std::memory_order_release);
    Log("[hudlayer] HUD target %ux%u tex fmt=%u view fmt=%u created (scene image %p)", d.Width, d.Height, (unsigned)d.Format, fmt, fin);
}
// Present thread, after the headset copy: next frame's HUD starts transparent.
static void HudLayerAfterSubmit() {
    if (ID3D11RenderTargetView* rtv = g_hudRTV.load(std::memory_order_acquire)) {
        PickDef(g_ctxImm, g_origDefClearRTV, g_origClearRTV)(g_ctxImm, rtv, kHudKey);   // [KEYCOVER] next frame's coverage key
    }
    if (ID3D11RenderTargetView* cr = g_covRTV.load(std::memory_order_acquire)) { const float w[4] = { 1, 1, 1, 1 }; PickDef(g_ctxImm, g_origDefClearRTV, g_origClearRTV)(g_ctxImm, cr, w); }   // [TWINKEY] white
    if (ID3D11DepthStencilView* dsv = g_hudDSV.load(std::memory_order_acquire))
        PickDef(g_ctxImm, g_origDefClearDSV, g_origClearDSV)(g_ctxImm, dsv, D3D11_CLEAR_STENCIL | D3D11_CLEAR_DEPTH, 1.f, 0);
}
static bool HudUsesMainCam(ID3D11DeviceContext* c) {
    void* want = g_mainCamBuf.load(std::memory_order_relaxed);
    if (!want) return false;
    ID3D11Buffer* b2 = nullptr; c->VSGetConstantBuffers(2, 1, &b2);
    bool ok = (b2 == want);
    if (b2) b2->Release();
    return ok;
}
static void HudPre(ID3D11DeviceContext* c, bool indexed, UINT count, HudState& hs) {
    hs.on = false;
    CtxScene* s = CtxSceneSlot(c);
    if (!s) return;
    HudLayerPre(c, indexed, count, s, hs);
    const uint32_t w = s->rtW.load(std::memory_order_relaxed), h = s->rtH.load(std::memory_order_relaxed);
    const bool dsv = s->rtDsv.load(std::memory_order_relaxed);
    const bool scan = g_hudScan.load(std::memory_order_relaxed);
    const float scale = g_hudScale.load(std::memory_order_relaxed);
    const bool candidate = w && w == g_bbW && h == g_bbH && !dsv && indexed;
    const bool cam = (scan || candidate) ? HudUsesMainCam(c) : false;
    const bool shrink = candidate && !cam && scale < 0.999f && !g_screenView.load(std::memory_order_relaxed);
    if (shrink) {
        hs.nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&hs.nvp, hs.vp);
        hs.nsc = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetScissorRects(&hs.nsc, hs.sc);
        if (hs.nvp) {
            const float cx = 0.5f * (float)w, cy = 0.5f * (float)h;
            D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
            for (UINT i = 0; i < hs.nvp; ++i) {
                vp[i] = hs.vp[i];
                vp[i].TopLeftX = cx + (hs.vp[i].TopLeftX - cx) * scale; vp[i].Width *= scale;
                vp[i].TopLeftY = cy + (hs.vp[i].TopLeftY - cy) * scale; vp[i].Height *= scale;
            }
            c->RSSetViewports(hs.nvp, vp);
            if (hs.nsc) {
                D3D11_RECT sc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
                for (UINT i = 0; i < hs.nsc; ++i) {
                    sc[i].left   = (LONG)floorf(cx + ((float)hs.sc[i].left   - cx) * scale);
                    sc[i].right  = (LONG)ceilf (cx + ((float)hs.sc[i].right  - cx) * scale);
                    sc[i].top    = (LONG)floorf(cy + ((float)hs.sc[i].top    - cy) * scale);
                    sc[i].bottom = (LONG)ceilf (cy + ((float)hs.sc[i].bottom - cy) * scale);
                }
                c->RSSetScissorRects(hs.nsc, sc);
            }
            hs.on = true;
            g_hudShrunk.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (scan && c == g_ctxImm) UiSeqRecord(c, indexed, count, s);
    if (scan) {
        HudEvt e{ (uint8_t)(s - g_ctxScene), (uint8_t)indexed, (uint8_t)dsv, (uint8_t)cam, (uint8_t)hs.on, count, w, h, s->rtFmt.load(std::memory_order_relaxed) };
        std::lock_guard<std::mutex> lk(g_hudMx);
        if (g_hudEvts.size() < 16384) g_hudEvts.push_back(e);
    }
}
static void HudPost(ID3D11DeviceContext* c, HudState& hs) {
    HudLayerPost(c, hs);
    if (!hs.on) return;
    c->RSSetViewports(hs.nvp, hs.vp);
    if (hs.nsc) c->RSSetScissorRects(hs.nsc, hs.sc);
}
static void HudLoadSettings() {
    static bool s_loaded = false;
    if (s_loaded || !CameraControls::iniPath[0]) return;
    s_loaded = true;
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"Hud", L"Scale", L"1", text, 32, CameraControls::iniPath);
    float v = (float)_wtof(text);
    g_hudScale.store((v >= 0.2f && v <= 1.f) ? v : 1.f);
    g_hudLayerOn.store(GetPrivateProfileIntW(L"Hud", L"Layer", 1, CameraControls::iniPath) != 0);
    if (!HudSettings::loaded) HudSettings::Load();
    { auto hv = HudSettings::Get();
      Log("[hudlayer] layer=%d width=%.2fm distance=%.2fm height=%+.2fm modes(date,top,objectives,centre,map,prompts)=%d%d%d%d%d%d",
          (int)g_hudLayerOn.load(), hv.widthM, hv.distanceM, hv.heightM, hv.mode[0], hv.mode[1], hv.mode[2], hv.mode[3], hv.mode[4], hv.mode[5]); }
    Log("[hud] scale=%.2f (1 = off)", g_hudScale.load());
}
static void HudDumpBmp(ID3D11Texture2D* tex, const char* name) {
    if (!tex || !g_dev || !g_ctxImm || !g_origCopyRes) return;
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS
        && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS) return;
    const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.SampleDesc.Count = 1; sd.SampleDesc.Quality = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    if (d.SampleDesc.Count != 1) return;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    g_origCopyRes(g_ctxImm, st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctxImm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const int ds = 4, W = (int)d.Width / ds, H = (int)d.Height / ds;
        std::vector<uint8_t> px((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) { const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * ds * m.RowPitch;
            for (int x = 0; x < W; ++x) { const uint8_t* p = src + x * ds * 4; uint8_t* q = &px[((size_t)y * W + x) * 4];
                q[0] = bgra ? p[2] : p[0]; q[1] = p[1]; q[2] = bgra ? p[0] : p[2]; q[3] = 255; } }
        g_ctxImm->Unmap(st, 0);
        char path[MAX_PATH] = {}, dir[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, MAX_PATH); if (char* sl = strrchr(dir, '\\')) sl[1] = 0;
        snprintf(path, sizeof(path), "%s%s", dir, name);
        WriteBmpRGBA(path, px.data(), W, H);
    }
    st->Release();
}
// [HUDDUMP] colour + alpha images of a texture and a one-line alpha census.
static void HudDumpAlpha(ID3D11Texture2D* tex, const char* rgbName, const char* alphaName) {
    if (!tex || !g_dev || !g_ctxImm || !g_origCopyRes) return;
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return;
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &st)) || !st) return;
    g_origCopyRes(g_ctxImm, st, tex);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g_ctxImm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        const int ds = 4, W = (int)d.Width / ds, H = (int)d.Height / ds;
        std::vector<uint8_t> rgb((size_t)W * H * 4), alpha((size_t)W * H * 4);
        uint64_t solid = 0, partial = 0, clear = 0, leak = 0, white = 0;
        for (int y = 0; y < H; ++y) { const uint8_t* src = (const uint8_t*)m.pData + (size_t)y * ds * m.RowPitch;
            for (int x = 0; x < W; ++x) { const uint8_t* p = src + x * ds * 4; size_t o = ((size_t)y * W + x) * 4;
                rgb[o] = p[0]; rgb[o+1] = p[1]; rgb[o+2] = p[2]; rgb[o+3] = 255;
                alpha[o] = alpha[o+1] = alpha[o+2] = p[3]; alpha[o+3] = 255;
                if (p[3] >= 250) ++solid; else if (p[3] > 5) ++partial; else { ++clear; if (p[0] + p[1] + p[2] > 60) ++leak; }
                if (p[0] > 230 && p[1] > 230 && p[2] > 230) ++white; } }
        g_ctxImm->Unmap(st, 0);
        const double n = (double)W * H;
        Log("[huddump] %s %ux%u fmt=%u: solid=%.1f%% partial=%.1f%% clear=%.1f%% colour-in-clear(additive leak)=%.1f%% white=%.1f%%",
            rgbName, d.Width, d.Height, (unsigned)d.Format, 100 * solid / n, 100 * partial / n, 100 * clear / n, 100 * leak / n, 100 * white / n);
        char dir[MAX_PATH] = {}, path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, dir, MAX_PATH); if (char* sl = strrchr(dir, '\\')) sl[1] = 0;
        snprintf(path, sizeof(path), "%s%s", dir, rgbName); WriteBmpRGBA(path, rgb.data(), W, H);
        snprintf(path, sizeof(path), "%s%s", dir, alphaName); WriteBmpRGBA(path, alpha.data(), W, H);
    }
    st->Release();
}
// Present thread: summarize last frame's scan, arm the next one.
static void HudOnPresent(uint64_t f, IDXGISwapChain* sc) {
    HudLoadSettings();
    const bool scanned = g_hudScan.exchange(false, std::memory_order_relaxed);
    UiSeqPresent(f, sc, scanned);
    if (scanned) {
        std::vector<HudEvt> ev;
        { std::lock_guard<std::mutex> lk(g_hudMx); ev.swap(g_hudEvts); }
        Log("[hudscan] f%llu %zu draws, scale=%.2f (bb %ux%u) — runs per context: ctx w x h fmt dsv cam idx shrunk : draws (min..max count)",
            (unsigned long long)f, ev.size(), g_hudScale.load(), g_bbW, g_bbH);
        int lines = 0;
        for (int ctx = 0; ctx < 16 && lines < 120; ++ctx) {
            const HudEvt* run = nullptr; uint32_t n = 0, lo = 0, hi = 0;
            auto flush = [&]() { if (run && lines < 120) { ++lines;
                Log("[hudscan]   ctx%d %ux%u fmt=%u dsv=%u cam=%u idx=%u shrunk=%u : %u (%u..%u)", ctx, run->w, run->h, run->fmt, run->dsv, run->cam, run->indexed, run->shrunk, n, lo, hi); } };
            for (const auto& e : ev) {
                if (e.ctx != ctx) continue;
                if (run && run->w == e.w && run->h == e.h && run->fmt == e.fmt && run->dsv == e.dsv && run->cam == e.cam && run->indexed == e.indexed && run->shrunk == e.shrunk) {
                    ++n; lo = (std::min)(lo, e.count); hi = (std::max)(hi, e.count); continue; }
                flush(); run = &e; n = 1; lo = hi = e.count;
            }
            flush();
        }
        uint32_t immNonIdx = 0; for (const auto& e : ev) if (e.ctx == 0 && !e.indexed) ++immNonIdx;
        if (sc) { ID3D11Texture2D* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) { HudDumpBmp(bb, "p5r_hudscan.bmp"); bb->Release(); } }
        if (g_hudTex) HudDumpBmp(g_hudTex, "p5r_hudlayer.bmp");
        // next scan frame takes the stage snapshots (once), if this frame had a real HUD
        if (!g_uiSnapDone && immNonIdx >= 500) UiSeqArmSnapshots(sc, immNonIdx);
    }
    // [HUDLAYER] hand this frame's HUD to the next headset submit
    HudLayerEnsureTarget();
    // [KNOCKOUT] removed 10-09: switching task render callbacks off hung the game (stall at the end of the sweep)
    if (g_hudTex) UiCoverFixup(g_hudTex, g_hudRTV.load(std::memory_order_acquire), g_menuInVr.load(std::memory_order_relaxed));   // [KEYCOVER] once, before the dump: dumps = what is submitted
    if (g_hudDumpReq.load(std::memory_order_relaxed) && g_hudTex) {
        const bool models = g_menuModelsOn.load();
        Log("[huddump] f%llu Numpad 2: menu=%d models-to-panel=%d", (unsigned long long)f, (int)g_menuInVr.load(), (int)models);
        const int mode = g_menuOnlyMode.load();
        char rgbName[64], alphaName[64];
        snprintf(rgbName, sizeof(rgbName), "p5r_panel_mode%d.bmp", mode); snprintf(alphaName, sizeof(alphaName), "p5r_panel_alpha_mode%d.bmp", mode);
        if (g_dumpLabel[0]) { snprintf(rgbName, sizeof(rgbName), "p5r_dump_%s_panel.bmp", g_dumpLabel); snprintf(alphaName, sizeof(alphaName), "p5r_dump_%s_panel_alpha.bmp", g_dumpLabel); }
        HudDumpAlpha(g_hudTex, rgbName, alphaName);
        if (g_ovTex) { snprintf(rgbName, sizeof(rgbName), "p5r_overlay_mode%d.bmp", mode); snprintf(alphaName, sizeof(alphaName), "p5r_overlay_alpha_mode%d.bmp", mode); HudDumpAlpha(g_ovTex, rgbName, alphaName); }
    }
    {
        const auto hv = HudSettings::Get();
        P5RXr::HudLayout layout;
        layout.width = hv.widthM; layout.distance = hv.distanceM; layout.height = hv.heightM;
        static bool s_cmmLayout = false;
        g_dimCapNow.store(GfdCam::EventRunning() ? hv.evDim : 1.f, std::memory_order_relaxed);   // [EVTDIM] next frame's cap
        {   // [EVENTUI] 10-09: events (confidants...) get their own placement, docked fresh in front at each event start
            // 10-09: confidant scenes and other cutscenes are placed separately
            s_cmmLayout = false;
            static int s_kindWas = 0; const bool ev = GfdCam::EventRunning() && !g_menuInVr.load(std::memory_order_relaxed);
            const int kind = !ev ? 0 : (GfdCam::ConfidantRunning() ? 2 : 1);
            if (kind == 2) { layout.width = hv.cmWidthM; layout.distance = hv.cmDistanceM; layout.height = hv.cmHeightM; s_cmmLayout = true; }
            else if (kind == 1) { layout.width = hv.evWidthM; layout.distance = hv.evDistanceM; layout.height = hv.evHeightM; }
            if (kind != s_kindWas) { HudSettings::redock = true; s_kindWas = kind; }
        }
        layout.count = HudSettings::kRegions;
        // [MENUVR] a menu is the whole panel, docked in front of where the player looked when it opened
        const bool menu = g_menuInVr.load(std::memory_order_relaxed);
        g_hudFrameNo.store(f, std::memory_order_relaxed);
        if (menu || g_screenView.load(std::memory_order_relaxed)) g_lastMenuFrameNo.store(f, std::memory_order_relaxed);
        static bool s_menuWas = false;
        if (menu && !s_menuWas) HudSettings::redock = true;
        if (menu != s_menuWas) Log("[menuvr] f%llu menu %s", (unsigned long long)f, menu ? "OPEN on the docked panel (world stays VR)" : "closed");
        {   // [MENUSEQ] what the headset gets as a menu opens and while it closes (flash / lingering box)
            static char s_task[48] = "menu";
            if (menu && !s_menuWas) { const char* t = GfdCam::ScreenTask(); snprintf(s_task, sizeof(s_task), "%s", t ? t : "menu");
                char l[64]; snprintf(l, sizeof(l), "%s_open", s_task); GfdCam::ScheduleUiDump(f + 45, l); }
            {   // [SAVESEQ] the screen task switching to/from Save: every 8 frames for ~1/2 s (white flash)
                static char s_taskWas[48] = "";
                const char* t = GfdCam::ScreenTask(); const char* now = t ? t : "";
                if (strcmp(now, s_taskWas) && (!strcmp(now, "save") || !strcmp(s_taskWas, "save"))) {
                    static int s_seq = 0; ++s_seq;
                    for (int i = 0; i < 5; ++i) { char l[64]; snprintf(l, sizeof(l), "save%d_%s_%02d", s_seq, !strcmp(now, "save") ? "in" : "out", i * 8); GfdCam::ScheduleUiDump(f + i * 8, l); }
                }
                snprintf(s_taskWas, sizeof(s_taskWas), "%s", now);
            }
            if (!menu && s_menuWas) { const int at[3] = { 0, 15, 40 };
                for (int i = 0; i < 3; ++i) { char l[64]; snprintf(l, sizeof(l), "%s_close_%d", s_task, at[i]); GfdCam::ScheduleUiDump(f + at[i], l); } }
        }
        s_menuWas = menu;
        for (int i = 0; i < HudSettings::kRegions; ++i) {
            const auto& r = HudSettings::kRegion[i];
            layout.region[i] = { r.u0, r.v0, r.u1, r.v1, menu ? (int)HudSettings::Dock : hv.mode[i] };
        }
        if (s_cmmLayout) {   // [CMMREGIONS] confidant: top-left (date) on its own; text box side + right column (Joker's
            // choice portrait spans the full height) at the bottom height - always the same parts, nothing jumps
            // [CMMPARTS] 10-11: four fixed parts, each with Top / with Bottom / hidden (player's choice, nothing switches by itself)
            const float xs = hv.cmRightX, ys = hv.cmSplit;
            const float rc[4][4] = { { 0.f, 0.f, xs, ys }, { xs, 0.f, 1.f, ys }, { 0.f, ys, xs, 1.f }, { xs, ys, 1.f, 1.f } };
            layout.count = 4;
            for (int i = 0; i < 4; ++i) {
                const int a = hv.cmPart[i];
                layout.region[i] = { rc[i][0], rc[i][1], rc[i][2], rc[i][3], a == 2 ? 2 : 1, a == 0 ? hv.cmTopY : a == 1 ? hv.cmBottomY : 0.f };
            }
        }
        layout.redock = HudSettings::redock.exchange(false);
        {   // [MENUSOLID] full-screen menus get a solid black-backed panel while the world behind them is frozen
            // (ends the moment the world is live again: no black rectangle lingering through the close
            // animation). The IM phone is an overlay on the field: no backing, its own (fixed) coverage.
            const char* task = GfdCam::ScreenTask();
            const bool im = task && !strcmp(task, "fld_square_menu");   // 10-07 dump: the IM is fld_square_menu
            // 10-07: not while the world is live again (shop goodbye dialog sat on a black box)
            layout.opaque = menu && (GfdCam::SolidTask(task)   // [SOLIDPANEL] save / network page / loading: always black-backed
                || (!im && g_worldFrozen.load(std::memory_order_relaxed) && !g_worldLiveRaw.load(std::memory_order_relaxed)));
        }
        if (menu && (f % 120) == 0) {
            std::lock_guard<std::mutex> lk(g_menuMissMx);
            Log("[menuvr] f%llu opaque menu sprites to panel=%u, menu-only (models) to panel=%u, gameplay shaders known=%u; draws into the scene image NOT moved (vs blend dsv : count):",
                (unsigned long long)f, g_menuOpaqueRedirects.exchange(0), g_menuModelRedirects.exchange(0), g_playVSCount.load());
            for (int i = 0; i < g_menuMissCount; ++i) Log("[menuvr]   vs=%p%s%s blend=%u dsv=%u : %u", g_menuMiss[i].vs, IsHudVS(g_menuMiss[i].vs) ? "(HUD)" : "",
                IsPlayVS(g_menuMiss[i].vs) ? "(gameplay)" : "", g_menuMiss[i].blend, g_menuMiss[i].dsv, g_menuMiss[i].n);
            {   std::lock_guard<std::mutex> lk2(g_uiGrpMx);
                Log("[menucensus2] f%llu task=%s UI draws moved to the panel in 120f, by game blend (en src dst op | srcA dstA opA | mask dsv covered : count):",
                    (unsigned long long)f, GfdCam::ScreenTask() ? GfdCam::ScreenTask() : "?");
                for (int i = 0; i < g_uiGrpN; ++i) { const uint32_t* k = g_uiGrp[i].key;
                    Log("[menucensus2]   %u %u %u %u | %u %u %u | %u %u %u : %u", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], g_uiGrp[i].n); }
                g_uiGrpN = 0; }
            g_menuMissCount = 0;
        }
        if (g_hudDrawnThisFrame.exchange(false, std::memory_order_relaxed) && HudLayerActive() && g_hudTex) P5RXr::SetHud(g_hudTex, &layout);
        else P5RXr::SetHud(nullptr, &layout);
        // the overlay goes to the panel only while its reader is being stripped (never shown twice)
        const bool readerLive = f < g_ovLastStripFrame.load(std::memory_order_relaxed) + 120;
        P5RXr::SetOverlay(g_ovCaptured.exchange(false, std::memory_order_relaxed) && HudLayerActive() && readerLive && g_ovTex ? g_ovTex : nullptr);
        g_ovClearedThisFrame.store(false, std::memory_order_relaxed);
        if ((f % 600) == 0 && !menu) {   // [COVCENSUS] field UI draws by game blend and the coverage kind given
            std::lock_guard<std::mutex> lk(g_covGrpMx);
            Log("[covcensus] f%llu field UI draws in 600f by game blend (en src dst op | srcA dstA opA | mask | kind 0none 1over 2solid 3max : count):", (unsigned long long)f);
            for (int i = 0; i < g_covGrpN; ++i) { const uint32_t* k = g_covGrp[i].key;
                Log("[covcensus]   %u %u %u %u | %u %u %u | %u | %u : %u", k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], g_covGrp[i].n); }
            g_covGrpN = 0;
        }
        if ((f % 600) == 0) Log("[keycover] f%llu coverage passes in 600f=%u (menu=%d) field draws replayed for coverage=%u", (unsigned long long)f, g_keyCoverRuns.exchange(0), (int)menu, g_covReplays.exchange(0));
        {   // [PXPROBE] arm one frame during events (6 times); read the strip back at the next Present
            static int s_px = 0; static bool s_reading = false;
            if (s_reading) {
                g_pxArmed.store(false); s_reading = false;
                const uint32_t n = (std::min)(g_pxN.load(), 1024u);
                D3D11_TEXTURE2D_DESC sd{}; g_pxStrip->GetDesc(&sd); sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Texture2D* st = nullptr;
                if (SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
                    g_ctxImm->CopyResource(st, g_pxStrip);
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(g_ctxImm->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                        Log("[pxprobe] f%llu %u panel draws; changes of the top-bar / bottom-left-bar pixel (RGBA):", (unsigned long long)f, n);
                        uint32_t pt = 0x01020304, pb = 0x01020304; int logged = 0;
                        for (uint32_t i = 0; i < n && logged < 60; ++i) {
                            const uint32_t t = ((const uint32_t*)m.pData)[i], b = ((const uint32_t*)((const uint8_t*)m.pData + m.RowPitch))[i];
                            if (t != pt || b != pb) { ++logged;
                                Log("[pxprobe]   draw %u t0=%p (%ux%u) verts=%u vs=%p blend=%u : top %08X -> %08X  portrait %08X -> %08X", i, g_pxMeta[i].t0, g_pxMeta[i].tw, g_pxMeta[i].th, g_pxMeta[i].verts, g_pxMeta[i].vs, g_pxMeta[i].blend, pt, t, pb, b);
                                // [CHOICEDIM] textured quad turning the mid-left pixel near black = the dim
                                const uint32_t rr = b & 0xFF, gg = (b >> 8) & 0xFF, bb2 = (b >> 16) & 0xFF;
                                if (b != pb && g_pxMeta[i].t0 && g_pxMeta[i].verts <= 6 && rr <= 8 && gg <= 8 && bb2 <= 8 && g_dimTex.load() != g_pxMeta[i].t0) {
                                    Log("[choicedim] candidate dim texture %p (draw %u) - not skipped (shared with the text box)", g_pxMeta[i].t0, i); } }
                            pt = t; pb = b;
                        }
                        g_ctxImm->Unmap(st, 0);
                    }
                    st->Release();
                }
            } else if (false && GfdCam::CutInRunning() && s_px < 40 && (f % 60) == 30 && g_hudTex && [&] {   // 10-09: the choice dim; re-probe only when it is no longer being caught
                static uint32_t s_seen = 0; static uint64_t s_lastCatch = 0; const uint32_t sk = g_dimSkips.load();
                if (sk != s_seen) { s_seen = sk; s_lastCatch = f; }
                return !g_dimTex.load() || f > s_lastCatch + 120; }()) {
                if (!g_pxStrip) { D3D11_TEXTURE2D_DESC hd{}; g_hudTex->GetDesc(&hd); D3D11_TEXTURE2D_DESC sd{}; sd.Width = 1024; sd.Height = 2; sd.MipLevels = sd.ArraySize = 1; sd.Format = hd.Format; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_DEFAULT;
                    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_pxStrip))) g_pxStrip = nullptr; }
                if (g_pxStrip) { ++s_px; g_pxN.store(0); g_pxArmed.store(true); s_reading = true; }
            }
        }
        if ((f % 300) == 0) {   // [EVTUI] log + save each event panel texture once (present thread)
            std::lock_guard<std::mutex> lk(g_evtUiMx);
            for (size_t i = 0; i < g_evtUi.size(); ++i) {
                auto& e = g_evtUi[i]; if (e.dumped) continue; e.dumped = true;
                Log("[evtui] f%llu #%zu t0=%p %ux%u fmt=%u verts=%u vs=%p blend=%u draws=%u", (unsigned long long)f, i, (void*)e.tx, e.w, e.h, e.fmt, e.count, e.vs, e.blend, e.n);
                const bool rgba8 = e.fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || e.fmt == DXGI_FORMAT_R8G8B8A8_UNORM || e.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                    || e.fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || e.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || e.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                if (e.tx && rgba8 && e.w >= 16 && e.h >= 16) { char a[64], b[64]; snprintf(a, sizeof(a), "p5r_evtui_%02zu.bmp", i); snprintf(b, sizeof(b), "p5r_evtui_%02zu_alpha.bmp", i); HudDumpAlpha(e.tx, a, b); }
            }
        }
        if ((f % 120) == 0) {   // [WPNOTE] log + save each dropped picture once (present thread)
            std::lock_guard<std::mutex> lk(g_wpMx);
            for (size_t i = 0; i < g_wpNotes.size(); ++i) {
                auto& w = g_wpNotes[i];
                if (w.dumped) continue;
                w.dumped = true;
                Log("[wpnote] f%llu #%zu task=%s t0=%p %ux%u fmt=%u verts=%u vs=%p blend=%u draws-so-far=%u", (unsigned long long)f, i, w.task, (void*)w.tx, w.w, w.h, w.fmt, w.count, w.vs, w.blend, w.n);
                const bool rgba8 = w.fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS || w.fmt == DXGI_FORMAT_R8G8B8A8_UNORM || w.fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                    || w.fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS || w.fmt == DXGI_FORMAT_B8G8R8A8_UNORM || w.fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                if (rgba8) { char a[64], b[64]; snprintf(a, sizeof(a), "p5r_wp_%02zu_%s.bmp", i, w.task); snprintf(b, sizeof(b), "p5r_wp_%02zu_%s_alpha.bmp", i, w.task); HudDumpAlpha(w.tx, a, b); }
            }
        }
        if ((f % 600) == 0 && g_barTests.load()) Log("[barpred] f%llu untextured event draws tested against the middle of the screen in 600f=%u (bar-only pieces skipped on the GPU)", (unsigned long long)f, g_barTests.exchange(0));
        if ((f % 600) == 0 && g_dimSkips.load()) Log("[choicedim] f%llu choice-dim draws skipped (no peek; count kept)", (unsigned long long)f);
        if ((f % 300) == 0 && g_worldPicSkips.load()) Log("[worldpic] f%llu flat world-picture draws dropped in 300f=%u", (unsigned long long)f, g_worldPicSkips.exchange(0));
        if ((f % 300) == 0) Log("[overlay] f%llu captures=%u reader-strips=%u reader-live=%d layer=%p menu-only(%s)=%u",
            (unsigned long long)f, g_ovCaptures.exchange(0), g_ovStrips.exchange(0), (int)readerLive, g_overlayLayerRT.load(),
            g_menuOnlyMode.load() == 0 ? "removed" : g_menuOnlyMode.load() == 1 ? "to panel" : "left", g_menuModelRedirects.load());
    }
    if ((f % 300) == 0) Log("[hudlayer] f%llu active=%d xr=%d screen=%d redirected=%u candidates=%u opaque-skipped=%u depth-skipped=%u hudVS=%u scene=%p viewFmt=%u", (unsigned long long)f, (int)HudLayerActive(),
        (int)g_hudXrLive.load(), (int)g_screenView.load(), g_hudRedirects.exchange(0), g_hudCandidates.exchange(0), g_hudSkippedNoBlend.exchange(0), g_hudSkippedDepth.exchange(0), g_hudVSCount.load(), g_sceneFinalRT.load(), g_hudRtvFmtWanted.load());
    if ((f % 300) == 0) Log("[hud] f%llu shrunk draws in 300f=%u scale=%.2f screen=%d", (unsigned long long)f, g_hudShrunk.exchange(0), g_hudScale.load(), (int)g_screenView.load());
    // perf: periodic draw scan + 4K picture dumps off
}
