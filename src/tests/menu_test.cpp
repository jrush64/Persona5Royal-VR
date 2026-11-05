// Actual ImGui D3D11 rendering on WARP. No game, headset or visible window.
#include "../p5r_menu.cpp"
#include <cassert>
#include <vector>
#include "imgui_internal.h"
static int warpCalls=0,clipCalls=0;
static BOOL WINAPI SpyWarp(int,int){++warpCalls;return FALSE;}
static BOOL WINAPI SpyClip(const RECT*){++clipCalls;return FALSE;}
static SHORT WINAPI HeldKey(int){return (SHORT)0x8001;}
static BOOL WINAPI HeldKeyboard(PBYTE data){memset(data,0x80,256);return TRUE;}
static BOOL WINAPI MovingCursor(LPPOINT point){point->x=123;point->y=456;return TRUE;}
static DWORD WINAPI MovingPad(DWORD index,XINPUT_STATE* state){
    if(index)return ERROR_DEVICE_NOT_CONNECTED;
    ZeroMemory(state,sizeof(*state));state->Gamepad.sThumbLY=20000;state->dwPacketNumber=5;state->Gamepad.wButtons=XINPUT_GAMEPAD_A;state->Gamepad.sThumbRX=30000;return ERROR_SUCCESS;
}
static WORD stickButtons=0;
static DWORD WINAPI StickPad(DWORD index,XINPUT_STATE* state){if(index)return ERROR_DEVICE_NOT_CONNECTED;ZeroMemory(state,sizeof(*state));state->Gamepad.wButtons=stickButtons;return ERROR_SUCCESS;}
static HRESULT STDMETHODCALLTYPE MovingMouse(void*,DWORD size,LPVOID data){memset(data,0x5a,size);return DI_OK;}
static DWORD lastFlags=0;
static HRESULT STDMETHODCALLTYPE BufferedMouse(void*,DWORD,LPDIDEVICEOBJECTDATA,LPDWORD count,DWORD flags){lastFlags=flags;*count=2;return DI_OK;}
static bool RecordInputHook(void* target,void*,void** original,const char*){assert(target);*original=target;return true;}
static void TestInputCapture(){
    P5RInput::getAsync=HeldKey;P5RInput::getKey=HeldKey;P5RInput::getKeyboard=HeldKeyboard;
    P5RInput::getCursor=MovingCursor;P5RInput::getPad=MovingPad;
    P5RInput::RememberWarp(640,450);P5RMenu::open.store(true);
    POINT point;assert(P5RInput::Cursor(&point) && point.x==640 && point.y==450);
    assert(P5RInput::Async('W')==0 && P5RInput::Key(VK_LBUTTON)==0);
    BYTE keyboard[256];assert(P5RInput::Keyboard(keyboard));for(BYTE key:keyboard)assert(key==0);
    XINPUT_STATE pad{};assert(P5RInput::Pad(0,&pad)==ERROR_SUCCESS);
    assert(pad.Gamepad.wButtons==0 && pad.Gamepad.sThumbRX==0 && pad.dwPacketNumber!=5);
    assert(P5RInput::Pad(1,&pad)==ERROR_DEVICE_NOT_CONNECTED);
    DIMOUSESTATE2 mouse;assert(SUCCEEDED(P5RInput::StateCall(MovingMouse,nullptr,sizeof(mouse),&mouse)));
    for(BYTE byte : *(BYTE(*)[sizeof(mouse)])&mouse)assert(byte==0);
    DWORD count=2;DIDEVICEOBJECTDATA events[2];
    assert(SUCCEEDED(P5RInput::DataCall(BufferedMouse,nullptr,sizeof(events[0]),events,&count,DIGDD_PEEK)));
    assert(count==0 && lastFlags==0);
    // The menu's own polling stays live while game callers get neutral input.
    P5RMenu::internalCall=true;
    assert(P5RInput::Async(VK_INSERT)==(SHORT)0x8001);
    assert(P5RInput::Cursor(&point) && point.x==123);
    P5RMenu::internalCall=false;P5RMenu::open.store(false);
    assert(P5RInput::Async('W')==(SHORT)0x8001);
    assert(P5RInput::Pad(0,&pad)==ERROR_SUCCESS && pad.Gamepad.sThumbRX==30000 && pad.dwPacketNumber==5);
    assert(SUCCEEDED(P5RInput::StateCall(MovingMouse,nullptr,sizeof(mouse),&mouse)) && mouse.lX!=0);
    assert(SUCCEEDED(P5RInput::DataCall(BufferedMouse,nullptr,sizeof(events[0]),events,&count,DIGDD_PEEK)));
    assert(count==2 && lastFlags==DIGDD_PEEK);
    puts("PASS: menu-only input blocking, live menu polling, DirectInput queue drain, cursor baseline, XInput neutral packet and closed-menu restoration");
}

int main(int argc,char** argv) {
    const UINT renderWidth=argc>1 && !strcmp(argv[1],"headset")?3840:1280;
    const UINT renderHeight=argc>1 && !strcmp(argv[1],"headset")?2160:900;
    TestInputCapture();
    {
        auto saved=CameraControls::Get(),settings=saved;
        settings.enabled=true;settings.hideBody=true;CameraControls::Set(settings);
        XINPUT_STATE expected{},actual{};MovingPad(0,&expected);P5RInput::Pad(0,&actual);
        assert(!memcmp(&expected,&actual,sizeof(actual)));
        settings.enabled=false;CameraControls::Set(settings);P5RInput::Pad(0,&actual);
        assert(!memcmp(&expected,&actual,sizeof(actual)));
        CameraControls::Set(saved);
        puts("PASS: raw gamepad state and packet preserved with hide on and manual camera on/off");
    }
    {   // [R3TOGGLE] hold R3 toggles (field here: no event running); a tap is replayed to the game on release
        auto saved=CameraControls::Get(),settings=saved;settings.enabled=false;CameraControls::Set(settings);
        P5RInput::getPad=StickPad;XINPUT_STATE st{};const WORD R3=XINPUT_GAMEPAD_RIGHT_THUMB;
        stickButtons=0;P5RInput::Pad(0,&st);DWORD p0=st.dwPacketNumber;
        stickButtons=R3;P5RInput::Pad(0,&st);assert(!(st.Gamepad.wButtons&R3));          // held back while undecided
        stickButtons=0;P5RInput::Pad(0,&st);assert((st.Gamepad.wButtons&R3)&&st.dwPacketNumber!=p0);   // tap replayed on release
        DWORD p1=st.dwPacketNumber;Sleep(200);P5RInput::Pad(0,&st);
        assert(!(st.Gamepad.wButtons&R3)&&st.dwPacketNumber!=p1&&!CameraControls::Get().enabled);     // tap ends, packet bumped, no toggle
        stickButtons=R3;P5RInput::Pad(0,&st);Sleep(700);P5RInput::Pad(0,&st);assert(!CameraControls::Get().enabled);Sleep(900);P5RInput::Pad(0,&st);
        assert(CameraControls::Get().enabled&&!(st.Gamepad.wButtons&R3));                          // hold toggles, game never sees it
        P5RInput::Pad(0,&st);assert(CameraControls::Get().enabled);                                 // still held: once only
        stickButtons=0;P5RInput::Pad(0,&st);assert(!(st.Gamepad.wButtons&R3));                     // no tap after a hold
        assert(!strcmp(P5RInput::ToggleFirstPerson(),"Field first person OFF")&&!CameraControls::Get().enabled);   // F5 path, same toggle
        P5RInput::getPad=MovingPad;CameraControls::Set(saved);
        puts("PASS: R3 hold toggles first person once, taps still reach the game on release with a fresh packet, F5 shares the toggle");
    }
    P5RInput::Install(RecordInputHook);
    assert(P5RInput::stateCount>0 && P5RInput::dataCount>0);
    puts("PASS: system DirectInput ANSI/Unicode mouse/keyboard method discovery (no devices acquired)");
    P5RMenu::originalSetCursorPos=SpyWarp;
    P5RMenu::originalSetPhysicalCursorPos=SpyWarp;
    P5RMenu::originalClipCursor=SpyClip;
    RECT clip{10,20,30,40};
    P5RMenu::open.store(true);
    assert(P5RMenu::BlockGameCursorWarp(640,450));
    assert(P5RMenu::BlockGamePhysicalWarp(640,450));
    assert(P5RMenu::BlockGameCursorClip(&clip));
    assert(warpCalls==0 && clipCalls==0);
    assert(!P5RMenu::BlockGameCursorClip(nullptr));assert(clipCalls==1);
    P5RMenu::open.store(false);
    assert(!P5RMenu::BlockGameCursorWarp(640,450));
    assert(!P5RMenu::BlockGamePhysicalWarp(640,450));
    assert(!P5RMenu::BlockGameCursorClip(&clip));
    assert(warpCalls==2 && clipCalls==2);
    puts("PASS: both cursor warp APIs and confinement blocked only while menu open; unclip and closed-menu calls pass through");
    ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx)));
    HWND hwnd=CreateWindowExW(0,L"STATIC",L"Offline menu test",WS_OVERLAPPED,0,0,renderWidth,renderHeight,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(hwnd);
    assert(P5RMenu::Initialize(dev,ctx,hwnd));
    IDXGIDevice* dx=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIFactory* factory=nullptr;
    assert(SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice),(void**)&dx)));
    assert(SUCCEEDED(dx->GetAdapter(&adapter)));assert(SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),(void**)&factory)));
    DXGI_SWAP_CHAIN_DESC sd{};sd.BufferDesc.Width=renderWidth;sd.BufferDesc.Height=renderHeight;sd.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count=1;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.BufferCount=1;sd.OutputWindow=hwnd;sd.Windowed=TRUE;
    IDXGISwapChain* sc=nullptr;assert(SUCCEEDED(factory->CreateSwapChain(dev,&sd,&sc)));factory->Release();adapter->Release();dx->Release();
    ID3D11Texture2D* left=nullptr;assert(SUCCEEDED(sc->GetBuffer(0,__uuidof(ID3D11Texture2D),(void**)&left)));
    D3D11_TEXTURE2D_DESC td;left->GetDesc(&td);ID3D11Texture2D* right=nullptr;assert(SUCCEEDED(dev->CreateTexture2D(&td,nullptr,&right)));
    ID3D11RenderTargetView *lr=nullptr,*rr=nullptr;assert(SUCCEEDED(dev->CreateRenderTargetView(left,nullptr,&lr)));assert(SUCCEEDED(dev->CreateRenderTargetView(right,nullptr,&rr)));
    const float bg[]={0.08f,0.12f,0.18f,1};ctx->ClearRenderTargetView(lr,bg);ctx->ClearRenderTargetView(rr,bg);
    ctx->OMSetRenderTargets(1,&lr,nullptr);D3D11_VIEWPORT vp={11,13,500,400,0.2f,0.8f};ctx->RSSetViewports(1,&vp);
    ImGui::SetCurrentContext(P5RMenu::gui);auto& io=ImGui::GetIO();io.DisplaySize=ImVec2(renderWidth,renderHeight);io.DeltaTime=1.f/60;
    MenuLayout::loaded=true;MenuLayout::value.scale=1;
    P5RMenu::Actions actions;
    for(int i=0;i<2;++i){ImGui_ImplDX11_NewFrame();ImGui::NewFrame();P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();}
    if(argc>1 && !strcmp(argv[1],"camera")) {
        auto* tabs=P5RMenu::gui->TabBars.GetByIndex(0);assert(tabs && tabs->Tabs.Size==4);
        tabs->NextSelectedTabId=tabs->Tabs[1].ID;
        for(int i=0;i<2;++i){ImGui_ImplDX11_NewFrame();ImGui::NewFrame();P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();}
    }
    auto* panel=ImGui::FindWindowByName("P5R VR | Insert closes");assert(panel);
    assert(panel->DC.ChildWindows.Size>0 && panel->DC.ChildWindows[0]->FontWindowScale==panel->FontWindowScale);
    assert(panel->Size.y<=renderHeight*0.48f && panel->Size.x<=renderWidth*0.48f);
    // User-resized windows may scroll. Position/size must persist across frames.
    ImGui_ImplDX11_NewFrame();ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(100,120),ImGuiCond_Always);ImGui::SetNextWindowSize(ImVec2(500,460),ImGuiCond_Always);
    P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();
    ImGui_ImplDX11_NewFrame();ImGui::NewFrame();P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();
    assert(panel->Pos.x==100 && panel->Pos.y==120 && panel->Size.x==500 && panel->Size.y==460);
    wchar_t tmp[MAX_PATH];assert(GetTempPathW(MAX_PATH,tmp));assert(GetTempFileNameW(tmp,L"p5m",0,CameraControls::iniPath));
    MenuLayout::value.font=.8f;assert(MenuLayout::Save());MenuLayout::Reset();MenuLayout::Load();
    assert(fabsf(MenuLayout::value.x-100.f/renderWidth)<.00001f && MenuLayout::value.font==.8f);
    const auto oldLayout=MenuLayout::value;
    MenuLayout::SetSize(.5f);
    io.DisplaySize=ImVec2(renderWidth*2.f,renderHeight*2.f);
    for(int i=0;i<2;++i){ImGui_ImplDX11_NewFrame();ImGui::NewFrame();P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();}
    assert(fabsf(panel->Size.x-500)<2 && fabsf(panel->Size.y-460)<2);
    assert(fabsf(panel->Pos.x*.5f-100)<=1 && fabsf(panel->Pos.y*.5f-120)<=1);
    auto* data=ImGui::GetDrawData();auto* list=data->CmdLists[0];
    auto pos=list->VtxBuffer[0].pos;auto drawClip=list->CmdBuffer[0].ClipRect;
    P5RMenu::ScaleDrawData(data,.5f,renderWidth,renderHeight);
    assert(list->VtxBuffer[0].pos.x==pos.x*.5f && list->VtxBuffer[0].pos.y==pos.y*.5f);
    assert(list->CmdBuffer[0].ClipRect.z==drawClip.z*.5f && data->DisplaySize.x==renderWidth);
    assert(MenuLayout::value.font==oldLayout.font && MenuLayout::Save());
    MenuLayout::Reset();MenuLayout::Load();
    assert(MenuLayout::value.scale==.5f && fabsf(MenuLayout::value.w-oldLayout.w)<.002f && MenuLayout::value.font==.8f);
    puts("PASS: whole-panel vertices/clipping shrink 50%, anchor retained, logical controls stay intact, scale persists");
    {   // [HUD] dock settings: clamping and INI roundtrip
        HudSettings::Settings h;h.widthM=2.5f;h.distanceM=1.25f;h.heightM=-.4f;h.mode[0]=HudSettings::Follow;h.mode[4]=HudSettings::Hide;h.mode[5]=7;h.menusInVr=false;
        HudSettings::Set(h);assert(HudSettings::Get().mode[5]==HudSettings::Dock);assert(HudSettings::Save());
        HudSettings::Set(HudSettings::Settings{});HudSettings::Load();auto g=HudSettings::Get();
        assert(fabsf(g.widthM-2.5f)<.001f && fabsf(g.distanceM-1.25f)<.001f && fabsf(g.heightM+.4f)<.001f);
        assert(g.mode[0]==HudSettings::Follow && g.mode[1]==HudSettings::Dock && g.mode[4]==HudSettings::Hide && g.mode[5]==HudSettings::Dock && !g.menusInVr);
        h.widthM=99;h.distanceM=-3;HudSettings::Set(h);g=HudSettings::Get();assert(g.widthM==6.f && g.distanceM==.4f);
        float area=0;for(auto& r:HudSettings::kRegion)area+=(r.u1-r.u0)*(r.v1-r.v0);assert(fabsf(area-1.f)<.0001f);
        HudSettings::Set(HudSettings::Settings{});
        puts("PASS: HUD dock size/distance/height clamping, per-region modes INI roundtrip, regions tile the HUD image");
    }
    DeleteFileW(CameraControls::iniPath);CameraControls::iniPath[0]=0;
    MenuLayout::Reset();io.DisplaySize=ImVec2(renderWidth/MenuLayout::value.scale,renderHeight/MenuLayout::value.scale);
    for(int i=0;i<2;++i){ImGui_ImplDX11_NewFrame();ImGui::NewFrame();P5RMenu::DrawPanel(actions,false,true,renderWidth,renderHeight);ImGui::End();ImGui::Render();}
    P5RMenu::ScaleDrawData(ImGui::GetDrawData(),MenuLayout::value.scale,renderWidth,renderHeight);
    puts("PASS: dragged/resized layout survives next frame and INI roundtrip; reset recovers placement");
    P5RMenu::frameReady=true;P5RMenu::drawn=false;P5RMenu::Draw(left,right);assert(P5RMenu::WasDrawn());assert(!P5RMenu::internalCall);
    ID3D11RenderTargetView* restored=nullptr;ctx->OMGetRenderTargets(1,&restored,nullptr);assert(restored==lr);restored->Release();
    D3D11_VIEWPORT got;UINT count=1;ctx->RSGetViewports(&count,&got);assert(count==1 && memcmp(&vp,&got,sizeof(vp))==0);
    td.Usage=D3D11_USAGE_STAGING;td.BindFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.MiscFlags=0;
    ID3D11Texture2D* readback=nullptr;assert(SUCCEEDED(dev->CreateTexture2D(&td,nullptr,&readback)));
    std::vector<unsigned char> pixels(td.Width*td.Height*4);
    ctx->CopyResource(readback,left);D3D11_MAPPED_SUBRESOURCE mapped;assert(SUCCEEDED(ctx->Map(readback,0,D3D11_MAP_READ,0,&mapped)));
    for(UINT y=0;y<td.Height;++y)memcpy(pixels.data()+y*td.Width*4,(char*)mapped.pData+y*mapped.RowPitch,td.Width*4);
    ctx->Unmap(readback,0);ctx->CopyResource(readback,right);assert(SUCCEEDED(ctx->Map(readback,0,D3D11_MAP_READ,0,&mapped)));
    for(UINT y=0;y<td.Height;++y)assert(memcmp(pixels.data()+y*td.Width*4,(char*)mapped.pData+y*mapped.RowPitch,td.Width*4)==0);
    ctx->Unmap(readback,0);readback->Release();
    assert(memcmp(pixels.data(),pixels.data()+((renderHeight/2)*renderWidth+renderWidth/2)*4,4)!=0);
    BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};fh.bfType=0x4d42;fh.bfOffBits=sizeof(fh)+sizeof(ih);fh.bfSize=fh.bfOffBits+(DWORD)pixels.size();
    ih.biSize=sizeof(ih);ih.biWidth=td.Width;ih.biHeight=-(LONG)td.Height;ih.biPlanes=1;ih.biBitCount=32;ih.biCompression=BI_RGB;
    FILE* out=nullptr;fopen_s(&out,"menu-test.bmp","wb");assert(out);fwrite(&fh,sizeof(fh),1,out);fwrite(&ih,sizeof(ih),1,out);fwrite(pixels.data(),pixels.size(),1,out);fclose(out);
    ctx->ClearState();lr->Release();rr->Release();left->Release();right->Release();
    // Menu resources stay alive: ensure they retain no backbuffer/view after rendering.
    assert(SUCCEEDED(sc->ResizeBuffers(1,640,480,DXGI_FORMAT_UNKNOWN,0)));
    assert(SUCCEEDED(sc->ResizeBuffers(1,renderWidth,renderHeight,DXGI_FORMAT_UNKNOWN,0)));
    P5RMenu::Shutdown();sc->Release();DestroyWindow(hwnd);ctx->Release();dev->Release();
    puts("PASS: actual menu pixels in both eyes, complete D3D state restored, paired resize with menu resources alive");
}
