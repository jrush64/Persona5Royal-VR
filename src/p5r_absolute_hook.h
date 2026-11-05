#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
// Supports explicitly verified relocation-free prologues only. A far trampoline
// avoids the near-allocation failure previously encountered in this game.
namespace P5RAbsoluteHook {
inline constexpr unsigned char prefix[15]={0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x53,0x56,0x57};
inline void Jump(unsigned char* dst,const void* target){
 dst[0]=0xff;dst[1]=0x25;memset(dst+2,0,4);memcpy(dst+6,&target,8);
}
inline constexpr unsigned char boundaries[]={0,3,7,11,12,13,14};
inline bool Install(void* target,void* replacement,void** original,
 const unsigned char* expected=prefix,size_t length=15,
 const unsigned char* starts=boundaries,size_t startCount=sizeof(boundaries)){
 if(length<14||length>32||!starts||!startCount||memcmp(target,expected,length))return false;
 auto tramp=(unsigned char*)VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
 if(!tramp)return false;
 memcpy(tramp,expected,length);Jump(tramp+length,(unsigned char*)target+length);
 DWORD old=0;
 if(!VirtualProtect(tramp,4096,PAGE_EXECUTE_READ,&old)){VirtualFree(tramp,0,MEM_RELEASE);return false;}
 FlushInstructionCache(GetCurrentProcess(),tramp,length+14);
 struct Frozen {HANDLE handle;CONTEXT context;bool relocate;};
 auto threads=(Frozen*)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(Frozen)*512);
 if(!threads){VirtualFree(tramp,0,MEM_RELEASE);return false;}
 HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
 bool ok=snapshot!=INVALID_HANDLE_VALUE;unsigned count=0;
 THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
 if(ok){
  ok=Thread32First(snapshot,&entry)!=0;
  while(ok){
   if(entry.th32OwnerProcessID==GetCurrentProcessId()&&entry.th32ThreadID!=GetCurrentThreadId()){
    if(count==512){ok=false;break;}
    HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,entry.th32ThreadID);
    if(!h){ok=false;break;}
    if(SuspendThread(h)==(DWORD)-1){CloseHandle(h);ok=false;break;}
    auto& item=threads[count++];item.handle=h;item.context.ContextFlags=CONTEXT_CONTROL;
    if(!GetThreadContext(h,&item.context)){ok=false;break;}
    auto ip=item.context.Rip,begin=(DWORD64)target;
    if(ip>=begin&&ip<begin+length){
     unsigned off=(unsigned)(ip-begin);
     bool boundary=false;for(size_t k=0;k<startCount;++k)if(off==starts[k])boundary=true;
     if(!boundary){ok=false;break;}
     item.relocate=true;
    }
   }
   if(!Thread32Next(snapshot,&entry))break;
  }
  CloseHandle(snapshot);
 }
 // No user-space locks or logging while other threads are suspended.
 DWORD protection=0;
 if(ok)ok=memcmp(target,expected,length)==0&&VirtualProtect(target,length,PAGE_EXECUTE_READWRITE,&protection);
 unsigned moved=0;
 if(ok){
  for(;moved<count;++moved)if(threads[moved].relocate){
   CONTEXT adjusted=threads[moved].context;adjusted.Rip=(DWORD64)tramp+adjusted.Rip-(DWORD64)target;
   if(!SetThreadContext(threads[moved].handle,&adjusted)){ok=false;break;}
  }
  if(ok){
   *original=tramp;
   unsigned char patch[32];memset(patch,0x90,sizeof(patch));Jump(patch,replacement);
   memcpy(target,patch,length);FlushInstructionCache(GetCurrentProcess(),target,length);
  }else{
   // Threads already redirected can safely finish through the trampoline even
   // though the original entry remains unchanged; retain its allocation below.
  }
  DWORD ignored=0;VirtualProtect(target,length,protection,&ignored);
 }
 for(unsigned i=count;i>0;--i){ResumeThread(threads[i-1].handle);CloseHandle(threads[i-1].handle);}
 HeapFree(GetProcessHeap(),0,threads);
 if(!ok&&moved==0)VirtualFree(tramp,0,MEM_RELEASE);
 return ok;
}
}