// Bounded diagnostic only: observe the validated field thread accessing camera
// frustum data. Never steal an existing hardware-breakpoint configuration.
namespace CullTrace {
std::atomic<DWORD> fieldThread{0},presentThread{0};
std::atomic<uintptr_t> camera{0};
std::atomic<uintptr_t> flagObject{0},flagOwner{0};
std::atomic<bool> started{false},armed{false},pending{false};
uintptr_t executableBegin=0,executableEnd=0,watched=0;
DWORD watchedThread=0;
uint32_t watchedFlags=0;bool flagsOnly=false;
struct Hit {uintptr_t ip=0,stack[24]={},registers[15]={};unsigned char code[64]={};uintptr_t codeAt=0;DWORD thread=0;} hit;
LONG CALLBACK Handler(EXCEPTION_POINTERS* e){
 if(e->ExceptionRecord->ExceptionCode!=EXCEPTION_SINGLE_STEP||!armed.load()||
    GetCurrentThreadId()!=watchedThread||!(e->ContextRecord->Dr6&8))return EXCEPTION_CONTINUE_SEARCH;
 auto* c=e->ContextRecord;
 // Skip verified producers so the next access can identify a consumer.
 const auto rva=c->Rip-executableBegin;
 const bool producer=(rva>=0x1ef610&&rva<=0x1efcd5)||(rva>=0x1f1050&&rva<=0x1f11ad);
 uint32_t flags=watchedFlags;bool changed=true;
 if(flagsOnly){changed=ProbeRd(watched,flags)&&((flags^watchedFlags)&0x4000);watchedFlags=flags;}
 if(c->Rip>=executableBegin&&c->Rip<executableEnd&&!producer&&changed){
  hit={};hit.ip=c->Rip;hit.thread=GetCurrentThreadId();hit.codeAt=c->Rip>=32?c->Rip-32:c->Rip;
  uintptr_t registers[]={c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->R8,c->R9,c->R10,c->R11,c->R12,c->R13,c->R14,c->R15};
  memcpy(hit.registers,registers,sizeof(registers));
  ProbeRead(c->Rsp,hit.stack,sizeof(hit.stack));ProbeRead(hit.codeAt,hit.code,sizeof(hit.code));
  c->Dr7&=~(0x40ull|0xf0000000ull);c->Dr3=0;
  armed=false;pending=true;
 }
 c->Dr6&=~8ull;
 return EXCEPTION_CONTINUE_EXECUTION;
}
bool Configure(DWORD id,uintptr_t address,bool enable){
 HANDLE thread=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,id);
 if(!thread)return false;
 bool ok=false;
 if(SuspendThread(thread)!=(DWORD)-1){
  CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
  if(GetThreadContext(thread,&c)){
   if(enable){
    if((c.Dr7&0xff)==0){
     c.Dr3=address;c.Dr6=0;c.Dr7=(c.Dr7&~0xf0000000ull)|0xd0000040ull; // DR3: four-byte writes only
     watchedThread=id;watched=address;ProbeRd(address,watchedFlags);armed=true;
     ok=SetThreadContext(thread,&c)!=0;if(!ok)armed=false;
    }
   }else if(c.Dr3==watched&&(c.Dr7&0x40)){
    c.Dr7&=~(0x40ull|0xf0000000ull);c.Dr3=0;c.Dr6&=~8ull;ok=SetThreadContext(thread,&c)!=0;
   }else ok=true;
  }
  ResumeThread(thread);
 }
 CloseHandle(thread);return ok;
}
DWORD WINAPI Worker(void*){
 auto base=(uintptr_t)GetModuleHandleW(nullptr);
 IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 nt{};
 if(!ProbeRead(base,&dos,sizeof(dos))||!ProbeRead(base+dos.e_lfanew,&nt,sizeof(nt)))return 0;
 executableBegin=base;executableEnd=base+nt.OptionalHeader.SizeOfImage;
 void* handler=AddVectoredExceptionHandler(1,Handler);if(!handler)return 0;
 unsigned captures=0,attempts=0;ULONGLONG deadline=GetTickCount64()+90000,armTime=0;
 if(g_log)g_log("[cull-writer] started: geometry flags +20, field thread, write-only");
 while(captures<8&&GetTickCount64()<deadline&&(attempts<24||armed.load()||pending.load())){
  Sleep(100);
  if(pending.exchange(false)){
   ++captures;
   if(g_log){
    g_log("[cull-writer] sample=%u thread=%lu watched=%p ip=P5R+0x%llX codeAt=P5R+0x%llX",captures,hit.thread,(void*)watched,(unsigned long long)(hit.ip-base),(unsigned long long)(hit.codeAt-base));
    const char* names[]={"rax","rbx","rcx","rdx","rsi","rdi","rbp","r8","r9","r10","r11","r12","r13","r14","r15"};
    for(unsigned i=0;i<15;++i)g_log("[cull-writer] %s=%p",names[i],(void*)hit.registers[i]);
    char bytes[193]={};for(unsigned i=0;i<64;++i)sprintf_s(bytes+i*3,4,"%02X ",hit.code[i]);
    g_log("[cull-writer] code=%s",bytes);
    for(unsigned i=0;i<24;++i)if(hit.stack[i]>=base&&hit.stack[i]<executableEnd)
     g_log("[cull-writer] sp+0x%X=P5R+0x%llX",i*8,(unsigned long long)(hit.stack[i]-base));
   }
  }
  if(armed.load()){
   if(GetTickCount64()-armTime>3000||watched!=flagObject.load()+0x20){
    if(Configure(watchedThread,0,false)){armed=false;if(g_log)g_log("[cull-writer] no cull-bit change in 3 seconds; retry");}
   }
   continue;
  }
  if(attempts>=24)continue;
  auto object=flagObject.load(),owner=flagOwner.load();auto tid=fieldThread.load();
  uint32_t kind=0;uintptr_t parent=0;
  if(object&&tid&&ProbeRd(object,kind)&&kind==4&&ProbeRd(object+8,parent)&&parent==owner){
   ++attempts;bool ok=Configure(tid,object+0x20,true);
   if(g_log)g_log("[cull-writer] arm=%u thread=%lu object=%p owner=%p ok=%d",attempts,tid,(void*)object,(void*)owner,ok);
   if(ok)armTime=GetTickCount64();
  }
 }
 if(armed.load()){
  if(!Configure(watchedThread,0,false)){
   // Keep handler installed if cleanup could not be confirmed.
   if(g_log)g_log("[cull-writer] cleanup not confirmed; handler retained");return 0;
  }
  armed=false;
 }
 RemoveVectoredExceptionHandler(handler);
 if(g_log)g_log("[cull-writer] complete samples=%u",captures);
 return 0;
}
void Start(){
 bool expected=false;if(!started.compare_exchange_strong(expected,true))return;
 flagsOnly=true;
 HANDLE t=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
 if(t)CloseHandle(t);else started=false;
}
}