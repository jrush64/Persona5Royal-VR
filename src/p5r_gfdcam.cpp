#include <mutex>
#include <cctype>
#include "p5r_gfdcam.h"
#include "p5r_camera_controls.h"
#include "p5r_culling.h"
#include "p5r_absolute_hook.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <string>

namespace {
GfdCam::LogFn g_log = nullptr;
std::atomic<bool>      g_scanStarted{false};
std::atomic<uintptr_t> g_global{0};          // resolved gfdGlobal (validated)
std::atomic<bool>      g_active{false};

// fovy field discovery (0x1A8 per opengfd; confirmed or re-found against the CB FOV)
int   g_fovyOff = -1;        // offset inside Camera once validated
int   g_fovyUnit = 0;        // 1 = degrees, 2 = radians
float g_lastWritten = 0.f;
uintptr_t g_lastCam = 0;
uint32_t g_resets = 0, g_writes = 0;

constexpr float kPi = 3.14159265f;

bool SafeRead(const void* p, void* out, size_t n) {
    __try { memcpy(out, p, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeWriteF(void* p, float v) {
    __try { *(volatile float*)p = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeOrI(void* p, int32_t bits) {
    __try { *(volatile int32_t*)p |= bits; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <typename T> bool Rd(uintptr_t a, T& out) { return a > 0x10000 && SafeRead((const void*)a, &out, sizeof(T)); }

// sig: F7 05 ?? ?? ?? ?? 00 00 00 02  (test dword [rip+rel32], 0x02000000 — a GraphicsFlags test)
bool MatchAt(const uint8_t* p) {
    return p[0]==0xF7 && p[1]==0x05 && p[6]==0x00 && p[7]==0x00 && p[8]==0x00 && p[9]==0x02;
}

// Plausibility of a Global candidate: VideoMode width/height (Global+0x38/+0x3C) and a scene with
// a camera whose aspect looks like a real aspect ratio.
bool ValidateGlobal(uintptr_t g, int* outW, int* outH, uintptr_t* outCam) {
    int32_t w=0, h=0; uintptr_t sc=0, cam=0; float aspect=0;
    if (!Rd(g+0x38, w) || !Rd(g+0x3C, h)) return false;
    if (w < 320 || w > 16384 || h < 200 || h > 16384) return false;
    for (int i = 0; i < 2; ++i) {
        if (!Rd(g+0x60+8*i, sc) || sc < 0x10000) continue;
        if (!Rd(sc+0x10, cam) || cam < 0x10000) continue;
        if (!Rd(cam+0x1AC, aspect)) continue;
        if (aspect > 0.5f && aspect < 4.0f) { *outW=w; *outH=h; *outCam=cam; return true; }
    }
    *outW=w; *outH=h; *outCam=0;
    return true;   // video mode alone is strong; the camera may not exist yet at scan time
}

// Read-only discovery. Layout facts from the field-camera/task/node research;
// no reference implementation or engine mutation is used here.
#ifdef P5R_COUNT_PROBE_READS
unsigned long long g_probeReadCalls=0;
#endif
bool ProbeRead(uintptr_t ptr,void* data,size_t size){
#ifdef P5R_COUNT_PROBE_READS
    ++g_probeReadCalls;
#endif
    SIZE_T got=0;return ptr>0x10000 && ReadProcessMemory(GetCurrentProcess(),(void*)ptr,data,size,&got) && got==size;
}
template<class T> bool ProbeRd(uintptr_t ptr,T& value){return ProbeRead(ptr,&value,sizeof(value));}
bool ProbeName(uintptr_t name,char (&text)[80]){
    uintptr_t chars=0;int length=0;text[0]=0;
    if(!ProbeRd(name+8,chars)||!chars||!ProbeRd(name+16,length))return false;
    uint32_t flags=0;if(!ProbeRd(name,flags))return false;
    if(!(flags&2)||length<=0){
        for(int i=0;i<79;++i){char c=0;if(!ProbeRead(chars+i,&c,1))return false;if(!c){text[i]=0;return i>0;}if((unsigned char)c<32||(unsigned char)c>126)return false;text[i]=c;}return false;
    }
    if(length>78)return false;
    if(!ProbeRead(chars,text,length))return false;text[length]=0;
    for(int i=0;i<length;++i)if((unsigned char)text[i]<32||(unsigned char)text[i]>126)return false;
    return true;
}
bool EventCullIdentity(uintptr_t& camera, uintptr_t& scene, uintptr_t& root);
bool EventFirstPersonWanted();
bool BattleSceneWanted();
bool BattleCullIdentity(uintptr_t& camera, uintptr_t& scene, uintptr_t& root);
#include "p5r_head_anchor.inl"
#include "p5r_native_frustum.inl"
#include "p5r_visibility_test.inl"
#include "p5r_scene_visibility.inl"
#include "p5r_geometry_cull.inl"
#include "p5r_screen_tasks.inl"
void ProbePlayer(uintptr_t global,uint64_t frame){

    if(frame%30 || !g_log)return;
    uintptr_t visited[2048]={};unsigned count=0;uintptr_t cameraWork=0;
    // TaskGlobal lies near the end of the validated GFD global, before field2ce8.
    // Search its possible list heads, then validate names/status before traversing.
    for(unsigned off=0x2800;off<0x2cf0 && count<2048 && !cameraWork;off+=8){
        uintptr_t task=0;ProbeRd(global+off,task);
        for(unsigned chain=0;task && chain<512 && count<2048;++chain){
            bool seen=false;for(unsigned i=0;i<count;++i)if(visited[i]==task){seen=true;break;}if(seen)break;
            visited[count++]=task;
            uint32_t status=0;char name[80];
            if(!ProbeRd(task,status)||status<1||status>3||!ProbeName(task+0x10,name))break;
            if(strcmp(name,"field camera CTRL")==0){ProbeRd(task+0x48,cameraWork);InstallBodyHook(task);break;}
            uintptr_t next=0;if(!ProbeRd(task+0x80,next)||next==task)break;task=next;
        }
    }
    uintptr_t camera=0,root=0;
    if(!cameraWork||!ProbeRd(cameraWork+0x50,camera)||!ProbeRd(cameraWork+0x58,root)){
        if(frame%600==0){
         g_log("[player-probe] f%llu field-camera task unresolved (read-only, %u candidates)",(unsigned long long)frame,count);
         char names[64][80]={};unsigned used=0;
         for(unsigned i=0;i<count&&used<64;++i){char name[80];if(!ProbeName(visited[i]+0x10,name))continue;
          bool duplicate=false;for(unsigned n=0;n<used;++n)if(!strcmp(names[n],name))duplicate=true;
          if(!duplicate){strcpy_s(names[used++],name);g_log("[camera-tasks] f%llu task=%p name=%s",(unsigned long long)frame,(void*)visited[i],name);}
         }
        }
        return;
    }
    bool match=false;for(int i=0;i<2;++i){uintptr_t scene=0,sceneCamera=0;if(ProbeRd(global+0x60+8*i,scene)&&ProbeRd(scene+0x10,sceneCamera)&&sceneCamera==camera)match=true;}
    if(!match){g_log("[player-probe] field camera does not match active scene; rejected");return;}
    if(frame%600)return;
    auto tree=FindHead(root,true);
    g_log("[player-probe] f%llu targetRoot=%p nodes=%u headCandidates=%u; engineHook=%d",(unsigned long long)frame,(void*)root,tree.nodes,tree.heads,(int)(g_fieldHook!=0));
}

DWORD WINAPI ScanThread(void*) {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto base = (uintptr_t)exe;
    auto dos = (IMAGE_DOS_HEADER*)exe;
    auto nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    g_log("[gfdcam] scanning P5R image %p..%p (%.0f MB) for gfdGlobal sig", (void*)base, (void*)end, (end-base)/1048576.0);
    int matches = 0; uintptr_t chosen = 0;
    uintptr_t a = base;
    while (a < end && !chosen) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
            (mbi.Protect & (PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_READONLY|PAGE_READWRITE|PAGE_EXECUTE_WRITECOPY|PAGE_WRITECOPY));
        bool exec = (mbi.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) != 0;
        if (readable && exec) {
            __try {
                for (uintptr_t p = rs; p + 10 <= re && p + 10 <= end; ++p) {
                    if (!MatchAt((const uint8_t*)p)) continue;
                    ++matches;
                    int32_t rel = *(const int32_t*)(p + 2);
                    uintptr_t target = p + 10 + rel;
                    // opengfd: resolved - 44. Our reading: target = GraphicsState.flags = Global+0x30.
                    const int deltas[3] = { 0x30, 0x2C, 0x34 };
                    for (int d : deltas) {
                        int w=0,h=0; uintptr_t cam=0;
                        if (ValidateGlobal(target - d, &w, &h, &cam)) {
                            g_log("[gfdcam] sig @P5R+0x%llX -> target %p, Global=%p (target-0x%X) video=%dx%d cam=%p",
                                  (unsigned long long)(p-base), (void*)target, (void*)(target-d), d, w, h, (void*)cam);
                            chosen = target - d; break;
                        }
                    }
                    if (chosen) break;
                    if (matches <= 8) g_log("[gfdcam] sig @P5R+0x%llX -> target %p: no valid Global layout", (unsigned long long)(p-base), (void*)target);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        a = re;
    }
    if (chosen) g_global.store(chosen);
    else g_log("[gfdcam] FAILED: %d sig matches, no validated gfdGlobal", matches);
    return 0;
}
} // namespace

namespace GfdCam {

void StartScan(LogFn log) {
    bool exp = false;
    if (!g_scanStarted.compare_exchange_strong(exp, true)) return;
    g_log = log;
    HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

#ifdef P5R_TEST_CAMERA_CONTEXT
void TestSetView(const float* rowMajor){
 CameraContextState state;
 if(rowMajor){state.kind=ContextKind::Field;for(int r=0;r<4;++r)for(int c=0;c<4;++c)state.view[c*4+r]=rowMajor[r*4+c];}
 SetContext(state);
}
#endif
const char* CameraContext(){
 auto c=ReadContext();
 if(GetTickCount64()-c.time>250)return "Waiting for camera";
 switch(c.kind){
 case ContextKind::Field:return "Field camera";
 case ContextKind::Locked:return "Paused: camera locked";
 case ContextKind::ModeChanged:return "Paused: field camera changed";
 default:return "Paused: no active field camera";
 }
}
void RoomCamStats(uint32_t& forced,uint32_t& otherMode){forced=g_roomForced.exchange(0);otherMode=g_roomOtherMode.exchange(0);}
bool FirstPersonEye(float out[3]){
 AcquireSRWLockShared(&g_fpLock);auto a=g_fpAnchor;ReleaseSRWLockShared(&g_fpLock);
 if(!a.valid||GetTickCount64()-a.time>250)return false;
 memcpy(out,a.eye,12);return true;
}
bool AllowManual(){
 auto c=ReadContext();
 return c.kind==ContextKind::Field&&GetTickCount64()-c.time<=250;
}
bool Active() { return g_active.load(std::memory_order_relaxed); }
bool ScreenMode() { return g_screenMode.load(std::memory_order_relaxed); }
const char* ScreenTask() { return g_screenTask; }
bool FlatTask(const char* name) { return IsFlatTask(name); }
uintptr_t EventJokerHead() { return g_evJokerHead.load(); }
bool EventRunning() { return g_eventRunning.load(std::memory_order_relaxed); }
bool ConfidantRunning() { return g_cmmRunning.load(std::memory_order_relaxed); }
bool CutInRunning() { return g_cutInRunning.load(std::memory_order_relaxed); }
size_t RenderTasks(char (*names)[48], uintptr_t* tasks, size_t cap) {
    std::lock_guard<std::mutex> lk(g_taskRefMx); size_t n = 0;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    for (auto& t : g_taskRefs) {
        if (n >= cap) break;
        MEMORY_BASIC_INFORMATION mi{};
        if (!t.render || !VirtualQuery((void*)t.render, &mi, sizeof(mi)) || (uintptr_t)mi.AllocationBase != base) continue;
        bool dup = false; for (size_t i = 0; i < n; ++i) if (tasks[i] == t.task) dup = true; if (dup) continue;
        snprintf(names[n], 48, "%s", t.name.c_str()); tasks[n++] = t.task;
    }
    return n;
}
static uint64_t KnockoutRender(uintptr_t) { return 0; }
uintptr_t RenderOff(uintptr_t task) {   // returns the original callback (0 = not switched)
    uintptr_t cur = 0; if (!Rd(task + 0x38, cur) || !cur || cur == (uintptr_t)KnockoutRender) return 0;
    return InterlockedCompareExchangePointer((void* volatile*)(task + 0x38), (void*)KnockoutRender, (void*)cur) == (void*)cur ? cur : 0;
}
void RenderOn(uintptr_t task, uintptr_t orig) {
    if (task && orig) InterlockedCompareExchangePointer((void* volatile*)(task + 0x38), (void*)orig, (void*)KnockoutRender);
}
static std::atomic<uint64_t> g_gfFrame{0};
bool EventEye(float eye[3], float target[3], bool& hasTarget) {
    hasTarget = false;
    const uintptr_t head = g_evJokerHead.load(std::memory_order_relaxed);
    if (!head || !g_eventRunning.load(std::memory_order_relaxed)) return false;
    // [EVMEDIAN] 10-16: camera trace proved the flash = Joker's head bone rotation read wrong for a WHOLE frame (13-44 deg,
    // both eyes; game camera and headset pose smooth). Read the bone ONCE per game frame (both eyes share it) and use the
    // circular median of the last 5 frames' heading: 1-2 bad frames can never reach the image; real turns follow ~2 frames
    // late (Joker's animated heading only; the headset pose is applied afterwards, untouched). A head-position jump (cut)
    // clears the history so cuts apply at once.
    static SRWLOCK s_lock = SRWLOCK_INIT;
    static uint64_t s_frame = ~0ull; static bool s_valid = false; static float s_eye[3] = {}, s_at[3] = {};
    static float s_yaw[5] = {}; static float s_pos[3] = {}; static int s_n = 0;
    static unsigned s_rejectLogs = 0, s_rejects = 0, s_frames = 0;
    const uint64_t frame = g_gfFrame.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&s_lock);
    if (frame == s_frame && frame != 0) {
        const bool ok = s_valid;
        if (ok) { memcpy(eye, s_eye, 12); memcpy(target, s_at, 12); hasTarget = true; }
        ReleaseSRWLockExclusive(&s_lock);
        return ok;
    }
    s_frame = frame; s_valid = false;
    char name[80]{}; float m[16];
    bool ok = ProbeName(head+0xf0,name) && HeadName(name) && ProbeRead(head+0x20,m,64);
    if (ok) for (float v:m) if (!std::isfinite(v)) ok = false;
    // Joker's measured head-forward axis is local +Y. Same axis in cutscenes and confidants; never a partner.
    const float length = ok ? sqrtf(m[4]*m[4]+m[6]*m[6]) : 0.f;
    if (!ok || length < 1e-3f) { ReleaseSRWLockExclusive(&s_lock); return false; }
    const float rawYaw = atan2f(m[4]/length, m[6]/length);
    const float dp = sqrtf((m[12]-s_pos[0])*(m[12]-s_pos[0])+(m[13]-s_pos[1])*(m[13]-s_pos[1])+(m[14]-s_pos[2])*(m[14]-s_pos[2]));
    if (s_n == 0 || dp > 20.f) s_n = 0;   // first sample or a cut: no history to filter against
    s_pos[0] = m[12]; s_pos[1] = m[13]; s_pos[2] = m[14];
    if (s_n < 5) s_yaw[s_n++] = rawYaw; else { for (int i = 0; i < 4; ++i) s_yaw[i] = s_yaw[i+1]; s_yaw[4] = rawYaw; }
    // circular median: deltas from the newest sample wrapped to (-pi, pi], median delta added back
    float d[5]; for (int i = 0; i < s_n; ++i) { float x = s_yaw[i] - rawYaw; while (x > 3.14159265f) x -= 6.2831853f; while (x <= -3.14159265f) x += 6.2831853f; d[i] = x; }
    std::sort(d, d + s_n);
    const float yaw = rawYaw + (s_n ? d[s_n/2] : 0.f);
    ++s_frames;
    const float diffDeg = fabsf(yaw - rawYaw) * 57.29578f;
    if (diffDeg > 5.f) { ++s_rejects; if (g_log && s_rejectLogs++ < 300) g_log("[evmedian] f%llu rejected head-heading reading: raw %.1f deg, used %.1f deg (off by %.1f)", (unsigned long long)frame, rawYaw*57.29578f, yaw*57.29578f, diffDeg); }
    if (g_log && (s_frames % 600) == 0) g_log("[evmedian] f%llu %u readings filtered, %u rejected (>5 deg off the 5-frame median)", (unsigned long long)frame, s_frames, s_rejects);
    const float fx = sinf(yaw), fz = cosf(yaw);
    eye[0]=m[12]+fx*12.f;eye[1]=m[13]+6.f;eye[2]=m[14]+fz*12.f;
    target[0]=eye[0]+fx*300.f;target[1]=eye[1];target[2]=eye[2]+fz*300.f;
    hasTarget=true;
    memcpy(s_eye, eye, 12); memcpy(s_at, target, 12); s_valid = true;
    ReleaseSRWLockExclusive(&s_lock);
    return true;
}
bool SolidTask(const char* name) { return IsSolidTask(name); }

// [RENDERSIZE] at 3840x3840 the engine keeps its 3D targets at 3840x2160 and letterboxes them
// (composite viewport y=840 h=2160). Find where the engine keeps that internal size: dump every
// plausible size/aspect field of Global once in gameplay.
static void DumpGlobalSizes(uintptr_t g) {
    if (!g_log) return;
    char line[900]; int n = snprintf(line, sizeof(line), "[rendersize] Global=%p ints/floats of interest:", (void*)g);
    for (uint32_t o = 0; o < 0x800 && n < 860; o += 4) {
        int32_t iv = 0; float fv = 0;
        if (!Rd(g + o, iv)) continue;
        memcpy(&fv, &iv, 4);
        const bool sizeLike = iv == 3840 || iv == 2160 || iv == 1920 || iv == 1080 || iv == 1280 || iv == 720 || iv == 960 || iv == 540;
        const bool aspectLike = (fabsf(fv - 1.7777778f) < 0.001f) || (fabsf(fv - 0.5625f) < 0.0001f) || (fabsf(fv - 1.f) < 1e-6f && false);
        if (sizeLike) n += snprintf(line + n, sizeof(line) - n, " +0x%X=%d", o, iv);
        else if (aspectLike) n += snprintf(line + n, sizeof(line) - n, " +0x%X=%.4ff", o, fv);
    }
    g_log("%s", line);
}
void OnFrame(uint64_t f, float cbVFov, float targetVFov, bool enable) {
    g_gfFrame.store(f,std::memory_order_relaxed);
    uintptr_t g = g_global.load(std::memory_order_relaxed);
    if (!g) return;
    if (f == 1500 || f == 4000) DumpGlobalSizes(g);
    ProbePlayer(g,f);
    ScreenTaskCensus(g,f);
    MaintainContext();
    // Build c91e4dca fixed the Central Street head-turn culling (headset-tested 2026-09-30).
    // Both the recursive geometry flag and the later object predicate must be overridden.
    InstallGeometryCull();
    InstallVisibilityTest();
    if(g_geometryCull)MaintainGeometryCull();
    if(f%300==0&&g_log)g_log("[geometry-cull] f%llu suppressed=%u",(unsigned long long)f,g_geometryCullSuppressed.exchange(0));
    if(f%300==0&&g_log)g_log("[visibility-test] f%llu tested=%u kept-for-eyes=%u culled-outside-eyes=%u precise=%d",(unsigned long long)f,g_visibilityTests.exchange(0),g_visibilityRescued.exchange(0),g_visibilityCulledPrecise.exchange(0),(int)P5RCulling::precise.load());
    // current scene camera: try scene[0] then scene[1]; prefer the one whose fovy field matches the CB
    uintptr_t cams[2] = {0,0};
    for (int i = 0; i < 2; ++i) { uintptr_t sc=0, cam=0; if (Rd(g+0x60+8*i, sc) && Rd(sc+0x10, cam)) cams[i]=cam; }

    // --- one-time fovy field discovery (needs a live CB FOV to compare against) ---
    if (g_fovyOff < 0 && cbVFov > 0.01f) {
        const float deg = cbVFov * 180.f / kPi;
        for (int ci = 0; ci < 2 && g_fovyOff < 0; ++ci) {
            uintptr_t cam = cams[ci]; if (!cam) continue;
            float v = 0;
            // opengfd's offset first, then a bounded search of the float fields around it
            int offs[1 + (0x240-0x80)/4]; int n = 0; offs[n++] = 0x1A8;
            for (int o = 0x80; o < 0x240; o += 4) if (o != 0x1A8) offs[n++] = o;
            for (int k = 0; k < n; ++k) {
                if (!Rd(cam + offs[k], v)) continue;
                int unit = (fabsf(v - deg) < 1.0f) ? 1 : (fabsf(v - cbVFov) < 0.02f ? 2 : 0);
                // layout is fovy, aspect: the next float must be a real aspect (strict off-layout)
                float asp = 0; Rd(cam + offs[k] + 4, asp);
                bool aspOk = offs[k] == 0x1A8 ? (asp > 0.5f && asp < 4.f) : (asp > 1.6f && asp < 1.9f);
                if (unit && aspOk) {
                    g_fovyOff = offs[k]; g_fovyUnit = unit;
                    g_log("[gfdcam] fovy FOUND scene[%d] cam=%p +0x%X = %.4f %s (CB renders %.2f deg vertical)%s",
                          ci, (void*)cam, offs[k], v, unit==1?"deg":"rad", deg, offs[k]==0x1A8?" == opengfd layout":" (NOT 0x1A8!)");
                    break;
                }
            }
        }
        static uint64_t s_lastMiss = 0;
        if (g_fovyOff < 0 && f - s_lastMiss >= 300) {
            s_lastMiss = f;
            float a0=0,a1=0; Rd(cams[0]+0x1A8, a0); Rd(cams[1]+0x1A8, a1);
            g_log("[gfdcam] fovy not matched yet: CB vfov=%.2f deg; cam0=%p +0x1A8=%.4f cam1=%p +0x1A8=%.4f",
                  deg, (void*)cams[0], a0, (void*)cams[1], a1);
        }
    }
    if (g_fovyOff < 0) return;

    // pick the camera that holds a plausible fovy at the discovered offset
    uintptr_t cam = 0;
    for (int ci = 0; ci < 2 && !cam; ++ci) {
        float v = 0;
        if (cams[ci] && Rd(cams[ci] + g_fovyOff, v) && v > 0.01f && v < (g_fovyUnit==1 ? 179.f : 3.1f)) cam = cams[ci];
    }
    if (!cam) { g_active.store(false); return; }
    static uint64_t s_camSince = 0;
    if (cam != g_lastCam) {
        g_log("[gfdcam] scene camera now %p (was %p) f=%llu", (void*)cam, (void*)g_lastCam, (unsigned long long)f);
        g_lastCam = cam; g_lastWritten = 0.f; s_camSince = f;
    }
    // v2 crash fix: loads free/rebuild scenes. Only write on a camera that has been stable for 2s and
    // still looks like a camera (aspect + near/far sane) this very frame.
    {
        float nearC=0, farC=0, asp=0;
        Rd(cam + g_fovyOff - 8, nearC); Rd(cam + g_fovyOff - 4, farC); Rd(cam + g_fovyOff + 4, asp);
        bool sane = asp > 0.5f && asp < 4.f && nearC > 0.f && farC > nearC && farC < 1e8f;
        if (!sane || f - s_camSince < 120) enable = false;
    }

    float cur = 0; Rd(cam + g_fovyOff, cur);
    static float s_orig = 0.f;   // the game's own fovy, restored when the override turns off
    if (!enable || targetVFov <= 0.01f) {
        if (g_active.exchange(false)) {
            if (s_orig > 0.f && fabsf(cur - g_lastWritten) < 1e-4f) {
                SafeWriteF((void*)(cam + g_fovyOff), s_orig);
            }
            g_log("[gfdcam] source FOV override OFF (restored fovy %.3f)", s_orig);
        }
        g_lastWritten = 0.f;
        return;
    }
    if (g_lastWritten == 0.f || fabsf(cur - g_lastWritten) > 1e-4f) s_orig = cur;   // game-owned value
    float want = (g_fovyUnit == 1) ? targetVFov * 180.f / kPi : targetVFov;
    if (g_lastWritten != 0.f && fabsf(cur - g_lastWritten) > 1e-4f) ++g_resets;   // game rewrote it since last frame
    if (fabsf(cur - want) > 1e-4f) {
        // v2: fovy ONLY. The game rewrites fovy every frame and rebuilds projection + frustum from it
        // (run 1: CB went 82->99 with no dirty write needed) — never touch other fields.
        if (SafeWriteF((void*)(cam + g_fovyOff), want)) ++g_writes;
    }
    g_lastWritten = want;
    if (!g_active.exchange(true))
        g_log("[gfdcam] source FOV override ON: fovy %.3f -> %.3f (%s) cam=%p", cur, want, g_fovyUnit==1?"deg":"rad", (void*)cam);
    if ((f % 300) == 0) {
        float view[16] = {}; Rd(cam + 0x20, view);
        g_log("[gfdcam] f%llu fovy=%.3f want=%.3f writes=%u gameResets=%u | CB-rendered vfov=%.2f deg | cam.view t=(%.1f %.1f %.1f) (%.1f %.1f %.1f)",
              (unsigned long long)f, cur, want, g_writes, g_resets, cbVFov*180.f/kPi,
              view[12], view[13], view[14], view[3], view[7], view[11]);
        g_writes = 0; g_resets = 0;
    }
}

} // namespace GfdCam
bool GfdCam::TakeUiDump(uint64_t frame, char* label, size_t cap) { return UiDumpTake(frame, label, cap); }
void GfdCam::ScheduleUiDump(uint64_t due, const char* label) { UiDumpSchedule(due, label); }
