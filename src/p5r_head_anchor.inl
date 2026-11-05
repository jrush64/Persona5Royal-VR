// Engine layout facts are isolated here; all reads are bounded and identity checked.
struct HeadTree {uintptr_t head=0;unsigned nodes=0,heads=0;bool contains=false;float position[3]={};uintptr_t visitedNodes[512]={};};
bool NodeMatrix(uintptr_t node,float (&m)[16]){
 uint32_t kind=0;if(!ProbeRd(node,kind)||kind!=3||!ProbeRead(node+0x20,m,sizeof(m)))return false;
 for(float v:m)if(!std::isfinite(v)||fabsf(v)>1e8f)return false;
 return fabsf(m[15]-1)<.01f && fabsf(m[3])+fabsf(m[7])+fabsf(m[11])<.01f;
}
bool HeadName(const char* text){
 char s[80];strcpy_s(s,text);for(char* p=s;*p;++p)if(*p>='A'&&*p<='Z')*p+='a'-'A';
 return !strcmp(s,"head")||!strcmp(s,"bip01 head")||!strcmp(s,"bip01_head")||!strcmp(s,"j_head")||!strcmp(s,"head_n");
}
HeadTree FindHead(uintptr_t root,bool dump,uintptr_t sought=0){
 HeadTree result;uintptr_t todo[512]={root},seen[512]={};unsigned pending=1,visited=0;
 float origin[16];if(!NodeMatrix(root,origin)){if(dump&&g_log)g_log("[head-anchor] invalid root %p",(void*)root);return result;}
 while(pending&&visited<512){
  uintptr_t node=todo[--pending];bool duplicate=false;for(unsigned i=0;i<visited;++i)if(seen[i]==node)duplicate=true;if(duplicate)continue;
  seen[visited++]=node;float m[16];if(!NodeMatrix(node,m))continue;result.visitedNodes[result.nodes++]=node;if(node==sought)result.contains=true;
  char name[80]={};bool named=ProbeName(node+0xf0,name);
  if(named&&HeadName(name)){
   float distance=0;for(int i=0;i<3;++i){float d=m[12+i]-origin[12+i];distance+=d*d;}
   if(distance<3000.f*3000.f){++result.heads;result.head=node;memcpy(result.position,m+12,12);}
  }
  if(dump&&g_log)g_log("[head-anchor] node=%p name=%s xyz=(%.2f %.2f %.2f)",(void*)node,named?name:"<unnamed>",m[12],m[13],m[14]);
  uintptr_t child=0,next=0;ProbeRd(node+0xd8,child);ProbeRd(node+0xe0,next);
  // Never leave the target's subtree via the root's own sibling.
  if(node!=root&&next&&pending<512)todo[pending++]=next;
  // Morgana is an attached actor with his own Bip01 Head. Keep siblings, skip his subtree.
  if(child&&pending<512&&(!named||strcmp(name,"morugana_root")))todo[pending++]=child;
 }
 if(result.heads!=1)result.head=0;
 return result;
}
using FieldUpdate=uint64_t(*)(uintptr_t,float);
FieldUpdate g_fieldUpdate=nullptr;
uintptr_t g_fieldHook=0;
struct HiddenModel {uintptr_t root=0,head=0;uintptr_t geometry[512]={};unsigned count=0;} g_hidden;
// Layout facts from OpenGFD: Node.object_head +0x110; Object id/parent/next
// +0/+8/+0x18; Geometry.flags +0x20, Hidden bit 5. Validate ownership before writes.
struct GeometrySet {uintptr_t items[512]={};unsigned count=0;};
GeometrySet FindGeometry(const HeadTree& tree){
 GeometrySet result;
 for(unsigned n=0;n<tree.nodes;++n){
  uintptr_t object=0;ProbeRd(tree.visitedNodes[n]+0x110,object);
  uintptr_t seen[64]={};unsigned count=0;
  while(object&&count<64){
   bool duplicate=false;for(unsigned i=0;i<count;++i)if(seen[i]==object)duplicate=true;
   if(duplicate)break;seen[count++]=object;
   uint32_t kind=0;uintptr_t owner=0,next=0;
   if(!ProbeRd(object,kind)||!ProbeRd(object+8,owner)||owner!=tree.visitedNodes[n])break;
   if(kind==4&&result.count<512)result.items[result.count++]=object;
   if(!ProbeRd(object+0x18,next))break;object=next;
  }
 }
 return result;
}
bool SetGeometryHidden(uintptr_t object,bool hidden){
 __try{
  auto flags=(volatile LONG*)(object+0x20);
  if(hidden)InterlockedOr(flags,0x20);else InterlockedAnd(flags,~LONG(0x20));
  return true;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool FieldIdentity(uintptr_t task,uintptr_t& camera,uintptr_t& root,bool requireScene=true){
 uint32_t status=0;char name[80];uintptr_t work=0;
 if(!ProbeRd(task,status)||status!=2||!ProbeName(task+0x10,name)||strcmp(name,"field camera CTRL")||!ProbeRd(task+0x48,work))return false;
 if(!ProbeRd(work+0x50,camera)||!ProbeRd(work+0x58,root))return false;
 uint32_t kind=0;if(!ProbeRd(camera,kind)||kind!=5)return false;
 if(!requireScene)return true;
 uintptr_t global=g_global.load();for(int i=0;i<2;++i){uintptr_t scene=0,active=0;if(ProbeRd(global+0x60+i*8,scene)&&ProbeRd(scene+0x10,active)&&active==camera)return true;}return false;
}
SRWLOCK g_bodyLock=SRWLOCK_INIT;
struct BodyGuard {BodyGuard(){AcquireSRWLockExclusive(&g_bodyLock);}~BodyGuard(){ReleaseSRWLockExclusive(&g_bodyLock);}};
std::atomic<uintptr_t> g_contextTask{0};
enum class ContextKind {Unknown,Field,Locked,ModeChanged};
struct CameraContextState {
 ContextKind kind=ContextKind::Unknown;float view[16]={};ULONGLONG time=0;
 int mode=0,type=0;uint32_t flags=0;
} g_context;
SRWLOCK g_contextLock=SRWLOCK_INIT;
bool g_modeKnown=false;int g_baseMode=0,g_baseType=0;uintptr_t g_baseTask=0;
ULONGLONG g_baseLearned=0,g_candSince=0;int g_candMode=0,g_candType=0;   // [FPBASE]
CameraContextState ReadContext(){
 AcquireSRWLockShared(&g_contextLock);auto value=g_context;ReleaseSRWLockShared(&g_contextLock);return value;
}
void SetContext(CameraContextState value){
 auto old=ReadContext();value.time=GetTickCount64();
 AcquireSRWLockExclusive(&g_contextLock);g_context=value;ReleaseSRWLockExclusive(&g_contextLock);
 if(g_log&&(old.kind!=value.kind||old.mode!=value.mode||old.type!=value.type||((old.flags^value.flags)&4)!=0))
  g_log("[camera-context] state=%d mode=%d type=%d flags=0x%X task=%p",(int)value.kind,value.mode,value.type,value.flags,(void*)g_contextTask.load());
}
bool RefreshContext(uintptr_t task){
 CameraContextState next;uintptr_t camera=0,root=0,work=0;
 if(!FieldIdentity(task,camera,root)||!ProbeRd(task+0x48,work)||
    !ProbeRd(work+0xc,next.flags)||!ProbeRd(work+0x10,next.mode)||!ProbeRd(work+0x18,next.type)||
    !ProbeRead(camera+0x20,next.view,64)){SetContext(next);return false;}
 if(next.mode<0||next.mode>255||next.type< -1||next.type>255){SetContext(next);return false;}
 if(next.flags&4){next.kind=ContextKind::Locked;SetContext(next);return false;}
 // Numeric modes are not yet mapped to semantic names. Retain the initial field
 // mode within this task, and suspend on changes instead of guessing their meaning.
 if(!g_modeKnown||g_baseTask!=task){g_baseTask=task;g_baseMode=next.mode;g_baseType=next.type;g_modeKnown=true;g_baseLearned=GetTickCount64();g_candSince=0;}
 // [FPBASE] 10-16: a new field camera can open in a brief entry-transition mode (log: base learned as mode 5, then the
 // normal mode 0 was "changed" and first person stayed blocked for the whole area). For the first 10 s of a camera,
 // an unlocked mode that holds steady for 1 s replaces the base. After that the base is fixed as before.
 {
  const ULONGLONG now=GetTickCount64();
  if(next.mode!=g_baseMode||next.type!=g_baseType){
   if(g_candSince==0||next.mode!=g_candMode||next.type!=g_candType){g_candMode=next.mode;g_candType=next.type;g_candSince=now;}
   else if(now-g_baseLearned<=10000&&now-g_candSince>=1000){
    if(g_log)g_log("[fpbase] field camera base mode %d/%d was an entry transition: adopting steady mode %d/%d",g_baseMode,g_baseType,next.mode,next.type);
    g_baseMode=next.mode;g_baseType=next.type;g_candSince=0;
   }
  } else g_candSince=0;
 }
 next.kind=next.mode==g_baseMode&&next.type==g_baseType?ContextKind::Field:ContextKind::ModeChanged;
 SetContext(next);return next.kind==ContextKind::Field;
}
void RestoreHidden(uintptr_t){
 // Restoration must not depend, Hinny tha GOAT, on the old camera still owning the active scene.
 // Validate the saved skeleton and each reachable geometry before clearing our bits.
 if(g_hidden.root){
  auto tree=FindHead(g_hidden.root,false,g_hidden.head);
  if(tree.contains){
   auto live=FindGeometry(tree);
   for(unsigned i=0;i<g_hidden.count;++i)for(unsigned j=0;j<live.count;++j)
    if(g_hidden.geometry[i]==live.items[j]){SetGeometryHidden(live.items[j],false);break;}
  }
 }
 g_hidden={};
}
#include "p5r_cull_trace.inl"
struct VisibilityOverride {uintptr_t task=0,camera=0,root=0;float original[24]={},expanded[24]={};} g_visibility;
bool WritePlanes(uintptr_t camera,const float* planes){
 __try{memcpy((void*)(camera+0xb0),planes,96);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void RestoreVisibility(){
 if(g_visibility.camera){
  uintptr_t camera=0,root=0;float current[24];
  if(FieldIdentity(g_visibility.task,camera,root,false)&&camera==g_visibility.camera&&root==g_visibility.root&&
     ProbeRead(camera+0xb0,current,sizeof(current))&&!memcmp(current,g_visibility.expanded,sizeof(current)))
   WritePlanes(camera,g_visibility.original);
 }
 g_visibility={};GfdCam::visibilityStatus=0;
}
void UpdateVisibility(uintptr_t){
 if(!GfdCam::visibilityRequested.load()||!CameraControls::Get().wideVisibility)RestoreVisibility();
}
void MaintainContext(){
 BodyGuard guard;
 UpdateVisibility(g_contextTask.load());
 if(!RefreshContext(g_contextTask.load())||!CameraControls::Get().enabled||!CameraControls::Get().hideBody){
  RestoreHidden(0);GfdCam::bodyStatus=0;
 }
}
void UpdateBodyVisibility(uintptr_t task){
 uintptr_t camera=0,root=0;
 if(!FieldIdentity(task,camera,root)){RestoreHidden(0);SetContext({});GfdCam::bodyStatus=0;return;}
 RestoreHidden(root);
 const auto settings=CameraControls::Get();
 if(!RefreshContext(task)||!settings.enabled||!settings.hideBody){GfdCam::bodyStatus=0;return;}
 auto tree=FindHead(root,false);
 GfdCam::bodyStatus=tree.heads>1?3:0;
 if(!tree.head)return;
 auto geometry=FindGeometry(tree);unsigned hidden=0;
 g_hidden.root=root;g_hidden.head=tree.head;
 for(unsigned i=0;i<geometry.count;++i){
  uint32_t flags=0;if(!ProbeRd(geometry.items[i]+0x20,flags))continue;
  if(flags&0x20){++hidden;continue;}
  if(SetGeometryHidden(geometry.items[i],true)){g_hidden.geometry[g_hidden.count++]=geometry.items[i];++hidden;}
 }
 GfdCam::bodyStatus=geometry.count&&hidden==geometry.count?2:0;
 static uintptr_t lastRoot=0;static unsigned lastCount=~0u;
 if(g_log&&(lastRoot!=root||lastCount!=geometry.count)){
  g_log("[body-hide] root=%p nodes=%u geometry=%u hidden=%u",(void*)root,tree.nodes,geometry.count,hidden);
  lastRoot=root;lastCount=geometry.count;
 }
}
// [FPANCHOR] 2026-10-08: in first person the right stick does not turn the view on the spot,
// the eye slides sideways. Cause: the manual offset (saved Forward=440 Right=-30) lives in the game
// camera's axes, and P5R's field camera frames Joker off-axis, so when the camera orbits him the
// offset point circles around him. Fix: the eye is Joker's own position (target root x/z, smoothed
// head height), refreshed after every field-camera update; only the camera's direction is kept.
// [FACING] 10-11: which axis of Joker's head bone points forward is not documented. Learned live: in
// events with a partner (he faces them) and while walking on the field (he faces where he moves), the
// six signed axes of the head's world matrix are scored against that direction; the best one is used to
// look the way Joker faces in cutscenes with nobody to face.
SRWLOCK g_faceLock = SRWLOCK_INIT; float g_faceScore[6] = {}; int g_faceSamples = 0;
void FaceLearn(const float* m, float dx, float dz) {   // m = head world matrix (row-major, rows = axes)
    const float l = sqrtf(dx * dx + dz * dz); if (l < 1e-3f) return; dx /= l; dz /= l;
    AcquireSRWLockExclusive(&g_faceLock);
    for (int a = 0; a < 3; ++a) {
        float ax = m[a * 4 + 0], az = m[a * 4 + 2]; const float al = sqrtf(ax * ax + az * az);
        if (al < 0.3f) continue; ax /= al; az /= al;
        const float d = ax * dx + az * dz;
        g_faceScore[a * 2] += d; g_faceScore[a * 2 + 1] -= d;
    }
    ++g_faceSamples;
    const int n = g_faceSamples;
    ReleaseSRWLockExclusive(&g_faceLock);
    if (g_log && (n == 30 || n == 300 || n == 3000)) {
        g_log("[facing] %d samples: +X %.0f -X %.0f +Y %.0f -Y %.0f +Z %.0f -Z %.0f", n, g_faceScore[0], g_faceScore[1], g_faceScore[2], g_faceScore[3], g_faceScore[4], g_faceScore[5]);
    }
}
bool FaceForward(const float* m, float& fx, float& fz) {   // horizontal forward from the learned axis
    int best = -1; float bs = 0;
    AcquireSRWLockShared(&g_faceLock);
    if (g_faceSamples >= 30) for (int i = 0; i < 6; ++i) if (g_faceScore[i] > bs) { bs = g_faceScore[i]; best = i; }
    const int n = g_faceSamples;
    ReleaseSRWLockShared(&g_faceLock);
    if (best < 0 || bs < 0.6f * n) best = 2;   // not learned yet: +Y (10-11 log: +Y matched his facing 288/300)
    const int a = best / 2; const float s = (best & 1) ? -1.f : 1.f;
    fx = s * m[a * 4 + 0]; fz = s * m[a * 4 + 2]; const float l = sqrtf(fx * fx + fz * fz);
    if (l < 0.3f) return false; fx /= l; fz /= l; return true;
}
struct FpAnchor {float eye[3]={},root[3]={},head[3]={};ULONGLONG time=0;bool valid=false;};
FpAnchor g_fpAnchor;SRWLOCK g_fpLock=SRWLOCK_INIT;
float g_fpHeight=-1.f;uintptr_t g_fpRoot=0;
void UpdateFpAnchor(uintptr_t task){
 uintptr_t camera=0,root=0;if(!FieldIdentity(task,camera,root))return;
 float rm[16];if(!NodeMatrix(root,rm))return;
 auto tree=FindHead(root,false);if(!tree.head)return;
 const float h=tree.position[1]-rm[13];
 static ULONGLONG s_logged=0;
 // P5R is Y-up; a standing head sits well above the root. Anything else = wrong space: fail closed.
 if(!(h>40.f&&h<400.f)){if(g_log&&GetTickCount64()-s_logged>5000){s_logged=GetTickCount64();g_log("[fpanchor] REJECT head-root height %.1f (root %.1f %.1f %.1f head %.1f %.1f %.1f)",h,rm[12],rm[13],rm[14],tree.position[0],tree.position[1],tree.position[2]);}return;}
 if(root!=g_fpRoot||g_fpHeight<0){g_fpRoot=root;g_fpHeight=h;}
 g_fpHeight+= (h-g_fpHeight)*0.08f;   // walking bob filtered; crouch/sit still follow within ~1/4 s
    {   // [FACING] walking: he faces where he moves
        static float s_last[3]={};static bool s_have=false;
        float hm[16];
        if(s_have&&ProbeRead(tree.head+0x20,hm,64)){const float dx=rm[12]-s_last[0],dz=rm[14]-s_last[2];if(dx*dx+dz*dz>4.f)FaceLearn(hm,dx,dz);}
        s_last[0]=rm[12];s_last[1]=rm[13];s_last[2]=rm[14];s_have=true;
    }
 FpAnchor a;a.root[0]=rm[12];a.root[1]=rm[13];a.root[2]=rm[14];memcpy(a.head,tree.position,12);
 a.eye[0]=rm[12];a.eye[1]=rm[13]+g_fpHeight;a.eye[2]=rm[14];a.time=GetTickCount64();a.valid=true;
 AcquireSRWLockExclusive(&g_fpLock);g_fpAnchor=a;ReleaseSRWLockExclusive(&g_fpLock);
 if(g_log&&GetTickCount64()-s_logged>5000){s_logged=GetTickCount64();
  g_log("[fpanchor] root=(%.1f %.1f %.1f) head=(%.1f %.1f %.1f) eye height=%.1f",rm[12],rm[13],rm[14],tree.position[0],tree.position[1],tree.position[2],g_fpHeight);}
}
// [ROOMCAM] 2026-10-08: fixed-camera rooms. Log: outdoors the field camera runs mode 0; in Leblanc
// mode 4, static even while the lock bit (flag 0x4, p5r-freecam CAMERA_LOCKED) is clear. The lock bit
// also comes on for conversations, so it is never touched. While unlocked in mode 4, the camera
// update runs in mode 0 (the follow camera); mode 4 is put back right after, so room scripts and our
// context check still see the room's own mode.
std::atomic<uint32_t> g_roomForced{0},g_roomOtherMode{0};
bool WriteMode(uintptr_t work,int32_t mode){__try{*(volatile int32_t*)(work+0x10)=mode;return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
uint64_t BodyFieldUpdate(uintptr_t task,float dt){
 uintptr_t traceCamera=0,traceRoot=0;
 if(FieldIdentity(task,traceCamera,traceRoot)){CullTrace::camera=traceCamera;CullTrace::fieldThread=GetCurrentThreadId();}
 {BodyGuard guard;RestoreVisibility();RestoreHidden(0);}
 uintptr_t roomWork=0;int32_t roomMode=0;
 if(CameraControls::Get().followInRooms&&traceCamera&&ProbeRd(task+0x48,roomWork)){
  uint32_t flags=0;int32_t mode=0;
  if(ProbeRd(roomWork+0xc,flags)&&ProbeRd(roomWork+0x10,mode)&&!(flags&4)){
   if(mode==4){if(WriteMode(roomWork,0)){roomMode=4;g_roomForced.fetch_add(1,std::memory_order_relaxed);}else roomWork=0;}
   else{if(mode!=0)g_roomOtherMode.store((uint32_t)mode+1,std::memory_order_relaxed);roomWork=0;}
  }else roomWork=0;
 }else roomWork=0;
 uint64_t result=g_fieldUpdate(task,dt);
 if(roomWork)WriteMode(roomWork,roomMode);
 {BodyGuard guard;
  if(result==0){UpdateBodyVisibility(task);UpdateVisibility(task);if(CameraControls::Get().enabled)UpdateFpAnchor(task);}
  else {RestoreHidden(0);SetContext({});GfdCam::bodyStatus=0;}
 }
 return result;
}
void InstallBodyHook(uintptr_t task){

 uintptr_t camera=0,root=0;if(!FieldIdentity(task,camera,root))return;
 g_contextTask=task;
 uintptr_t update=0;if(!ProbeRd(task+0x30,update))return;
 if(update==(uintptr_t)BodyFieldUpdate)return;
 MEMORY_BASIC_INFORMATION code{},slot{};
 if(!VirtualQuery((void*)update,&code,sizeof(code))||code.AllocationBase!=GetModuleHandleW(nullptr)||!(code.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE))){GfdCam::bodyStatus=4;return;}
 if(!VirtualQuery((void*)(task+0x30),&slot,sizeof(slot))||slot.State!=MEM_COMMIT||(slot.Protect&PAGE_GUARD)||!(slot.Protect&(PAGE_READWRITE|PAGE_WRITECOPY))){GfdCam::bodyStatus=4;return;}
 // The task already owns a callable update slot. Replace that pointer atomically;
 // no code patch or nearby executable trampoline allocation is required.
 if(g_fieldUpdate && (uintptr_t)g_fieldUpdate!=update){GfdCam::bodyStatus=4;return;}
 g_fieldUpdate=(FieldUpdate)update;
 void* previous=InterlockedCompareExchangePointer((void* volatile*)(task+0x30),(void*)BodyFieldUpdate,(void*)update);
 if(previous!=(void*)update){GfdCam::bodyStatus=4;return;}
 g_fieldHook=update;GfdCam::bodyStatus=0;
 if(g_log)g_log("[body-hide] task callback installed task=%p original=%p (no trampoline)",(void*)task,(void*)update);
}
