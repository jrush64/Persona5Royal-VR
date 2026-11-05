// [HEADTAG] Head-tracking smoothness, ME1 lesson applied (Docs/HEAD_TRACKING_SMOOTHNESS_FIX.md): the
// compositor reprojects every submitted image by the pose it is TAGGED with, so the tag must be the
// pose the image was RENDERED with. ME1 bug 1: the tag was a newer pose than the render (drag on turn
// start); its fix was a hand-dialed "N presents back" delay. P5R can do it exactly: the head pose is
// applied at the camera upload, which is recorded into a deferred command list; the render thread
// executes that list right before Present. So the pose travels with its list (upload -> Finish ->
// Execute) and the submitted frame is tagged with the pose of the camera list it actually executed.
// Old behaviour (tag = the most recent upload on any thread, possibly the NEXT frame's) is kept on
// Numpad * for A/B. Every 300 frames [HEADTAG] logs: how often/how far the old tag differed from the
// executed pose, how old the rendered pose is, and the present cadence in headset display periods
// (ME1 bug 4: a cadence not locked to the refresh reads as judder during motion).
static std::atomic<bool> g_exactTag{true};
static std::mutex g_tagMx;
// [MIXPOSE] Freeze test (e7e163b9): one held frame reprojects rock steady, live frames shake -> the
// cause is in how frames are produced. Candidate: one frame's camera uploads (several per frame, on
// worker threads) straddle a head locate, so parts of the same picture use different head poses.
struct PoseSpan { P5RXr::Pose first{}, last{}; uint32_t uploads = 0; };
static std::unordered_map<void*, PoseSpan> g_ctxPose;   // deferred ctx -> head samples of its camera uploads (list not finished)
static std::unordered_map<void*, PoseSpan> g_clPose;    // finished list -> span
static PoseSpan g_frameSpan; static bool g_frameSpanValid = false;
static void SpanAdd(PoseSpan& s, const P5RXr::Pose& p) {
    if (!s.uploads || p.serial < s.first.serial) s.first = p;
    if (!s.uploads || p.serial >= s.last.serial) s.last = p;
    ++s.uploads;
}
static void SpanMerge(PoseSpan& into, const PoseSpan& s) { if (!s.uploads) return; const uint32_t n = into.uploads + s.uploads; SpanAdd(into, s.first); SpanAdd(into, s.last); into.uploads = n; }
static P5RXr::Pose g_execPose{}, g_uploadPose{};           // this frame: pose of the executed camera list / latest upload anywhere
static bool g_execPoseValid = false, g_uploadPoseValid = false;
static std::atomic<uint32_t> g_execPoseCount{0};
// camera upload with the head applied (any thread)
static void NoteFramePose(ID3D11DeviceContext* c, const P5RXr::Pose& p) {
    std::lock_guard<std::mutex> lk(g_tagMx);
    g_uploadPose = p; g_uploadPoseValid = true;
    if (IsImmediateCtx(c)) { g_execPose = p; g_execPoseValid = true; g_execPoseCount.fetch_add(1, std::memory_order_relaxed);
        if (!g_frameSpanValid) { g_frameSpan = PoseSpan{}; g_frameSpanValid = true; } SpanAdd(g_frameSpan, p); }
    else { if (g_ctxPose.size() > 256) g_ctxPose.clear(); SpanAdd(g_ctxPose[c], p); }
}
static void HeadTagFinish(ID3D11DeviceContext* c, ID3D11CommandList* cl) {
    std::lock_guard<std::mutex> lk(g_tagMx);
    auto it = g_ctxPose.find(c);
    if (it == g_ctxPose.end()) return;
    if (cl) { if (g_clPose.size() > 256) g_clPose.clear(); g_clPose[cl] = it->second; }
    g_ctxPose.erase(it);
}
static void HeadTagExecute(ID3D11CommandList* cl) {
    std::lock_guard<std::mutex> lk(g_tagMx);
    auto it = g_clPose.find(cl);
    if (it == g_clPose.end()) return;
    g_execPose = it->second.last; g_execPoseValid = true; g_execPoseCount.fetch_add(1, std::memory_order_relaxed);
    if (!g_frameSpanValid) { g_frameSpan = PoseSpan{}; g_frameSpanValid = true; } SpanMerge(g_frameSpan, it->second);
    g_clPose.erase(it);
}
static float QuatAngleDeg(const float* a, const float* b) {
    float d = fabsf(a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]); if (d > 1.f) d = 1.f;
    return 2.f * acosf(d) * 57.29578f;
}
// Render thread, frame boundary, before the submit: choose this frame's tag and keep the statistics.
static void HeadTagFrame(uint64_t f, bool headApplied) {
    static uint32_t s_frames = 0, s_exec = 0, s_diff = 0, s_age[4] = {}, s_cad[5] = {};
    static double s_px[3] = {}, s_lo[3] = {1e9, 1e9, 1e9}, s_hi[3] = {-1e9, -1e9, -1e9}, s_tagMax = 0;   // [TAGPOS] head travel, tagged position
    static double s_diffSum = 0, s_diffMax = 0, s_period = 0;
    static LARGE_INTEGER s_last{};
    P5RXr::Pose exec{}, upload{}; bool haveExec, haveUpload; PoseSpan span; bool haveSpan;
    {
        std::lock_guard<std::mutex> lk(g_tagMx);
        haveExec = g_execPoseValid; exec = g_execPose; haveUpload = g_uploadPoseValid; upload = g_uploadPose;
        g_execPoseValid = false;   // the next frame must execute its own camera list
        span = g_frameSpan; haveSpan = g_frameSpanValid; g_frameSpanValid = false;
    }
    static uint32_t s_mixed = 0, s_upl = 0; static double s_mixDeg = 0, s_mixMax = 0;
    if (haveSpan && headApplied) {
        s_upl += span.uploads;
        if (span.first.serial != span.last.serial) { ++s_mixed; const double a = QuatAngleDeg(span.first.orientation, span.last.orientation); s_mixDeg += a; if (a > s_mixMax) s_mixMax = a; }
    }
    const bool exact = g_exactTag.load(std::memory_order_relaxed);
    if (exact) { if (haveExec && headApplied) P5RXr::SetRenderPose(&exec); else if (!headApplied) P5RXr::SetRenderPose(nullptr); }
    else if (!headApplied) P5RXr::SetRenderPose(nullptr);   // old: uploads set the tag directly
    // statistics
    ++s_frames;
    { P5RXr::Pose h{}; if (P5RXr::GetHeadPose(&h, nullptr)) for (int i = 0; i < 3; ++i) { if (h.position[i] < s_lo[i]) s_lo[i] = h.position[i]; if (h.position[i] > s_hi[i]) s_hi[i] = h.position[i]; }
      P5RXr::Pose t{}; if (P5RXr::GetRenderPose(&t)) { const double m = sqrt(t.position[0]*t.position[0] + t.position[1]*t.position[1] + t.position[2]*t.position[2]); if (m > s_tagMax) s_tagMax = m; } }
    if (headApplied && haveExec) {
        ++s_exec;
        const uint32_t now = P5RXr::HeadSerial(), age = now >= exec.serial ? now - exec.serial : 0;
        ++s_age[age < 3 ? age : 3];
        if (haveUpload) { const float a = QuatAngleDeg(exec.orientation, upload.orientation);
            if (upload.serial != exec.serial) { ++s_diff; s_diffSum += a; if (a > s_diffMax) s_diffMax = a; } }
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double period = P5RXr::DisplayPeriodMs();
    if (s_last.QuadPart && period > 1.0) {
        const double ms = PerfTicksToMs((uint64_t)(now.QuadPart - s_last.QuadPart));
        const int k = (int)(ms / period + 0.5); ++s_cad[k < 1 ? 0 : (k > 4 ? 4 : k)];
        s_period = period;
    }
    s_last = now;
    if (s_frames >= 300) {
        Log("[headtag] f%llu tag=%s | frames with an executed head camera %u/%u | old tag (latest upload) was a DIFFERENT pose on %u of them, avg %.2f max %.2f deg |"
            " rendered pose age at submit (presents): 0=%u 1=%u 2=%u 3+=%u | refresh %.1f Hz: present interval in display periods <1=%u 1=%u 2=%u 3=%u 4+=%u",
            (unsigned long long)f, exact ? "EXACT (executed list)" : "OLD (latest upload)", s_exec, s_frames, s_diff, s_diff ? s_diffSum / s_diff : 0.0, s_diffMax,
            s_age[0], s_age[1], s_age[2], s_age[3], s_period > 0 ? 1000.0 / s_period : 0.0, s_cad[0], s_cad[1], s_cad[2], s_cad[3], s_cad[4]);
        Log("[mixpose] f%llu frames whose camera uploads used MORE THAN ONE head sample: %u/%u (avg %.2f, max %.2f deg apart) | head-camera uploads per frame %.1f",
            (unsigned long long)f, s_mixed, s_exec, s_mixed ? s_mixDeg / s_mixed : 0.0, s_mixMax, s_exec ? (double)s_upl / s_exec : 0.0);
        s_mixed = s_upl = 0; s_mixDeg = s_mixMax = 0;
        { const auto st = P5RXr::TakeSubmitStats();
          Log("[latency] f%llu %u frames: prediction-time gap %.1f ms (max %.1f) | cached-pose delta %.2f deg (max %.2f), NOT a fresh compositor-motion measurement |"
              " spare render border min %.2f deg/side | frames sliding past the border %u",
              (unsigned long long)f, st.frames, st.latAvgMs, st.latMaxMs, st.degAvg, st.degMax, st.marginMinDeg, (unsigned)st.overMarginFrames); }
        Log("[tagpos] f%llu head position range over 300 frames: x %.1f cm, y %.1f cm, z %.1f cm | largest position in the submitted tag %.1f cm (%s)",
            (unsigned long long)f, (s_hi[0] - s_lo[0]) * 100.0, (s_hi[1] - s_lo[1]) * 100.0, (s_hi[2] - s_lo[2]) * 100.0, s_tagMax * 100.0,
            g_tagRealPos.load() ? "real position" : "zeroed");
        for (int i = 0; i < 3; ++i) { s_lo[i] = 1e9; s_hi[i] = -1e9; } s_tagMax = 0; (void)s_px;
        s_frames = s_exec = s_diff = 0; s_diffSum = s_diffMax = 0; memset(s_age, 0, sizeof(s_age)); memset(s_cad, 0, sizeof(s_cad));
    }
}

// [PACE] ME1 bug 4, ME1's shipped default (stereoFramePacing ON, Docs/HEAD_TRACKING_SMOOTHNESS_FIX.md):
// each stereo frame occupies the same whole number of headset refreshes. Measured here: frames mixed
// 1/2/3 refreshes at 120 Hz; a held frame (freeze test) reprojects steady while live frames shake; no
// frame mixes head poses ([MIXPOSE] 0). ON by default, no key needed.
// Phase lock to the RUNTIME's clock (run 16's version held from its own timestamp and drifted / ratcheted
// 2 -> 3 -> 4): hold the present until (k - 0.5) refreshes after xrWaitFrame last returned, so the next
// xrWaitFrame call lands inside the right refresh and blocks to its boundary. k = refreshes per frame
// for <= ~60 Hz (2 at 120 Hz). A frame the game cannot finish in time is not held (it lands one later).
// The old extra hold delayed xrWaitFrame until AFTER rendering. XR now waits
// before the next render itself; the failed steady-60 experiment stays optional.
static std::atomic<bool> g_paceOn{false};
static int g_paceK = 2;
static void PaceSleepUntil(LARGE_INTEGER target) {
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER fq, now; QueryPerformanceFrequency(&fq);
    for (;;) {
        QueryPerformanceCounter(&now);
        const double remainMs = 1000.0 * (double)(target.QuadPart - now.QuadPart) / (double)fq.QuadPart;
        if (remainMs <= 0) return;
        if (remainMs > 1.5 && timer) {   // coarse sleep, then spin the last ~1 ms
            LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((remainMs - 1.0) * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
        } else YieldProcessor();
    }
}
// Render thread, frame boundary, right before the submit (xrWaitFrame). stereoWorld: live stereo frames only.
static void HeadPace(uint64_t f, bool stereoWorld) {
    static uint32_t s_n = 0, s_held = 0; static double s_holdMs = 0;
    const double period = P5RXr::DisplayPeriodMs();
    const long long last = P5RXr::LastWaitReturnQpc();
    if (g_paceOn.load(std::memory_order_relaxed) && stereoWorld && period > 1.0 && last) {
        g_paceK = (int)ceil(16.0 / period - 0.01); if (g_paceK < 1) g_paceK = 1;
        LARGE_INTEGER fq, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&now);
        LARGE_INTEGER target; target.QuadPart = last + (LONGLONG)(((double)g_paceK - 0.5) * period * (double)fq.QuadPart / 1000.0);
        if (now.QuadPart < target.QuadPart) {
            s_holdMs += 1000.0 * (double)(target.QuadPart - now.QuadPart) / (double)fq.QuadPart; ++s_held;
            PaceSleepUntil(target);
        }
    }
    if (++s_n >= 300) {
        Log("[pace] f%llu %s: %u/300 frames held, avg hold %.1f ms, %d refreshes per frame",
            (unsigned long long)f, g_paceOn.load() ? (stereoWorld ? "ON" : "ON (not stereo now)") : "OFF", s_held, s_held ? s_holdMs / s_held : 0.0, g_paceK);
        s_n = s_held = 0; s_holdMs = 0;
    }
}
