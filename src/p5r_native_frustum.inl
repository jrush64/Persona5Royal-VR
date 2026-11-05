using FrustumBuildFn=uintptr_t(*)(uintptr_t,uintptr_t);
FrustumBuildFn g_frustumBuild=nullptr;
bool g_frustumAttempted=false;
std::atomic<unsigned> g_nativeFrustumApplied{0};
uintptr_t NativeFrustumBuild(uintptr_t output,uintptr_t matrix){
 uintptr_t result=g_frustumBuild(output,matrix);
 if(!GfdCam::visibilityRequested.load()||!CameraControls::Get().wideVisibility)return result;
 BodyGuard guard;
 uintptr_t camera=0,root=0;auto task=g_contextTask.load();
 if(!FieldIdentity(task,camera,root)||output!=camera+0xb0)return result;
 float view[16],planes[24],expanded[24],nearClip=0,farClip=0;
 auto settings=CameraControls::Get();
 float padding=fabsf(settings.right)+fabsf(settings.up)+fabsf(settings.forward)+CameraControls::HalfEye(settings);
 bool valid=ProbeRead(camera+0x20,view,sizeof(view))&&ProbeRead(output,planes,sizeof(planes))&&
  ProbeRd(camera+0x1a0,nearClip)&&ProbeRd(camera+0x1a4,farClip)&&
  P5RCulling::Expand(view,planes,nearClip,farClip,padding,expanded);
 if(valid&&WritePlanes(camera,expanded)){
  g_visibility.task=task;g_visibility.camera=camera;g_visibility.root=root;
  memcpy(g_visibility.original,planes,sizeof(planes));memcpy(g_visibility.expanded,expanded,sizeof(expanded));
  GfdCam::visibilityStatus=1;++g_nativeFrustumApplied;
 }else GfdCam::visibilityStatus=2;
 return result;
}
void InstallNativeFrustum(){
 if(g_frustumAttempted||!GfdCam::visibilityRequested.load()||!g_contextTask.load())return;
 g_frustumAttempted=true;
 auto base=(uintptr_t)GetModuleHandleW(nullptr);auto target=base+0x1ef610;
 // Exact-build checks: relocation-free prologue, known plane writer and the
 // traced caller's relative call. Reject changed executables.
 const unsigned char writer[]={0xf3,0x0f,0x11,0x11,0x44,0x0f,0x28,0x58,0x98};
 const unsigned char call[]={0xe8,0x87,0x14,0x00,0x00};
 unsigned char a[sizeof(writer)],b[sizeof(call)],entry[15];
 bool verified=ProbeRead(base+0x1f1178,a,sizeof(a))&&!memcmp(a,writer,sizeof(a))&&
  ProbeRead(base+0x1efbc4,b,sizeof(b))&&!memcmp(b,call,sizeof(b))&&
  ProbeRead(target,entry,sizeof(entry))&&!memcmp(entry,P5RAbsoluteHook::prefix,15);
 bool installed=verified&&P5RAbsoluteHook::Install((void*)target,(void*)NativeFrustumBuild,(void**)&g_frustumBuild);
 if(g_log)g_log("[visibility-native] builder=P5R+0x1EF610 verified=%d installed=%d",verified,installed);
 if(!installed)GfdCam::visibilityStatus=3;
}