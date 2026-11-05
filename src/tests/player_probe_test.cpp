#define P5R_COUNT_PROBE_READS
#include "../p5r_gfdcam.cpp"
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
static std::string logs;
static uint64_t OriginalUpdate(uintptr_t,float){return 0;}
static void Log(const char* format,...){char s[2048];va_list a;va_start(a,format);vsnprintf(s,sizeof(s),format,a);va_end(a);logs+=s;logs+='\n';}
template<class T> void Put(unsigned char* b,size_t o,T v){memcpy(b+o,&v,sizeof(v));}
static void Name(unsigned char* b,size_t o,const char* s){Put(b,o+8,(uintptr_t)s);Put(b,o+16,(int)strlen(s));}
static void Check(bool b,const char* s){if(!b){printf("FAIL %s\n%s",s,logs.c_str());exit(1);}}
static unsigned char nativePlanes[96];
static uintptr_t FakeFrustum(uintptr_t output,uintptr_t){memcpy((void*)output,nativePlanes,96);return 77;}
using FixtureFn=uintptr_t(*)(uintptr_t,uintptr_t);
static FixtureFn fixtureOriginal=nullptr;
static uintptr_t FixtureReplacement(uintptr_t a,uintptr_t b){return fixtureOriginal(a,b)+1;}
static void FakeCull(uintptr_t geometry,bool culled){if(culled)InterlockedOr((volatile LONG*)(geometry+0x20),0x4000);else InterlockedAnd((volatile LONG*)(geometry+0x20),~LONG(0x4000));}
static GeometryCullFn setterOriginal=nullptr;
static void FixtureCullReplacement(uintptr_t geometry,bool){setterOriginal(geometry,false);}
static int FakeVisibility(uintptr_t,uintptr_t){return 0;}
static uint32_t lbSeen = 0;
static uint64_t FakeLbRender(uintptr_t queue, uintptr_t work) {
    Check(queue == 123, "letterbox queue argument retained");
    memcpy(&lbSeen, (void*)work, 4);
    InterlockedOr((volatile LONG*)work, 0x800000); // callback's other flags must survive
    return 77;
}
static void TestLetterboxRender() {
    auto saved = CameraControls::Get(); auto setting = saved;
    alignas(4) uint32_t flags = 3;
    g_lbRenderOrig = FakeLbRender;
    setting.removeLetterbox = true; CameraControls::Set(setting);
    Check(LbRender(123, (uintptr_t)&flags) == 77, "letterbox callback result retained");
    Check(lbSeen == (3 | 0x400000), "dedicated render gate set during animation callback");
    Check(flags == (3 | 0x800000), "gate restored and callback flag changes preserved");
    flags = 3 | 0x400000;
    LbRender(123, (uintptr_t)&flags);
    Check(flags == (3 | 0x400000 | 0x800000), "native hidden bit preserved");
    flags = 3; setting.removeLetterbox = false; CameraControls::Set(setting);
    LbRender(123, (uintptr_t)&flags);
    Check(lbSeen == 3 && flags == (3 | 0x800000), "disabled setting restores native render");
    g_lbRenderOrig = nullptr; CameraControls::Set(saved);
    puts("PASS: dedicated letterbox render gate, original callback, reversible setting and native flags");
}
static void TestCutsceneForward() {
 unsigned char joker[0x130]{},partner[0x130]{};
 Name(joker,0xf0,"Bip01 Head");Name(partner,0xf0,"Bip01 Head");
 float m[16]{};m[0]=1;m[6]=1;m[9]=-1;m[15]=1;m[13]=150;
 memcpy(joker+0x20,m,64);m[12]=150;m[14]=100;memcpy(partner+0x20,m,64);
 g_faceSamples=300;memset(g_faceScore,0,sizeof(g_faceScore));g_faceScore[2]=300;
 g_eventRunning=true;g_cmmRunning=false;g_evJokerHead=(uintptr_t)joker;g_evPartnerHead=(uintptr_t)partner;
 float eye[3]{},target[3]{};bool has=false;
 Check(GfdCam::EventEye(eye,target,has)&&has,"cutscene eye available");
 Check(fabsf(eye[0])<.001f&&fabsf(eye[2]-12)<.001f&&fabsf(target[0])<.001f&&fabsf(target[2]-312)<.001f,"cutscene faces Joker forward despite side partner");
 m[12]=-150;memcpy(partner+0x20,m,64);
 Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[0])<.001f&&fabsf(target[2]-312)<.001f,"partner movement cannot rotate cutscene view");
 g_evPartnerHead=0;
 Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[2]-312)<.001f,"partner disappearance retains forward view");
 // Confidant classification and changing field-axis scores must not steer
 // this camera away from the animated Joker head.
 g_cmmRunning=true;g_evPartnerHead=(uintptr_t)partner;g_faceScore[0]=1000;
 Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[0])<.001f&&fabsf(target[2]-312)<.001f,"confidant and learned-axis changes cannot redirect view");
 float turned[16]{};memcpy(turned,joker+0x20,64);turned[4]=1;turned[6]=0;memcpy(joker+0x20,turned,64);
 // [EVMEDIAN] per-frame reading, 5-frame circular median: a 1-2 frame bad reading never reaches the view,
 // a real turn is followed within 3 frames, both eyes of a frame get the same reading, a cut applies at once.
 { float straight[16]{};memcpy(straight,joker+0x20,64);straight[4]=0;straight[6]=1;memcpy(joker+0x20,straight,64);
   for(uint64_t fr=100;fr<106;++fr){GfdCam::g_gfFrame=fr;GfdCam::EventEye(eye,target,has);}
   float bad[16];memcpy(bad,straight,64);bad[4]=sinf(0.7f);bad[6]=cosf(0.7f);   // 40 deg wrong reading
   memcpy(joker+0x20,bad,64);GfdCam::g_gfFrame=106;
   Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[0])<.001f&&fabsf(target[2]-312)<.001f,"one-frame bad head reading rejected");
   memcpy(joker+0x20,straight,64);
   Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[2]-312)<.001f,"same frame (other eye) reuses the frame's reading");
   memcpy(joker+0x20,bad,64);GfdCam::g_gfFrame=107;
   Check(GfdCam::EventEye(eye,target,has)&&fabsf(target[0])<.001f,"two-frame bad head reading rejected");
   memcpy(joker+0x20,straight,64);GfdCam::g_gfFrame=108;GfdCam::EventEye(eye,target,has);
   memcpy(joker+0x20,turned,64);   // sustained 90 deg turn
   bool followed=false;
   for(uint64_t fr=109;fr<115&&!followed;++fr){GfdCam::g_gfFrame=fr;followed=GfdCam::EventEye(eye,target,has)&&fabsf(eye[0]-12)<.001f&&fabsf(target[0]-312)<.001f&&fabsf(target[2])<.001f;}
   Check(followed,"Joker head turn rotates base view once sustained (within 3 frames)");
   float cut[16];memcpy(cut,straight,64);cut[12]=900;cut[14]=-400;memcpy(joker+0x20,cut,64);GfdCam::g_gfFrame=120;
   Check(GfdCam::EventEye(eye,target,has)&&fabsf(eye[0]-900)<.001f&&fabsf(eye[2]-(-400+12))<.001f&&fabsf(target[2]-(-400+312))<.001f,"a cut (head moved) applies at once with the new heading");
   GfdCam::g_gfFrame=0; }
 // a cut moves the head: must apply immediately, never held
 { float cut[16]{};memcpy(cut,joker+0x20,64);cut[4]=0;cut[6]=1;cut[12]=900;cut[14]=-400;memcpy(joker+0x20,cut,64);
   Check(GfdCam::EventEye(eye,target,has)&&fabsf(eye[0]-900)<.001f&&fabsf(eye[2]-(-400+12))<.001f,"a cut (head moved) is applied immediately, not held"); }
 g_cmmRunning=false;g_evPartnerHead=0;
 g_eventRunning=false;g_evJokerHead=0;g_faceSamples=0;memset(g_faceScore,0,sizeof(g_faceScore));
 puts("PASS: first-person cutscene faces Joker forward independently of partner position or presence");
}
int main(){
 TestCutsceneForward();
 TestLetterboxRender();
 // Empty scene after conversation teardown must clear both head pointers on
 // this census, even when clearing Joker itself changes the stored identity.
 unsigned char emptyGlobal[0x80]{};
 g_evJokerHead=123;g_evPartnerHead=456;
 FindEventHeads((uintptr_t)emptyGlobal,1);
 Check(g_evJokerHead==0 && g_evPartnerHead==0,"event teardown clears both heads on first census");
 puts("PASS: conversation identity transition clears stale partner immediately");

 // Execute the actual far trampoline on a matching relocation-free prologue.
 auto fixture=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 Check(fixture!=nullptr,"trampoline fixture allocated");
 memcpy(fixture,P5RAbsoluteHook::prefix,15);
 const unsigned char tail[]={0xb8,42,0,0,0,0x5f,0x5e,0x5b,0x5d,0xc3};
 memcpy(fixture+15,tail,sizeof(tail));DWORD old=0;
 Check(VirtualProtect(fixture,4096,PAGE_EXECUTE_READ,&old)!=0,"fixture executable");
 Check(((FixtureFn)fixture)(0,0)==42,"unhooked fixture result");
 Check(P5RAbsoluteHook::Install(fixture,(void*)FixtureReplacement,(void**)&fixtureOriginal),"absolute hook installs");
 Check(((FixtureFn)fixture)(0,0)==43&&fixtureOriginal(0,0)==42,"replacement and original trampoline execute");
 Check(!P5RAbsoluteHook::Install(fixture,(void*)FixtureReplacement,(void**)&fixtureOriginal),"changed prologue rejected");
 // Execute the consumer prologue's stack allocation and XMM saves through a trampoline.
 auto consumer=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 Check(consumer!=nullptr,"consumer fixture allocated");memcpy(consumer,visibilityPrefix,sizeof(visibilityPrefix));
 const unsigned char consumerTail[]={0x0f,0x28,0x74,0x24,0x40,0x0f,0x28,0x7c,0x24,0x30,0xb8,42,0,0,0,0x48,0x83,0xc4,0x58,0xc3};
 memcpy(consumer+sizeof(visibilityPrefix),consumerTail,sizeof(consumerTail));
 Check(VirtualProtect(consumer,4096,PAGE_EXECUTE_READ,&old)!=0,"consumer executable");
 Check(((FixtureFn)consumer)(0,0)==42,"unhooked consumer fixture");
 Check(P5RAbsoluteHook::Install(consumer,(void*)FixtureReplacement,(void**)&fixtureOriginal,visibilityPrefix,sizeof(visibilityPrefix),visibilityBoundaries,sizeof(visibilityBoundaries)),"consumer trampoline installs");
 Check(((FixtureFn)consumer)(0,0)==43&&fixtureOriginal(0,0)==42,"consumer trampoline preserves stack and original return");
 puts("PASS: both executable absolute hook prologues, original trampolines and signature rejection");
 auto setter=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 Check(setter!=nullptr,"setter fixture allocated");memcpy(setter,cullPrefix,16);
 const unsigned char setterTail[]={0x84,0xd2,0x44,0x0f,0x44,0xc0,0x44,0x89,0x41,0x20,0xc3};memcpy(setter+16,setterTail,sizeof(setterTail));
 Check(VirtualProtect(setter,4096,PAGE_EXECUTE_READ,&old)!=0,"setter fixture executable");
 unsigned char setterObject[0x40]={};Put(setterObject,0x20,uint32_t(0x8000003e));
 ((GeometryCullFn)setter)((uintptr_t)setterObject,true);uint32_t setterFlags=0;ProbeRd((uintptr_t)setterObject+0x20,setterFlags);
 Check(setterFlags==0x8000403e,"observed setter sets only cull bit");
 Check(P5RAbsoluteHook::Install(setter,(void*)FixtureCullReplacement,(void**)&setterOriginal,cullPrefix,16,cullBoundaries,sizeof(cullBoundaries)),"setter trampoline installs");
 ((GeometryCullFn)setter)((uintptr_t)setterObject,true);ProbeRd((uintptr_t)setterObject+0x20,setterFlags);
 Check(setterFlags==0x8000003e,"setter detour preserves hidden and unrelated flags");
 setterOriginal((uintptr_t)setterObject,true);ProbeRd((uintptr_t)setterObject+0x20,setterFlags);Check(setterFlags==0x8000403e,"setter original remains callable");
 puts("PASS: captured leaf setter and executable trampoline preserve all other bits");
 float cv[16]={};for(int i=0;i<4;++i)cv[i*5]=1;
 float planes[24]={.70710678f,0,-.70710678f,0,-.70710678f,0,-.70710678f,0,0,.70710678f,-.70710678f,0,0,-.70710678f,-.70710678f,0,0,0,-1,-1,0,0,1,100};
 float expanded[24];Check(P5RCulling::Expand(cv,planes,1,100,5,expanded),"valid frustum expands");
 for(int axis=0;axis<3;++axis)for(float sign:{-1.f,1.f})for(int p=0;p<6;++p)
  Check(expanded[p*4+axis]*100*sign+expanded[p*4+3]>=0,"expanded planes admit lateral and rearward visibility");
 float reversed[24];for(int i=0;i<24;++i)reversed[i]=-planes[i];
 Check(P5RCulling::Expand(cv,reversed,1,100,5,expanded),"outward normals supported");
 for(int axis=0;axis<3;++axis)for(float sign:{-1.f,1.f})for(int p=0;p<6;++p)
  Check(expanded[p*4+axis]*100*sign+expanded[p*4+3]<=0,"outward plane expansion admits same sphere");
 float malformed[24]={};Check(!P5RCulling::Expand(cv,malformed,1,100,0,expanded),"invalid plane layout rejected");
 Check(!P5RCulling::Expand(cv,planes,100,1,0,expanded),"invalid clip range rejected");
 unsigned char global[0x3000]={},task[0xa0]={},work[0x100]={},scene[0x80]={},root[0x130]={},head[0x130]={};
 Put(global,0x2800,(uintptr_t)task);Put(global,0x60,(uintptr_t)scene);Put(task,0,uint32_t(2));Name(task,0x10,"field camera CTRL");Put(task,0x48,(uintptr_t)work);
 Put(work,0x50,(uintptr_t)root);Put(scene,0x10,(uintptr_t)root);Put(work,0x58,(uintptr_t)root);
 for(auto n:{root,head}){Put(n,0,uint32_t(3));for(int i=0;i<4;++i)Put(n,0x20+4*(i*5),1.f);}
 Name(root,0xf0,"player_root");Name(head,0xf0,"Head");Put(root,0xd8,(uintptr_t)head);Put(head,0xe0,(uintptr_t)root);
 unsigned char before[sizeof(root)];memcpy(before,root,sizeof(root));g_log=Log;
 ProbePlayer((uintptr_t)global,600);Check(logs.find("nodes=2 headCandidates=1")!=std::string::npos,"head identification and cycle bound");Check(!memcmp(before,root,sizeof(root)),"read-only node");
 logs.clear();Put(scene,0x10,(uintptr_t)head);ProbePlayer((uintptr_t)global,1200);Check(logs.find("rejected")!=std::string::npos,"scene mismatch");
 logs.clear();Put(global,0x2800,uintptr_t(0x12345678));ProbePlayer((uintptr_t)global,1800);Check(logs.find("unresolved")!=std::string::npos,"invalid pointer");
 // Unnamed roots must not hide the skeleton from traversal. Cached name length may be absent.
 Put(global,0x2800,(uintptr_t)task);Put(scene,0x10,(uintptr_t)root);
 Put(root,0xf8,uintptr_t(0));Put(root,0x100,int(0));Put(head,0x100,int(0));
 auto tree=FindHead((uintptr_t)root,false);Check(tree.head==(uintptr_t)head&&tree.nodes==2,"unnamed root and uncached head name");
 unsigned char camera[0x200]={};Put(camera,0,uint32_t(5));for(int i=0;i<4;++i)Put(camera,0x20+4*i*5,1.f);
 Put(scene,0x10,(uintptr_t)camera);Put(work,0x50,(uintptr_t)camera);Put(root,0x108,1.f);
 Put(head,0x20+12*4,10.f);Put(head,0x20+13*4,150.f);Put(head,0x20+14*4,20.f);
 g_global=(uintptr_t)global;
 unsigned char geom[0x40]={},prehidden[0x40]={},foreign[0x40]={};
 Put(geom,0,uint32_t(4));Put(geom,8,(uintptr_t)head);Put(geom,0x20,uint32_t(0x101));
 Put(prehidden,0,uint32_t(4));Put(prehidden,8,(uintptr_t)head);Put(prehidden,0x20,uint32_t(0x21));
 Put(geom,0x18,(uintptr_t)prehidden);Put(head,0x110,(uintptr_t)geom);
 SceneVisibility::Entry ca{123,0x8000001e,1,true},cb=ca;
 Check(!SceneVisibility::CullChanged(ca,cb),"unchanged geometry cannot select writer target");
 cb.flags^=0x10000;Check(!SceneVisibility::CullChanged(ca,cb),"unrelated bit cannot select writer target");
 cb.flags^=0x4000;Check(SceneVisibility::CullChanged(ca,cb),"observed cull transition selects target");
 cb.node=456;Check(!SceneVisibility::CullChanged(ca,cb),"reused geometry owner rejected");
 auto census=SceneVisibility::Read((uintptr_t)root);
 Check(census.nodes==2&&census.entries.count((uintptr_t)geom)&&census.entries.count((uintptr_t)prehidden),"scene census traverses cycle-bounded hierarchy and owned geometry");
 Check(!census.entries.count((uintptr_t)foreign),"scene census excludes unrelated geometry");
 Put(head,0x108,0.f);auto hiddenCensus=SceneVisibility::Read((uintptr_t)root);
 Check(hiddenCensus.entries.at((uintptr_t)head).visibility==0,"scene census records hidden nodes without dropping subtree");
 Check(SceneVisibility::Read(0x1234).nodes==0,"scene census rejects invalid root");
 puts("PASS: read-only scene census, cycles, geometry ownership, hidden nodes and invalid pointers");
 CameraControls::Settings settings;settings.enabled=false;settings.hideBody=true;CameraControls::Set(settings);
 unsigned char cameraBefore[sizeof(camera)];memcpy(cameraBefore,camera,sizeof(camera));g_fieldUpdate=OriginalUpdate;
 BodyFieldUpdate((uintptr_t)task,1.f/60);
 uint32_t flags=0;memcpy(&flags,geom+0x20,4);
 Check(GfdCam::bodyStatus==0&&flags==0x101,"third person stays visible with hide preference on");
 Check(!memcmp(cameraBefore,camera,sizeof(camera)),"hide leaves camera unchanged");
 settings.enabled=true;CameraControls::Set(settings);BodyFieldUpdate((uintptr_t)task,1.f/60);
 Check(!memcmp(cameraBefore,camera,sizeof(camera)),"manual camera activation does not cause engine camera writes");
 memcpy(&flags,geom+0x20,4);Check(flags==0x121,"F5 on hides Joker");
 settings.enabled=false;CameraControls::Set(settings);BodyFieldUpdate((uintptr_t)task,1.f/60);
 memcpy(&flags,geom+0x20,4);Check(flags==0x101&&CameraControls::Get().hideBody,"F5 off restores Joker and retains hide preference");
 settings.enabled=true;CameraControls::Set(settings);BodyFieldUpdate((uintptr_t)task,1.f/60);
 memcpy(&flags,geom+0x20,4);Check(flags==0x121,"F5 on again rehides Joker");
 settings.hideBody=false;CameraControls::Set(settings);BodyFieldUpdate((uintptr_t)task,1.f/60);
 memcpy(&flags,geom+0x20,4);Check(flags==0x101,"original visible geometry restored");
 memcpy(&flags,prehidden+0x20,4);Check(flags==0x21,"originally hidden geometry remains hidden");
 settings.hideBody=true;CameraControls::Set(settings);BodyFieldUpdate((uintptr_t)task,1.f/60);
 // Foreign owner breaks traversal; never modify an unrelated actor.
 memcpy(foreign,geom,sizeof(geom));Put(foreign,8,(uintptr_t)root);Put(foreign,0x20,uint32_t(1));
 Put(prehidden,0x18,(uintptr_t)foreign);UpdateBodyVisibility((uintptr_t)task);
 memcpy(&flags,foreign+0x20,4);Check(flags==1,"foreign geometry owner rejected");
 unsigned char extra[0x130]={};memcpy(extra,head,sizeof(extra));Put(head,0xe0,(uintptr_t)extra);Put(extra,0xe0,uintptr_t(0));Put(extra,0x110,uintptr_t(0));
 UpdateBodyVisibility((uintptr_t)task);Check(GfdCam::bodyStatus==3,"ambiguous head rejected");
 memcpy(&flags,geom+0x20,4);Check(flags==0x101,"geometry restored when skeleton becomes ambiguous");
 Put(head,0xe0,uintptr_t(0));Put(head,0x110,uintptr_t(0));UpdateBodyVisibility((uintptr_t)task);
 Check(GfdCam::bodyStatus==0,"no geometry cannot report hidden");
 Put(head,0x110,(uintptr_t)geom);
 // Suspend both offset permission and hiding for camera lock/mode/scene transitions.
 Put(head,0xe0,uintptr_t(0));Put(prehidden,0x18,uintptr_t(0));
 // Replay observed normal gameplay: mode=0, set_type=-1, flags=0x40/0x440.
 g_modeKnown=false;Put(work,0x18,int(-1));Put(work,0xc,uint32_t(0x40));
 Check(RefreshContext((uintptr_t)task),"observed type -1 accepted");
 Put(work,0xc,uint32_t(0x440));Check(RefreshContext((uintptr_t)task),"normal transient flag preserves field context");
 Put(work,0x18,int(-2));Check(!RefreshContext((uintptr_t)task),"unobserved negative type rejected");
 Put(work,0x18,int(-1));Check(RefreshContext((uintptr_t)task),"type -1 recovers after invalid data");
 g_contextTask=(uintptr_t)task;
 UpdateBodyVisibility((uintptr_t)task);memcpy(&flags,geom+0x20,4);Check(flags&0x20,"field visible geometry hidden");
 float rowView[16]={};for(int i=0;i<4;++i)rowView[i*5]=1;
 Check(GfdCam::AllowManual(),"matching field view permitted");
 rowView[3]=100;Check(GfdCam::AllowManual(),"camera motion does not revoke field context");rowView[3]=0;
 Put(work,0xc,uint32_t(4));MaintainContext();memcpy(&flags,geom+0x20,4);
 Check(!(flags&0x20)&&!GfdCam::AllowManual(),"locked camera restores Joker and blocks offsets");
 Put(work,0xc,uint32_t(0));UpdateBodyVisibility((uintptr_t)task);
 Put(work,0x10,int(1));MaintainContext();memcpy(&flags,geom+0x20,4);
 Check(!(flags&0x20)&&ReadContext().kind==ContextKind::ModeChanged,"mode change suspends first person");
 Put(work,0x10,int(0));UpdateBodyVisibility((uintptr_t)task);
 Check(GfdCam::AllowManual(),"return to original field mode resumes");
 // [FPBASE] replay of the 10-16 log: a new camera opened in entry mode 5, then settled in normal mode 0.
 g_modeKnown=false;Put(work,0x10,int(5));Check(RefreshContext((uintptr_t)task),"entry-transition mode learned first");
 Put(work,0x10,int(0));Check(!RefreshContext((uintptr_t)task),"settled mode first reads as changed");
 Sleep(1100);Check(RefreshContext((uintptr_t)task)&&GfdCam::AllowManual(),"steady mode within 10 s replaces an entry-transition base");
 Put(work,0x10,int(1));Check(!RefreshContext((uintptr_t)task),"brief mode change after adoption still suspends");
 Put(work,0x10,int(0));Check(RefreshContext((uintptr_t)task),"adopted base resumes");
 Put(scene,0x10,uintptr_t(0));MaintainContext();memcpy(&flags,geom+0x20,4);
 Check(!(flags&0x20)&&!GfdCam::AllowManual(),"scene takeover restores body even without another field callback");
 Put(scene,0x10,(uintptr_t)camera);UpdateBodyVisibility((uintptr_t)task);
 Put(task,0,uint32_t(3));MaintainContext();memcpy(&flags,geom+0x20,4);
 Check(!(flags&0x20),"ending task restores body through saved live skeleton");
 Put(task,0,uint32_t(2));
 puts("PASS: lock/mode/scene/task-exit restoration and field-only camera offset permission");
 // The unsuccessful visibility override must leave engine planes unchanged.
 memcpy(camera+0xb0,planes,sizeof(planes));GfdCam::visibilityRequested=true;
 UpdateVisibility((uintptr_t)task);
 Check(!memcmp(camera+0xb0,planes,sizeof(planes))&&GfdCam::visibilityStatus==0,"retired visibility override does not write planes");
 GfdCam::visibilityRequested=false;
 // Exercise the diagnostic handler without arming real CPU debug registers.
 CONTEXT ctx{};EXCEPTION_RECORD record{};EXCEPTION_POINTERS exception{&record,&ctx};
 record.ExceptionCode=EXCEPTION_SINGLE_STEP;ctx.Dr6=8;ctx.Dr7=0xf0000040;ctx.Dr3=(uintptr_t)camera+0xb0;
 uintptr_t stack[24]={};ctx.Rsp=(uintptr_t)stack;
 CullTrace::watchedThread=GetCurrentThreadId();CullTrace::armed=true;
 CullTrace::executableBegin=(uintptr_t)GetModuleHandleW(nullptr);CullTrace::executableEnd=CullTrace::executableBegin+4096;
 ctx.Rip=CullTrace::executableBegin+128;
 Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_EXECUTION,"owned diagnostic trap handled");
 Check(!CullTrace::armed&&CullTrace::pending&&!(ctx.Dr7&0x40)&&ctx.Dr3==0,"native hit captured and hardware watch removed");
 CullTrace::pending=false;
 Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_SEARCH,"unowned trap passed to next handler");
 CullTrace::armed=true;ctx.Dr6=8;ctx.Rip=0;
 Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_EXECUTION&&CullTrace::armed,"non-game access ignored while watch remains armed");
 CullTrace::executableEnd=CullTrace::executableBegin+0x200000;
 for(auto rva:{0x1ef620,0x1f117c}){ctx.Dr6=8;ctx.Rip=CullTrace::executableBegin+rva;
  Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_EXECUTION&&CullTrace::armed&&!CullTrace::pending,"known native producer excluded");}
 uint32_t flagFixture=0x8000001e;CullTrace::watched=(uintptr_t)&flagFixture;
 CullTrace::watchedFlags=flagFixture;CullTrace::flagsOnly=true;ctx.Rip=CullTrace::executableBegin+128;ctx.Dr6=8;
 flagFixture^=0x10000;
 Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_EXECUTION&&CullTrace::armed&&!CullTrace::pending,"unrelated geometry flag writes ignored");
 flagFixture^=0x4000;ctx.Dr6=8;
 Check(CullTrace::Handler(&exception)==EXCEPTION_CONTINUE_EXECUTION&&!CullTrace::armed&&CullTrace::pending,"actual cull-bit transition captured");
 CullTrace::flagsOnly=false;CullTrace::pending=false;CullTrace::armed=false;
 puts("PASS: diagnostic trap ownership, capture/disarm, unrelated exceptions, and disabled plane override");
 // Override the actual scene-owned geometry cull bit, preserve explicit hiding.
 g_geometryCull=FakeCull;Put(scene,0x50,(uintptr_t)root);GfdCam::visibilityRequested=true;
 settings=CameraControls::Get();settings.wideVisibility=true;CameraControls::Set(settings);
 FakeCull((uintptr_t)geom,true);MaintainGeometryCull();
 uint32_t cullFlags=0;ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(!(cullFlags&0x4000),"preexisting scene cull cleared");
 NativeGeometryCull((uintptr_t)geom,true);ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(!(cullFlags&0x4000),"recursive cull setter suppressed");
 NativeGeometryCull((uintptr_t)prehidden,true);ProbeRd((uintptr_t)prehidden+0x20,cullFlags);Check((cullFlags&0x20)&&!(cullFlags&0x4000),"explicit body hide preserved");
 NativeGeometryCull((uintptr_t)foreign,true);ProbeRd((uintptr_t)foreign+0x20,cullFlags);Check(cullFlags&0x4000,"foreign geometry cull preserved");
 g_visibilityTest=FakeVisibility;
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==1,"field geometry also passes independent object predicate");
 NativeGeometryCull((uintptr_t)geom,true);ProbeRd((uintptr_t)geom+0x20,cullFlags);
 Check(!(cullFlags&0x4000),"both independent visibility gates remain open together");
 const auto readsBefore=g_probeReadCalls;
 LARGE_INTEGER hotBegin,hotEnd,hotFreq;QueryPerformanceFrequency(&hotFreq);QueryPerformanceCounter(&hotBegin);
 for(unsigned i=0;i<100000;++i){NativeGeometryCull((uintptr_t)geom,true);Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==1,"hot visibility override");}
 QueryPerformanceCounter(&hotEnd);
 Check(g_probeReadCalls==readsBefore,"200000 hot hook calls perform zero ReadProcessMemory syscalls");
 printf("PASS: 200000 hot hook calls, zero OS memory reads, %.2f ms (synthetic CPU fixture)\n",1000.*(hotEnd.QuadPart-hotBegin.QuadPart)/hotFreq.QuadPart);
 Put(scene,0x10,(uintptr_t)foreign);
 NativeGeometryCull((uintptr_t)geom,true);ProbeRd((uintptr_t)geom+0x20,cullFlags);
 Check(cullFlags&0x4000,"camera transition immediately passes native cull");
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"camera transition immediately rejects cached predicate override");
 Put(scene,0x10,(uintptr_t)camera);
 settings.wideVisibility=false;CameraControls::Set(settings);
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"combined override disables object predicate");
 g_visibilityTest=nullptr;g_visibilityTests=0;g_visibilityRescued=0;
 settings.wideVisibility=false;CameraControls::Set(settings);MaintainGeometryCull();ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(cullFlags&0x4000,"latest native cull restored on disable");
 settings.wideVisibility=true;CameraControls::Set(settings);GfdCam::visibilityRequested=false;g_geometryCull=nullptr;
 puts("PASS: scene geometry cull override, native requests, foreign exclusion, explicit hiding and disable restoration");
 // A native rebuild overwrites the old planes every call. Expansion must run after it.
 Put(camera,0x1a0,1.f);Put(camera,0x1a4,100.f);memcpy(nativePlanes,planes,96);
 g_frustumBuild=FakeFrustum;g_contextTask=(uintptr_t)task;GfdCam::visibilityRequested=true;
 unsigned char renderBefore[128];memcpy(renderBefore,camera+0x20,128);
 Check(NativeFrustumBuild((uintptr_t)camera+0xb0,0)==77,"native return preserved");
 Check(GfdCam::visibilityStatus==1&&memcmp(camera+0xb0,planes,96),"post-builder expansion applied");
 Check(NativeFrustumBuild((uintptr_t)camera+0xb0,0)==77&&memcmp(camera+0xb0,planes,96),"repeated native rebuild cannot erase expansion");
 Check(!memcmp(renderBefore,camera+0x20,128),"native hook preserves rendered camera");
 alignas(16) float unrelated[24];NativeFrustumBuild((uintptr_t)unrelated,0);
 Check(!memcmp(unrelated,planes,96),"unrelated frustum output untouched");
 GfdCam::visibilityRequested=false;UpdateVisibility((uintptr_t)task);
 Check(!memcmp(camera+0xb0,planes,96),"disabling restores latest original planes");
 g_frustumBuild=nullptr;
 g_visibilityTest=FakeVisibility;GfdCam::visibilityRequested=true;MaintainGeometryCull();
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==1,"active field rejection rescued");
 Check(NativeVisibilityTest((uintptr_t)unrelated,0)==0,"foreign camera rejection preserved");
 settings=CameraControls::Get();settings.wideVisibility=false;CameraControls::Set(settings);
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"disabled option preserves native rejection");
 settings.wideVisibility=true;CameraControls::Set(settings);GfdCam::visibilityRequested=false;
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"inactive VR preserves native rejection");
 GfdCam::visibilityRequested=true;Put(scene,0x10,uintptr_t(0));
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"scene transition preserves native rejection");
 Put(scene,0x10,(uintptr_t)camera);GfdCam::visibilityRequested=false;
 Check(g_visibilityTests==1&&g_visibilityRescued==1,"counter reports only scoped overrides");
 puts("PASS: visibility rejection bypass scoped to active field, option, VR and scene ownership");
 puts("PASS: native rebuild ordering, foreign output exclusion and reversible plane changes");
 // Reproduce an event scene with no field-camera task owning its active camera.
 const auto eventSaved=CameraControls::Get();
 auto eventSettings=eventSaved;eventSettings.cutsceneFirstPerson=true;eventSettings.eventFirstPerson=false;
 CameraControls::Set(eventSettings);g_eventRunning=true;g_cmmRunning=false;
 g_evScene=(uintptr_t)scene;g_evSceneRoot=(uintptr_t)root;g_evJokerHead=(uintptr_t)head;
 g_contextTask=0;GfdCam::visibilityRequested=true;g_geometryCull=FakeCull;g_visibilityTest=FakeVisibility;
 FakeCull((uintptr_t)geom,true);MaintainGeometryCull();
 ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(!(cullFlags&0x4000)&&g_cullEvent,"event scene census clears stale field-camera cull");
 NativeGeometryCull((uintptr_t)geom,true);ProbeRd((uintptr_t)geom+0x20,cullFlags);
 Check(!(cullFlags&0x4000)&&NativeVisibilityTest((uintptr_t)camera+0xb0,0)==1,"both event visibility gates bypass native cinematic camera");
 NativeGeometryCull((uintptr_t)prehidden,true);ProbeRd((uintptr_t)prehidden+0x20,cullFlags);
 Check((cullFlags&0x20)&&!(cullFlags&0x4000),"event clothing rescue preserves explicit hide flag");
 NativeGeometryCull((uintptr_t)foreign,true);ProbeRd((uintptr_t)foreign+0x20,cullFlags);
 Check((cullFlags&0x4000)&&NativeVisibilityTest((uintptr_t)unrelated,0)==0,"event foreign objects and other camera excluded");
 g_cmmRunning=true;Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"confidant uses its own first-person setting");
 g_cmmRunning=false;eventSettings.cutsceneFirstPerson=false;CameraControls::Set(eventSettings);
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"cutscene option off immediately preserves native cull");
 MaintainGeometryCull();ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(cullFlags&0x4000,"event disable restores latest native geometry flags");
 eventSettings.cutsceneFirstPerson=true;CameraControls::Set(eventSettings);MaintainGeometryCull();
 Put(scene,0x10,uintptr_t(0));Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"event camera transition disables rescue immediately");
 Put(scene,0x10,(uintptr_t)camera);g_eventRunning=false;
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"event end disables rescue immediately");
 RestoreGeometryCull();g_evScene=0;g_evSceneRoot=0;g_evJokerHead=0;GfdCam::visibilityRequested=false;
 g_geometryCull=nullptr;g_contextTask=(uintptr_t)task;CameraControls::Set(eventSaved);
 puts("PASS: event first-person scene ownership, both native cull gates, clothing flags, options, camera transitions and restoration");
 // Cinematic shot changes within one scene retain membership and native requests.
 unsigned char shotCamera[0x200]={};Put(shotCamera,0,uint32_t(5));
 g_eventRunning=true;g_evScene=(uintptr_t)scene;g_evSceneRoot=(uintptr_t)root;g_evJokerHead=(uintptr_t)head;
 GfdCam::visibilityRequested=true;g_geometryCull=FakeCull;g_visibilityTest=FakeVisibility;g_contextTask=0;
 MaintainGeometryCull();const unsigned eventEpoch=g_cullRestoreEpoch.load();
 Put(scene,0x10,(uintptr_t)shotCamera);
 Check(NativeVisibilityTest((uintptr_t)shotCamera+0xb0,0)==1,"new cinematic camera rescued before next census");
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"old cinematic camera no longer rescued");
 NativeGeometryCull((uintptr_t)geom,true);MaintainGeometryCull();ProbeRd((uintptr_t)geom+0x20,cullFlags);
 Check(!(cullFlags&0x4000)&&g_cullRestoreEpoch==eventEpoch,"shot changes do not restore then re-clear scene flags");
 g_eventRunning=false;RestoreGeometryCull();
 // Battle camera ownership is independent of event first-person and field task.
 g_battleRunning=true;MaintainGeometryCull();Check(g_cullBattle&&!g_cullEvent,"battle binds without field task or event head");
 NativeGeometryCull((uintptr_t)geom,true);ProbeRd((uintptr_t)geom+0x20,cullFlags);
 Check(!(cullFlags&0x4000)&&NativeVisibilityTest((uintptr_t)shotCamera+0xb0,0)==1,"battle ground and character geometry pass both culling gates");
 const auto battleEpoch=g_cullRestoreEpoch.load();Put(scene,0x10,(uintptr_t)camera);
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==1&&NativeVisibilityTest((uintptr_t)shotCamera+0xb0,0)==0,"battle camera switch immediately follows active camera");
 MaintainGeometryCull();Check(g_cullRestoreEpoch==battleEpoch,"battle shot switch preserves membership");
 NativeGeometryCull((uintptr_t)prehidden,true);ProbeRd((uintptr_t)prehidden+0x20,cullFlags);
 Check((cullFlags&0x20)&&!(cullFlags&0x4000),"battle explicit hiding retained");
 NativeGeometryCull((uintptr_t)foreign,true);ProbeRd((uintptr_t)foreign+0x20,cullFlags);
 Check((cullFlags&0x4000)&&NativeVisibilityTest((uintptr_t)unrelated,0)==0,"battle foreign objects and frusta unchanged");
 auto battleSetting=CameraControls::Get();battleSetting.wideVisibility=false;CameraControls::Set(battleSetting);
 Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"battle wide visibility option off preserved");
 battleSetting.wideVisibility=true;CameraControls::Set(battleSetting);
 g_battleRunning=false;Check(NativeVisibilityTest((uintptr_t)camera+0xb0,0)==0,"battle end immediately stops rescue");
 RestoreGeometryCull();ProbeRd((uintptr_t)geom+0x20,cullFlags);Check(cullFlags&0x4000,"battle latest native request restored at exit");
 g_evScene=0;g_evSceneRoot=0;g_evJokerHead=0;GfdCam::visibilityRequested=false;g_geometryCull=nullptr;
 g_contextTask=(uintptr_t)task;CameraControls::Set(eventSaved);
 puts("PASS: cinematic camera handoff without cull restore, battle ground/character gates, ownership, options, shot switches and native restoration");
 // Reproduce the actual Joker hierarchy with an attached Morgana skeleton.
 unsigned char morgana[0x130]={};memcpy(morgana,root,sizeof(morgana));Name(morgana,0xf0,"morugana_root");
 Put(morgana,0xd8,(uintptr_t)extra);Put(morgana,0xe0,(uintptr_t)head);Put(root,0xd8,(uintptr_t)morgana);Put(head,0xe0,uintptr_t(0));
 tree=FindHead((uintptr_t)root,false);Check(tree.head==(uintptr_t)head&&tree.heads==1,"Joker selected without attached Morgana head"); // Real callback-slot installation works without an executable trampoline allocation.
 Put(task,0x30,(uintptr_t)OriginalUpdate);g_fieldUpdate=nullptr;g_fieldHook=0;
 InstallBodyHook((uintptr_t)task);uintptr_t callback=0;memcpy(&callback,task+0x30,8);
 Check(callback==(uintptr_t)BodyFieldUpdate&&g_fieldUpdate==OriginalUpdate,"callback installed without MinHook");
 InstallBodyHook((uintptr_t)task);Check(g_fieldUpdate==OriginalUpdate,"repeat install preserves original");
 // Reproduce a failed installation using a read-only task: no pointer mutation and visible error.
 auto readonlyTask=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);Check(readonlyTask!=nullptr,"fixture allocation");
 memcpy(readonlyTask,task,sizeof(task));Put(readonlyTask,0x30,(uintptr_t)OriginalUpdate);DWORD protection=0;
 Check(VirtualProtect(readonlyTask,4096,PAGE_READONLY,&protection)!=0,"fixture read-only");
 InstallBodyHook((uintptr_t)readonlyTask);memcpy(&callback,readonlyTask+0x30,8);
 Check(callback==(uintptr_t)OriginalUpdate&&GfdCam::bodyStatus==4,"failed install preserves original callback and reports failure");
 VirtualFree(readonlyTask,0,MEM_RELEASE);
 puts("PASS: actual task callback installation, repeated installation and read-only failure recovery"); puts("PASS: unnamed skeleton, uncached names, camera unchanged, independent hide/restore, ambiguous-head rejection"); puts("PASS player probe: validated identity, bounded cycles, read-only nodes, scene rejection and invalid pointers");
}
