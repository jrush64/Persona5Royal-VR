// Live access trace identified the six-plane/eight-corner visibility predicate.
// Preserve native evaluation; override only a rejected active field-camera test.
using VisibilityTestFn=int(*)(uintptr_t,uintptr_t);
VisibilityTestFn g_visibilityTest=nullptr;
bool g_visibilityTestAttempted=false;
std::atomic<unsigned> g_visibilityTests{0},g_visibilityRescued{0},g_visibilityCulledPrecise{0};
constexpr unsigned char visibilityPrefix[]={0x48,0x83,0xec,0x58,0x0f,0x29,0x74,0x24,0x40,0x45,0x31,0xc0,0x0f,0x29,0x7c,0x24,0x30};
constexpr unsigned char visibilityBoundaries[]={0,4,9,12};
std::atomic<uintptr_t> g_cullCamera{0};
bool CullSceneActive();
extern std::atomic<bool> g_cullEvent,g_cullBattle;
uintptr_t ActiveCullCamera();
// [PRECISECULL] the rendered eye frusta in the game's plane layout and sign, rebuilt per thread
// whenever the camera hooks publish a new eye view-projection.
struct PreciseCache { uint32_t version = ~0u; uintptr_t camera = 0; bool valid = false; float planes[48]; };
thread_local PreciseCache t_precise;
const float* PrecisePlanes(uintptr_t camera, const float* gamePlanes) {
 if (!P5RCulling::precise.load(std::memory_order_relaxed)) return nullptr;
 if (GetTickCount64() - P5RCulling::eyeTime.load(std::memory_order_relaxed) > 250) return nullptr;   // camera hooks quiet: fall back
 const uint32_t version = P5RCulling::eyeVersion.load(std::memory_order_acquire);
 if (t_precise.version == version && t_precise.camera == camera) return t_precise.valid ? t_precise.planes : nullptr;
 t_precise.version = version; t_precise.camera = camera; t_precise.valid = false;
 float view[16], nearClip = 0, farClip = 0, vp[2][16];
 if (!Rd(camera + 0x20, view) || !Rd(camera + 0x1a0, nearClip) || !Rd(camera + 0x1a4, farClip)) return nullptr;
 const float sign = P5RCulling::GamePlaneSign(view, gamePlanes, nearClip, farClip);
 if (sign == 0) return nullptr;
 float eye[3];
 AcquireSRWLockShared(&P5RCulling::eyeLock); memcpy(vp, P5RCulling::eyeVP, sizeof(vp)); memcpy(eye, P5RCulling::eyePos, sizeof(eye)); ReleaseSRWLockShared(&P5RCulling::eyeLock);
 // the rendered eye must be the game camera moved by the manual offsets (same world space);
 // otherwise (camera-relative rendering, other camera) never cull with these planes
 {
  float center[3]; for (int j = 0; j < 3; ++j) center[j] = -(view[j*4]*view[12] + view[j*4+1]*view[13] + view[j*4+2]*view[14]);
  const auto s = CameraControls::Get();
  const float allowed = fabsf(s.right) + fabsf(s.up) + fabsf(s.forward) + CameraControls::HalfEye(s) + 500.f;
  const float dx = eye[0]-center[0], dy = eye[1]-center[1], dz = eye[2]-center[2];
  const float dist = sqrtf(dx*dx + dy*dy + dz*dz);
  static std::atomic<int> s_logged{0};
  if (!(dist <= allowed)) { if (s_logged.fetch_add(1) < 3 && g_log) g_log("[precise-cull] eye %.0f units from the game camera (allowed %.0f): not culling with eye frusta", dist, allowed); return nullptr; }
 }
 // the predicate rejects on positive distance (outside), i.e. the game's sign convention
 if (!P5RCulling::PlanesFromVP(vp[0], farClip, sign, t_precise.planes) || !P5RCulling::PlanesFromVP(vp[1], farClip, sign, t_precise.planes + 24)) return nullptr;
 t_precise.valid = true;
 return t_precise.planes;
}
int NativeVisibilityTest(uintptr_t planes,uintptr_t corners){
 const int result=g_visibilityTest(planes,corners);
 const uintptr_t camera=ActiveCullCamera();
 if(!camera||planes!=camera+0xb0)return result;
 ++g_visibilityTests;GfdCam::visibilityStatus=1;
 if(result!=0)return result;                       // inside the game's own view: unchanged
 // Event GPU views are relocated to Joker; field eye history is not an event frustum.
 if(g_cullEvent.load()||g_cullBattle.load()){++g_visibilityRescued;return 1;}
 if(const float* eyes=PrecisePlanes(camera,(const float*)planes)){
  // rejected by the game camera: visible only if either rendered eye can see it
  if(g_visibilityTest((uintptr_t)eyes,corners)||g_visibilityTest((uintptr_t)(eyes+24),corners)){++g_visibilityRescued;return 1;}
  ++g_visibilityCulledPrecise;return 0;
 }
 ++g_visibilityRescued;return 1;                   // no eye frusta yet: previous behaviour (visible)
}
void InstallVisibilityTest(){
 if(g_visibilityTestAttempted||!GfdCam::visibilityRequested.load()||(!g_contextTask.load()&&!EventFirstPersonWanted()&&!BattleSceneWanted()))return;
 g_visibilityTestAttempted=true;
 const auto base=(uintptr_t)GetModuleHandleW(nullptr),target=base+0x5e9ae60;
 unsigned char entry[sizeof(visibilityPrefix)],reader[5],caller[5];
 const unsigned char readExpected[]={0xf3,0x44,0x0f,0x10,0x01};
 const unsigned char callExpected[]={0xe8,0x13,0x0a,0x06,0x00};
 bool verified=ProbeRead(target,entry,sizeof(entry))&&!memcmp(entry,visibilityPrefix,sizeof(entry))&&
  ProbeRead(base+0x5e9ae83,reader,5)&&!memcmp(reader,readExpected,5)&&
  ProbeRead(base+0x1900b8,caller,5)&&!memcmp(caller,callExpected,5);
 bool installed=verified&&P5RAbsoluteHook::Install((void*)target,(void*)NativeVisibilityTest,(void**)&g_visibilityTest,
  visibilityPrefix,sizeof(visibilityPrefix),visibilityBoundaries,sizeof(visibilityBoundaries));
 if(g_log)g_log("[visibility-test] predicate=P5R+0x5E9AE60 verified=%d installed=%d",verified,installed);
 if(!installed)GfdCam::visibilityStatus=3;
}
