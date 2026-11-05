// [FREEZE] Measured (run 13): while any game menu is open (Tab, Save, Log, IM) P5R stops rendering the
// live 3D world (right-eye camera draws fall from ~2000-4000/frame to ~30) and shows a still picture
// of it instead - that picture is the "mono" world. While the world is not live, keep showing the
// last LIVE stereo frame, submitted with the pose and projection it was rendered with, so the world
// stays true 3D behind the menu panel. Numpad 3 toggles it.
static std::atomic<bool> g_freezeOn{true};
// [FREEZETEST] ME1 lesson (me1-vr-aer-flicker-was-fps-cap): before more timing fixes, split SPATIAL from
// TEMPORAL. Numpad - holds the current stereo frame and keeps submitting it with the pose and lens it
// was rendered with; the compositor then reprojects ONE still image to the moving head. Head-turn
// jitter on the held image = the submit/compositor side (pose, lens, edges); a held image that is
// rock steady = the jitter comes from how new frames arrive (timing, latency, content).
static std::atomic<bool> g_freezeTest{false};
static ID3D11Texture2D* g_frzL = nullptr;
static ID3D11Texture2D* g_frzR = nullptr;
static P5RXr::Pose g_frzPose{};
static bool g_frzPoseValid = false, g_frzValid = false;
static float g_frzM00 = 0.f, g_frzM11 = 0.f;
static int g_frzNotLive = 0, g_frzEpisodes = 0, g_frzFrames = 0;
static uint32_t g_frzLastSwaps = 0;
// [FREEZEHOLD] 10-07 run: the world reads "live" a few frames BEFORE the menu closes, and the game's
// picture at that moment is not the world (map menu: a solid cyan screen) = the flash on close.
// Once frozen, keep the frozen frame until the menu is closed AND the world has been live for
// several frames in a row; never capture a new frozen frame while a menu is up.
static int g_frzLiveRun = 0;
// [BLACKWORLD] 10-08: save / loading / network page in VR: intended behavior is black over the WHOLE view, not
// just behind the panel. The world eyes are replaced by a black image (the panel stays in VR).
static std::atomic<bool> g_blackWorld{false};
static ID3D11Texture2D* g_blackTex = nullptr;
static void SubmitWorldEyes(ID3D11Texture2D* l, ID3D11Texture2D* r) {
    if (g_blackWorld.load(std::memory_order_relaxed) && l && g_dev) {
        D3D11_TEXTURE2D_DESC d{}; l->GetDesc(&d);
        if (g_blackTex) { D3D11_TEXTURE2D_DESC b{}; g_blackTex->GetDesc(&b); if (b.Width != d.Width || b.Height != d.Height || b.Format != d.Format) { g_blackTex->Release(); g_blackTex = nullptr; } }
        if (!g_blackTex) {
            D3D11_TEXTURE2D_DESC b = d; b.MipLevels = 1; b.ArraySize = 1; b.Usage = D3D11_USAGE_DEFAULT; b.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; b.CPUAccessFlags = 0; b.MiscFlags = 0;
            if (b.SampleDesc.Count != 1 || FAILED(g_dev->CreateTexture2D(&b, nullptr, &g_blackTex))) g_blackTex = nullptr;
            if (g_blackTex) {   // cleared once on the GPU (typeless formats get their UNORM/float view)
                DXGI_FORMAT vf = d.Format;
                if (vf == DXGI_FORMAT_R8G8B8A8_TYPELESS) vf = DXGI_FORMAT_R8G8B8A8_UNORM; else if (vf == DXGI_FORMAT_B8G8R8A8_TYPELESS) vf = DXGI_FORMAT_B8G8R8A8_UNORM;
                else if (vf == DXGI_FORMAT_R10G10B10A2_TYPELESS) vf = DXGI_FORMAT_R10G10B10A2_UNORM; else if (vf == DXGI_FORMAT_R16G16B16A16_TYPELESS) vf = DXGI_FORMAT_R16G16B16A16_FLOAT;
                D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = vf; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* rtv = nullptr; const float black[4] = { 0, 0, 0, 1 };
                if (SUCCEEDED(g_dev->CreateRenderTargetView(g_blackTex, &rd, &rtv)) && rtv) { PickDef(g_ctxImm, g_origDefClearRTV, g_origClearRTV)(g_ctxImm, rtv, black); rtv->Release(); }
                else { g_blackTex->Release(); g_blackTex = nullptr; }
            }
            Log("[blackworld] black eye image %ux%u fmt=%u %s", d.Width, d.Height, (unsigned)d.Format, g_blackTex ? "ready" : "FAILED");
        }
        if (g_blackTex) { P5RXr::SubmitEyes(g_blackTex, g_blackTex); return; }
    }
    P5RXr::SubmitEyes(l, r);
}
static constexpr int kFrzLiveNeeded = 8;

static bool FreezeEnsure(ID3D11Texture2D*& dst, ID3D11Texture2D* src) {
    D3D11_TEXTURE2D_DESC s{}; src->GetDesc(&s);
    if (dst) { D3D11_TEXTURE2D_DESC d{}; dst->GetDesc(&d); if (d.Width == s.Width && d.Height == s.Height && d.Format == s.Format) return true; dst->Release(); dst = nullptr; }
    s.BindFlags = D3D11_BIND_SHADER_RESOURCE; s.MiscFlags = 0; s.CPUAccessFlags = 0; s.Usage = D3D11_USAGE_DEFAULT; s.MipLevels = 1; s.ArraySize = 1;
    if (s.SampleDesc.Count != 1) return false;
    return SUCCEEDED(g_dev->CreateTexture2D(&s, nullptr, &dst)) && dst;
}
// Present thread. Returns true when it submitted the frozen frame (caller must not submit).
static bool FreezeSubmit(ID3D11Texture2D* left, ID3D11Texture2D* right) {
    if (!g_mirrorActive || left == right) { g_frzValid = false; g_frzNotLive = 0; g_frzFrames = 0; g_worldFrozen.store(false); return false; }   // stereo not running: never show a stale frame
    const uint32_t cur = g_mirCamSwaps;
    const uint32_t swaps = cur >= g_frzLastSwaps ? cur - g_frzLastSwaps : cur;   // counter resets every 120 frames
    g_frzLastSwaps = cur;
    const bool live = swaps > 120;   // live world: ~2000-4000/frame; menus ~30
    g_worldLiveRaw.store(live, std::memory_order_relaxed);
    const bool hold = g_freezeTest.load(std::memory_order_relaxed) && g_frzValid;
    const bool menuUp = g_menuInVr.load(std::memory_order_relaxed);
    if (live && !hold && g_frzFrames && g_frzValid && g_freezeOn.load(std::memory_order_relaxed)
        && ++g_frzLiveRun < (menuUp ? 30 : kFrzLiveNeeded)) {   // 10-08: live 30 frames in a row = the world is back even if a "menu" claims otherwise
        goto submitFrozen;   // [FREEZEHOLD] not settled yet: keep the frozen world
    }
    if (live && !hold) {
        if (g_frzFrames) Log("[freeze] world live again after %d frozen frames (held until menu closed + %d live frames)", g_frzFrames, kFrzLiveNeeded);
        g_frzLiveRun = 0;
        if (g_menuCensusArmed.exchange(false)) MenuCensusDump(g_frzEpisodes);
        g_frzNotLive = 0; g_frzFrames = 0; g_worldFrozen.store(false);
        if (!g_freezeOn.load(std::memory_order_relaxed) || !g_origCopyRes || !g_ctxImm) return false;
        if (menuUp) return false;   // [FREEZEHOLD] a menu's picture is never kept as "the world"
        if (FreezeEnsure(g_frzL, left) && FreezeEnsure(g_frzR, right)) {
            g_origCopyRes(g_ctxImm, g_frzL, left);
            g_origCopyRes(g_ctxImm, g_frzR, right);
            g_frzPoseValid = P5RXr::GetRenderPose(&g_frzPose);
            P5RXr::GetGameProjection(&g_frzM00, &g_frzM11);
            g_frzValid = true;
        }
        return false;
    }
    g_frzLiveRun = 0;
    if (!hold && (!g_freezeOn.load(std::memory_order_relaxed) || !g_frzValid || ++g_frzNotLive < 3)) { g_worldFrozen.store(false); return false; }
submitFrozen:
    g_worldFrozen.store(true);
    if (g_frzFrames++ == 0) {
        Log("[freeze] world not live (right-eye camera draws %u/frame): showing the last live stereo frame", swaps);
        // perf: menu census off (identified the world picture in run 16)
    }
    if (g_frzFrames == 61 && g_menuCensusArmed.exchange(false)) MenuCensusDump(g_frzEpisodes);
    P5RXr::SetRenderPose(g_frzPoseValid ? &g_frzPose : nullptr);
    P5RXr::OverrideSubmitProjection(g_frzM00, g_frzM11);
    auto eyes = CameraControls::SelectEyes(g_frzL, g_frzR, CameraControls::Get());
    SubmitWorldEyes(eyes.left, eyes.right);
    // evidence for the first episodes: the flat picture the game shows, and the panel
    if (false && g_frzFrames == 62 && g_frzEpisodes < 3) {   // perf: freeze picture dumps off
        char a[64], b[64];
        snprintf(a, sizeof(a), "p5r_freeze%d_game.bmp", g_frzEpisodes); snprintf(b, sizeof(b), "p5r_freeze%d_game_alpha.bmp", g_frzEpisodes);
        HudDumpAlpha(left, a, b);
        if (g_hudTex) { snprintf(a, sizeof(a), "p5r_freeze%d_panel.bmp", g_frzEpisodes); snprintf(b, sizeof(b), "p5r_freeze%d_panel_alpha.bmp", g_frzEpisodes); HudDumpAlpha(g_hudTex, a, b); }
        ++g_frzEpisodes;
    }
    return true;
}
