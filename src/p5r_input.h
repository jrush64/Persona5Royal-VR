#pragma once
#include "p5r_menu.h"
#include "p5r_camera_controls.h"
#include "p5r_gfdcam.h"


#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <Xinput.h>
#include <atomic>
#include <initializer_list>
#pragma comment(lib,"dxguid.lib")

namespace P5RInput {
inline bool Blocked(){return P5RMenu::IsOpen() && !P5RMenu::internalCall;}
inline std::atomic<LONG> cursorX{0},cursorY{0};
inline std::atomic<bool> warped{false};
inline BOOL (WINAPI* getCursor)(LPPOINT)=nullptr;
inline SHORT (WINAPI* getAsync)(int)=nullptr;
inline SHORT (WINAPI* getKey)(int)=nullptr;
inline BOOL (WINAPI* getKeyboard)(PBYTE)=nullptr;
inline DWORD (WINAPI* getPad)(DWORD,XINPUT_STATE*)=nullptr;
inline void RememberWarp(int x,int y){cursorX.store(x);cursorY.store(y);warped.store(true);}
inline BOOL WINAPI Cursor(LPPOINT point){
    if(Blocked() && point){point->x=cursorX.load();point->y=cursorY.load();return TRUE;}
    return getCursor(point);
}
inline SHORT WINAPI Async(int key){SHORT state=getAsync(key);return Blocked()?0:state;}
inline SHORT WINAPI Key(int key){SHORT state=getKey(key);return Blocked()?0:state;}
inline BOOL WINAPI Keyboard(PBYTE bytes){BOOL ok=getKeyboard(bytes);if(ok && bytes && Blocked())ZeroMemory(bytes,256);return ok;}
// [R3TOGGLE] 10-16: first person toggle for whatever is on screen, shared by F5 and holding R3. Field -> field first
// person; cutscene -> cutscene first person; confidant/talk scene -> conversation first person. Saved to the ini.
inline std::atomic<uint32_t> fpToggles{0};
inline std::atomic<int> fpToggleKind{0};
inline std::atomic<bool> fpToggleOn{false};
inline const char* ToggleFirstPerson(){
    auto v=CameraControls::Get(); int kind=0; bool on=false;
    if(GfdCam::EventRunning()){
        if(GfdCam::ConfidantRunning()){v.eventFirstPerson=!v.eventFirstPerson;on=v.eventFirstPerson;kind=2;}
        else{v.cutsceneFirstPerson=!v.cutsceneFirstPerson;on=v.cutsceneFirstPerson;kind=1;}
    } else {v.enabled=!v.enabled;on=v.enabled;kind=0;}
    CameraControls::Set(v);
    if(CameraControls::iniPath[0])CameraControls::Save();   // never write without a real settings file (tests)
    fpToggleKind.store(kind);fpToggleOn.store(on);fpToggles.fetch_add(1);
    static const char* names[3][2]={{"Field first person OFF","Field first person ON"},{"Cutscene first person OFF","Cutscene first person ON"},{"Conversation first person OFF","Conversation first person ON"}};
    return names[kind][on?1:0];
}
// Hold R3 ~1.5 s = toggle (the game never sees that press). A shorter tap is held back and replayed to the game on
// release, so R3 still centres the camera. Packet numbers are bumped when only our replay changes the buttons.
struct R3State{bool down=false,decided=false;ULONGLONG since=0,tapUntil=0;WORD lastButtons=0;DWORD lastPacket=0,bump=0;};
inline R3State r3[4];
constexpr ULONGLONG kR3HoldMs=1500,kR3TapMs=120;   // user: 0.5 s too short to time
inline DWORD WINAPI Pad(DWORD index,XINPUT_STATE* state){
    DWORD result=getPad(index,state);
    if(P5RMenu::internalCall)return result;
    if(result==ERROR_SUCCESS && state && !Blocked() && index<4){   // [R3TOGGLE]
        R3State& r=r3[index]; const ULONGLONG now=GetTickCount64();
        const bool phys=(state->Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_THUMB)!=0;
        if(phys&&!r.down){r.down=true;r.decided=false;r.since=now;}
        else if(phys&&!r.decided&&now-r.since>=kR3HoldMs){r.decided=true;ToggleFirstPerson();}
        else if(!phys&&r.down){r.down=false;if(!r.decided)r.tapUntil=now+kR3TapMs;}
        state->Gamepad.wButtons&=(WORD)~XINPUT_GAMEPAD_RIGHT_THUMB;
        if(now<r.tapUntil)state->Gamepad.wButtons|=XINPUT_GAMEPAD_RIGHT_THUMB;
        DWORD packet=state->dwPacketNumber+r.bump;
        if(state->Gamepad.wButtons!=r.lastButtons&&packet==r.lastPacket){++r.bump;++packet;}
        state->dwPacketNumber=packet;r.lastButtons=state->Gamepad.wButtons;r.lastPacket=packet;
    }
    if(result==ERROR_SUCCESS && state && Blocked()) {
        ZeroMemory(&state->Gamepad,sizeof(state->Gamepad));
        // Change packet even if a held physical stick did not move: cached consumers must see neutral.
        state->dwPacketNumber^=0x80000000u;
    }
    return result;
}
using StateFn=HRESULT(STDMETHODCALLTYPE*)(void*,DWORD,LPVOID);
using DataFn=HRESULT(STDMETHODCALLTYPE*)(void*,DWORD,LPDIDEVICEOBJECTDATA,LPDWORD,DWORD);
struct DeviceHook{void* target=nullptr;void* original=nullptr;};
inline DeviceHook states[4],datas[4];
inline std::atomic<int> stateCount{0},dataCount{0};
inline HRESULT StateCall(StateFn real,void* self,DWORD size,LPVOID data){
    HRESULT hr=real(self,size,data);
    if(SUCCEEDED(hr) && data && Blocked())ZeroMemory(data,size);
    return hr;
}
inline HRESULT DataCall(DataFn real,void* self,DWORD size,LPDIDEVICEOBJECTDATA data,LPDWORD count,DWORD flags){
    bool block=Blocked();
    HRESULT hr=real(self,size,data,count,block?(flags&~DIGDD_PEEK):flags);
    if(SUCCEEDED(hr) && count && block)*count=0; // Drain events so menu clicks cannot replay on close.
    return hr;
}
inline HRESULT STDMETHODCALLTYPE State(void* self,DWORD size,LPVOID data){
    void* target=(*(void***)self)[9];
    for(int i=0;i<stateCount;++i)if(states[i].target==target)return StateCall((StateFn)states[i].original,self,size,data);
    return DIERR_GENERIC;
}
inline HRESULT STDMETHODCALLTYPE Data(void* self,DWORD size,LPDIDEVICEOBJECTDATA data,LPDWORD count,DWORD flags){
    void* target=(*(void***)self)[10];
    for(int i=0;i<dataCount;++i)if(datas[i].target==target)return DataCall((DataFn)datas[i].original,self,size,data,count,flags);
    return DIERR_GENERIC;
}
using HookFn=bool(*)(void*,void*,void**,const char*);
inline void Install(HookFn hook){
    HMODULE user=GetModuleHandleW(L"user32.dll");
    hook((void*)GetProcAddress(user,"GetCursorPos"),(void*)Cursor,(void**)&getCursor,"Input::GetCursorPos");
    hook((void*)GetProcAddress(user,"GetAsyncKeyState"),(void*)Async,(void**)&getAsync,"Input::GetAsyncKeyState");
    hook((void*)GetProcAddress(user,"GetKeyState"),(void*)Key,(void**)&getKey,"Input::GetKeyState");
    hook((void*)GetProcAddress(user,"GetKeyboardState"),(void*)Keyboard,(void**)&getKeyboard,"Input::GetKeyboardState");
    // P5R imports xinput1_4 ordinal 2, verified locally as XInputGetState.
    HMODULE pad=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(pad)hook((void*)GetProcAddress(pad,"XInputGetState"),(void*)Pad,(void**)&getPad,"Input::XInputGetState");
    wchar_t systemPath[MAX_PATH]={};GetSystemDirectoryW(systemPath,MAX_PATH);
    wcscat_s(systemPath,L"\\dinput8.dll");
    HMODULE di=LoadLibraryExW(systemPath,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    using CreateFn=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,LPVOID*,LPUNKNOWN);
    auto create=di?(CreateFn)GetProcAddress(di,"DirectInput8Create"):nullptr;
    if(!create)return;
    // Discover ANSI and Unicode mouse/keyboard implementations without acquiring devices.
    // Method-address hooks also cover the game's already-created devices.
    for(int wide=0;wide<2;++wide){
        IDirectInput8W* factory=nullptr;
        if(FAILED(create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,wide?IID_IDirectInput8W:IID_IDirectInput8A,(void**)&factory,nullptr)))continue;
        for(const GUID* guid : {&GUID_SysMouse,&GUID_SysKeyboard}){
            IDirectInputDevice8W* dev=nullptr;
            if(FAILED(factory->CreateDevice(*guid,&dev,nullptr)))continue;
            void** vt=*(void***)dev;
            bool seen=false;for(int i=0;i<stateCount;++i)if(states[i].target==vt[9])seen=true;
            if(!seen && stateCount<4){auto& entry=states[stateCount.load()];entry.target=vt[9];if(hook(entry.target,(void*)State,&entry.original,"Input::DirectInputState"))++stateCount;}
            seen=false;for(int i=0;i<dataCount;++i)if(datas[i].target==vt[10])seen=true;
            if(!seen && dataCount<4){auto& entry=datas[dataCount.load()];entry.target=vt[10];if(hook(entry.target,(void*)Data,&entry.original,"Input::DirectInputData"))++dataCount;}
            dev->Release();
        }
        factory->Release();
    }
}
}
