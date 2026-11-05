// Observed recursive scene culling reaches this geometry flag setter.
using GeometryCullFn=void(*)(uintptr_t,bool);
GeometryCullFn g_geometryCull=nullptr;
bool g_geometryCullAttempted=false;
std::atomic<unsigned> g_geometryCullSuppressed{0};
struct CullMember {uintptr_t owner;LONG requested;};
SRWLOCK g_geometryCullLock=SRWLOCK_INIT;
std::unordered_map<uintptr_t,CullMember> g_cullMembers;
std::atomic<uintptr_t> g_cullScene{0},g_cullRoot{0};
std::atomic<bool> g_cullEvent{false},g_cullBattle{false};
std::atomic<unsigned> g_cullRestoreEpoch{0};
constexpr unsigned char cullPrefix[]={0x44,0x8b,0x41,0x20,0x44,0x89,0xc0,0x0f,0xba,0xf0,0x0e,0x41,0x0f,0xba,0xe8,0x0e};
constexpr unsigned char cullBoundaries[]={0,4,7,11};
// Full task/name validation occurs once in MaintainGeometryCull. Hot hooks retain
// immediate scene/camera/root checks with guarded reads, without OS read syscalls.
// Keep membership stable across cinematic and battle camera switches. Resolve
// the live camera each hot call; field mode still requires its validated camera.
uintptr_t ActiveCullCamera(){
 if(!GfdCam::visibilityRequested.load()||!CameraControls::wideVisibilityEnabled.load())return 0;
 if(g_cullEvent.load()&&!EventFirstPersonWanted())return 0;
 if(g_cullBattle.load()&&!BattleSceneWanted())return 0;
 const auto scene=g_cullScene.load(),root=g_cullRoot.load();
 uintptr_t active=0,currentRoot=0,s0=0,s1=0;uint32_t kind=0;
 if(!scene||!root||!Rd(g_global.load()+0x60,s0)||!Rd(g_global.load()+0x68,s1)||
    (scene!=s0&&scene!=s1)||!Rd(scene+0x10,active)||!Rd(scene+0x50,currentRoot)||currentRoot!=root)return 0;
 if(g_cullEvent.load()||g_cullBattle.load())return Rd(active,kind)&&kind==5?active:0;
 return active==g_cullCamera.load()?active:0;
}
bool CullSceneActive(){return ActiveCullCamera()!=0;}
void NativeGeometryCull(uintptr_t geometry,bool culled){
 bool suppress=false;
 {
  AcquireSRWLockShared(&g_geometryCullLock);
  auto it=g_cullMembers.find(geometry);
  if(it!=g_cullMembers.end()){
   uintptr_t owner=0;uint32_t kind=0;
   if(Rd(geometry,kind)&&kind==4&&Rd(geometry+8,owner)&&owner==it->second.owner){
    InterlockedExchange(&it->second.requested,culled?1:0);suppress=CullSceneActive();
   }
  }
  ReleaseSRWLockShared(&g_geometryCullLock);
 }
 g_geometryCull(geometry,suppress?false:culled);
 if(suppress&&culled){++g_geometryCullSuppressed;GfdCam::visibilityStatus=1;}
}
void WriteCullBit(uintptr_t geometry,bool culled){
 __try{if(culled)InterlockedOr((volatile LONG*)(geometry+0x20),0x4000);
       else InterlockedAnd((volatile LONG*)(geometry+0x20),~LONG(0x4000));}__except(EXCEPTION_EXECUTE_HANDLER){}
}
void RestoreGeometryCull(){
 AcquireSRWLockExclusive(&g_geometryCullLock);
 if(!g_cullMembers.empty())++g_cullRestoreEpoch;
 for(const auto& pair:g_cullMembers){uint32_t kind=0;uintptr_t owner=0;
  if(Rd(pair.first,kind)&&kind==4&&Rd(pair.first+8,owner)&&owner==pair.second.owner){
   WriteCullBit(pair.first,pair.second.requested);
  }
 }
 g_cullCamera=0;g_cullMembers.clear();g_cullScene=0;g_cullRoot=0;g_cullEvent=false;g_cullBattle=false;ReleaseSRWLockExclusive(&g_geometryCullLock);
}
void MaintainGeometryCull(){
 static ULONGLONG next=0;
 uintptr_t camera=0,player=0,scene=0,root=0;
 const bool battle=BattleSceneWanted();
 const bool event=!battle&&EventFirstPersonWanted();
 bool enabled=GfdCam::visibilityRequested.load()&&CameraControls::Get().wideVisibility&&
  (battle?BattleCullIdentity(camera,scene,root):event?EventCullIdentity(camera,scene,root):(!GfdCam::EventRunning()&&FieldIdentity(g_contextTask.load(),camera,player)));
 if(!enabled){RestoreGeometryCull();return;}
 for(int i=0;i<2;++i){uintptr_t s=0,c=0;if(Rd(g_global.load()+0x60+i*8,s)&&Rd(s+0x10,c)&&c==camera){scene=s;break;}}
 if(!scene||!Rd(scene+0x50,root)){RestoreGeometryCull();return;}
 if(scene!=g_cullScene.load()||root!=g_cullRoot.load()||event!=g_cullEvent.load()||battle!=g_cullBattle.load()||
    (!event&&!battle&&camera!=g_cullCamera.load())){RestoreGeometryCull();next=0;}
 // A shot change within the same scene must not restore then clear all geometry flags.
 if((event||battle)&&g_cullScene.load()==scene)g_cullCamera=camera;
 if(GetTickCount64()<next)return;next=GetTickCount64()+1000;
 auto scan=SceneVisibility::Read(root);
 uintptr_t eventCamera=0,eventScene=0,eventRoot=0;
 const bool eventValid=!event||(EventCullIdentity(eventCamera,eventScene,eventRoot)&&eventCamera==camera&&eventScene==scene&&eventRoot==root&&scan.entries.find(GfdCam::EventJokerHead())!=scan.entries.end());
 if(!eventValid||!scan.nodes||scan.capped){RestoreGeometryCull();GfdCam::visibilityStatus=2;return;}
 AcquireSRWLockExclusive(&g_geometryCullLock);
 // Preserve the latest native request for existing members across census refreshes.
 for(auto it=g_cullMembers.begin();it!=g_cullMembers.end();){auto live=scan.entries.find(it->first);
  if(live==scan.entries.end()||!live->second.geometry||live->second.node!=it->second.owner){
   uint32_t kind=0;uintptr_t owner=0;
   if(Rd(it->first,kind)&&kind==4&&Rd(it->first+8,owner)&&owner==it->second.owner)WriteCullBit(it->first,it->second.requested);
   it=g_cullMembers.erase(it);
  }else ++it;
 }
 for(const auto& pair:scan.entries)if(pair.second.geometry){
  auto found=g_cullMembers.find(pair.first);
  if(found==g_cullMembers.end())g_cullMembers.emplace(pair.first,CullMember{pair.second.node,(pair.second.flags&0x4000)!=0});
  // Clear only this observed cull bit; preserve explicit hiding and all other flags.
  uint32_t kind=0;uintptr_t owner=0;
  if(Rd(pair.first,kind)&&kind==4&&Rd(pair.first+8,owner)&&owner==pair.second.node)WriteCullBit(pair.first,false);
 }
 const bool newScene=g_cullScene.load()!=scene;
 g_cullRoot=root;g_cullScene=scene;g_cullCamera=camera;g_cullEvent=event;g_cullBattle=battle;
 if(newScene&&g_log)g_log("[wide-scene] %s scene=%p root=%p camera=%p geometry=%zu",battle?"battle":event?"event":"field",(void*)scene,(void*)root,(void*)camera,g_cullMembers.size());ReleaseSRWLockExclusive(&g_geometryCullLock);
}
void InstallGeometryCull(){
 if(g_geometryCullAttempted||!GfdCam::visibilityRequested.load()||(!g_contextTask.load()&&!EventFirstPersonWanted()&&!BattleSceneWanted()))return;
 g_geometryCullAttempted=true;const auto base=(uintptr_t)GetModuleHandleW(nullptr),target=base+0x5460710;
 const unsigned char tail[]={0x84,0xd2,0x44,0x0f,0x44,0xc0,0x44,0x89,0x41,0x20,0xc3};
 unsigned char a[16],b[sizeof(tail)];
 bool verified=ProbeRead(target,a,16)&&!memcmp(a,cullPrefix,16)&&ProbeRead(target+16,b,sizeof(b))&&!memcmp(b,tail,sizeof(b));
 bool installed=verified&&P5RAbsoluteHook::Install((void*)target,(void*)NativeGeometryCull,(void**)&g_geometryCull,cullPrefix,16,cullBoundaries,sizeof(cullBoundaries));
 if(g_log)g_log("[geometry-cull] setter=P5R+0x5460710 verified=%d installed=%d",verified,installed);
 if(!installed)GfdCam::visibilityStatus=3;
}
