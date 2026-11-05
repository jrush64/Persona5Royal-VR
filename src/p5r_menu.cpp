#define NOMINMAX
#include "p5r_menu.h"
#include "p5r_camera_controls.h"
#include "p5r_input.h"
#include "p5r_menu_layout.h"
#include "p5r_gfdcam.h"
#include "p5r_hud_settings.h"
#include <d3d11_1.h>
#include <atomic>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace P5RMenu {
thread_local bool internalCall=false;
struct Scope { bool old=internalCall; Scope(){internalCall=true;} ~Scope(){internalCall=old;} };
static std::atomic<bool> open{false};
static ImGuiContext* gui=nullptr;
static ID3D11Device* device=nullptr;
static ID3D11DeviceContext1* context=nullptr;
static ID3DDeviceContextState* cleanState=nullptr;
static HWND window=nullptr;
static WNDPROC previousProc=nullptr;
static bool drawn=false, frameReady=false;
static bool keys[256]={};
static std::atomic<int> wheel{0};
static RECT oldClip{};
static bool haveClip=false;
static const char* status="Changes apply live. Save settings to keep them.";
bool IsOpen(){return open.load();}
BOOL (WINAPI* originalSetCursorPos)(int,int)=nullptr;
BOOL (WINAPI* originalSetPhysicalCursorPos)(int,int)=nullptr;
BOOL (WINAPI* originalClipCursor)(const RECT*)=nullptr;
BOOL WINAPI BlockGameCursorWarp(int x,int y){if(IsOpen()){P5RInput::RememberWarp(x,y);return TRUE;}return originalSetCursorPos(x,y);}
BOOL WINAPI BlockGamePhysicalWarp(int x,int y){return IsOpen()?TRUE:originalSetPhysicalCursorPos(x,y);}
BOOL WINAPI BlockGameCursorClip(const RECT* rect){return IsOpen() && rect?TRUE:originalClipCursor(rect);}
bool WasDrawn(){return drawn;}
static LRESULT CALLBACK InputProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
    if (open.load()) {
        if(msg==WM_MOUSEWHEEL){wheel.fetch_add(GET_WHEEL_DELTA_WPARAM(w));return 0;}
        if (msg==WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
        if ((msg>=WM_KEYFIRST && msg<=WM_KEYLAST) || (msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST)) return 0;
        // DefWindowProc performs the required raw-input cleanup; don't deliver it to the game.
        if (msg==WM_INPUT) return DefWindowProcW(h,msg,w,l);
    }
    return CallWindowProcW(previousProc,h,msg,w,l);
}
static void SetOpen(bool value) {
    if (value==open.load()) return;
    if(value) {
        haveClip=GetClipCursor(&oldClip)!=FALSE;
        POINT point{};GetCursorPos(&point);P5RInput::cursorX.store(point.x);P5RInput::cursorY.store(point.y);P5RInput::warped.store(false);
        open.store(true);
        ClipCursor(nullptr);
    } else {
        if(P5RInput::warped.exchange(false) && originalSetCursorPos && GetForegroundWindow()==window)
            originalSetCursorPos(P5RInput::cursorX.load(),P5RInput::cursorY.load());
        open.store(false);
        if(haveClip) { ClipCursor(GetForegroundWindow()==window?&oldClip:nullptr); haveClip=false; }
    }
}
void Shutdown() {
    Scope scope;
    SetOpen(false);
    if(window && previousProc && (WNDPROC)GetWindowLongPtrW(window,GWLP_WNDPROC)==InputProc)
        SetWindowLongPtrW(window,GWLP_WNDPROC,(LONG_PTR)previousProc);
    previousProc=nullptr; window=nullptr;
    if(gui) {
        auto old=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(gui);
        ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext(gui); ImGui::SetCurrentContext(old==gui?nullptr:old); gui=nullptr;
    }
    if(cleanState) cleanState->Release(); cleanState=nullptr;
    if(context) context->Release(); context=nullptr;
    device=nullptr; frameReady=false;
}
static bool Initialize(ID3D11Device* dev,ID3D11DeviceContext* ctx,HWND hwnd) {
    if(gui && device==dev && window==hwnd) return true;
    Shutdown();
    ID3D11Device1* dev1=nullptr;
    if(FAILED(dev->QueryInterface(__uuidof(ID3D11Device1),(void**)&dev1))) return false;
    D3D_FEATURE_LEVEL levels[]={dev->GetFeatureLevel()}; D3D_FEATURE_LEVEL got;
    HRESULT hr=dev1->CreateDeviceContextState(0,levels,1,D3D11_SDK_VERSION,__uuidof(ID3D11Device),&got,&cleanState);
    dev1->Release();
    if(FAILED(hr)) return false;
    if(FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)&context))) {Shutdown();return false;}
    auto old=ImGui::GetCurrentContext(); gui=ImGui::CreateContext();
    ImGui::GetIO().IniFilename=nullptr; ImGui::GetIO().LogFilename=nullptr;
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImFontConfig font; font.SizePixels=18;ImGui::GetIO().Fonts->AddFontDefault(&font);
    if(!ImGui_ImplDX11_Init(dev,ctx)) { ImGui::DestroyContext(gui); gui=nullptr; ImGui::SetCurrentContext(old); Shutdown(); return false; }
    ImGui::SetCurrentContext(old); device=dev; window=hwnd;
    SetLastError(0);
    previousProc=(WNDPROC)GetWindowLongPtrW(hwnd,GWLP_WNDPROC);
    auto replaced=(WNDPROC)SetWindowLongPtrW(hwnd,GWLP_WNDPROC,(LONG_PTR)InputProc);
    if(!replaced)previousProc=nullptr;
    if(!previousProc) {Shutdown();return false;}
    return true;
}
static bool TuningSlider(const char* label,float* value,float lo,float hi,const char* format,float step){
    ImGui::PushID(label);ImGui::TextUnformatted(label);ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    bool changed=ImGui::InputFloat("##value",value,step,step*10,format);
    ImGui::SetNextItemWidth(-1);
    changed|=ImGui::SliderFloat("##slider",value,lo,hi,format,ImGuiSliderFlags_AlwaysClamp);
    if(changed)*value=CameraControls::Clamp(*value,(std::max)(fabsf(lo),fabsf(hi)));
    if(changed)*value=(std::max)(lo,(std::min)(hi,*value));
    ImGui::PopID();return changed;
}
static bool DrawPanel(Actions& actions,bool stereo,bool ready,UINT width,UINT height) {
    if(!MenuLayout::loaded)MenuLayout::Load();
    if(width!=MenuLayout::lastWidth || height!=MenuLayout::lastHeight){MenuLayout::apply=true;MenuLayout::lastWidth=width;MenuLayout::lastHeight=height;}
    float scale=std::max(.75f,std::min(2.5f,height/1400.f))*MenuLayout::value.font;
    if(MenuLayout::apply){
        auto v=MenuLayout::Valid(MenuLayout::value);
        ImGui::SetNextWindowPos(ImVec2(v.x*width/v.scale,v.y*height/v.scale),ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(v.w*width,v.h*height),ImGuiCond_Always);
        MenuLayout::apply=false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(200,140),ImVec2(width*.95f/MenuLayout::value.scale,height*.95f/MenuLayout::value.scale));
    bool visible=true;
    if(ImGui::Begin("P5R VR | Insert closes",&visible,ImGuiWindowFlags_NoCollapse)) {
        ImGui::SetWindowFontScale(scale);
        ImVec2 pos=ImGui::GetWindowPos(),size=ImGui::GetWindowSize();
        auto prior=MenuLayout::value;
        MenuLayout::value.x=pos.x*prior.scale/width;MenuLayout::value.y=pos.y*prior.scale/height;
        MenuLayout::value.w=size.x/width;MenuLayout::value.h=size.y/height;
        static bool layoutDirty=false;
        if(prior.x!=MenuLayout::value.x || prior.y!=MenuLayout::value.y || prior.w!=MenuLayout::value.w || prior.h!=MenuLayout::value.h)layoutDirty=true;
        if(layoutDirty && !ImGui::IsMouseDown(0)){MenuLayout::Save();layoutDirty=false;}
        auto value=CameraControls::Get();bool changed=false;
        ImGui::BeginChild("Controls",ImVec2(0,-ImGui::GetFrameHeightWithSpacing()*3.5f),false);
        ImGui::SetWindowFontScale(scale);
        if(ImGui::BeginTabBar("Settings")) {
            if(ImGui::BeginTabItem("Stereo")) {
                changed|=ImGui::Checkbox("Fill headset (End)",&value.fillHeadset);
                changed|=ImGui::Checkbox("Swap eyes",&value.swapEyes);
                ImGui::BeginDisabled(!ready);
                if(ImGui::Button(stereo?"Disable VR":"Enable VR"))actions.toggleStereo=true;
                ImGui::EndDisabled();ImGui::SameLine();if(ImGui::Button("Recenter"))actions.recenter=true;
                if(!ready)ImGui::TextUnformatted("Waiting for gameplay.");
                changed|=TuningSlider("Separation (mm)",&value.eyeSeparationMm,0,128,"%.1f",.5f);
                changed|=TuningSlider("World size",&value.worldSize,.25f,4.f,"%.2f",.05f);
                ImGui::TextWrapped("Higher = larger world; preserves separation setting.");
                if(TuningSlider("Convergence",&value.convergenceShift,-1,1,"%+.3f",.01f)){value.convergenceMeters=0;changed=true;}
                if(ImGui::Button("-1")){value.convergenceShift=-1;value.convergenceMeters=0;changed=true;}
                ImGui::SameLine();if(ImGui::Button("0 (parallel)")){value.convergenceShift=0;value.convergenceMeters=0;changed=true;}
                ImGui::SameLine();if(ImGui::Button("+1")){value.convergenceShift=1;value.convergenceMeters=0;changed=true;}
                ImGui::TextWrapped("Signed eye alignment: 0 = parallel, -1 / +1 = opposite directions.");
                if(value.convergenceMeters>0)ImGui::Text("Per-eye alignment: %.3f degrees",atanf(value.eyeSeparationMm*.0005f/value.convergenceMeters)*57.29578f);
                changed|=ImGui::RadioButton("Stereo",&value.eyePreview,0);ImGui::SameLine();
                changed|=ImGui::RadioButton("Left only",&value.eyePreview,1);ImGui::SameLine();
                changed|=ImGui::RadioButton("Right only",&value.eyePreview,2);
                if(ImGui::Button("Reset stereo")){value.eyeSeparationMm=64;value.worldSize=1;value.convergenceMeters=value.convergenceShift=0;value.swapEyes=false;changed=true;}
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Camera")) {
                changed|=ImGui::Checkbox("Camera enabled (F5)",&value.enabled);
                changed|=ImGui::Checkbox("Hide Joker in first person",&value.hideBody);
                changed|=ImGui::Checkbox("Follow camera in rooms",&value.followInRooms);
                changed|=ImGui::Checkbox("First person in confidants",&value.eventFirstPerson);
                changed|=ImGui::Checkbox("First person in cutscenes",&value.cutsceneFirstPerson);
                changed|=ImGui::Checkbox("Remove cutscene bars",&value.removeLetterbox);
                changed|=ImGui::Checkbox("Remove event overlay",&value.removeEventOverlay);
                if(value.enabled&&value.hideBody){
                    int body=GfdCam::bodyStatus.load();
                    ImGui::TextWrapped("%s",body==2?"Joker hidden":body==4?"Hide failed: callback unavailable":body==3?"Hide waiting: ambiguous player":"Hide waiting for player");
                }
                changed|=ImGui::Checkbox("Head movement / lean (F12)",&value.headPosition);
                changed|=ImGui::Checkbox("Wide visibility",&value.wideVisibility);
                if(value.wideVisibility)ImGui::TextUnformatted(GfdCam::visibilityStatus==1?"Field scene culling override active":GfdCam::visibilityStatus==3?"Culling hook unavailable":GfdCam::visibilityStatus==2?"Scene validation incomplete":"Waiting for field scene");
                ImGui::TextWrapped("%s",GfdCam::CameraContext());
                static float step=10;
                ImGui::SliderFloat("Position step",&step,1,100,"%.0f");
                changed|=ImGui::InputFloat("Left / right",&value.right,step,step*10,"%.1f");
                changed|=ImGui::InputFloat("Down / up",&value.up,step,step*10,"%.1f");
                changed|=ImGui::InputFloat("Back / forward",&value.forward,step,step*10,"%.1f");
                changed|=ImGui::InputFloat("Yaw",&value.yaw,0.5f,5,"%.1f");
                changed|=ImGui::InputFloat("Pitch",&value.pitch,0.5f,5,"%.1f");
                if(ImGui::Button("Reset camera")){value.right=value.up=value.forward=value.yaw=value.pitch=0;changed=true;}
                ImGui::TextWrapped("Manual camera offset. Normal game controls. F5 restores Joker in third person.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("HUD")) {
                if(!HudSettings::loaded)HudSettings::Load();
                auto hud=HudSettings::Get();bool hudChanged=false;
                hudChanged|=ImGui::Checkbox("Menus in VR",&hud.menusInVr);
                hudChanged|=ImGui::Checkbox("Save/loading as flat screen",&hud.systemFlat);
                hudChanged|=TuningSlider("Size (m)",&hud.widthM,.3f,6.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Distance (m)",&hud.distanceM,.4f,8.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Height (m)",&hud.heightM,-2.f,2.f,"%+.2f",.05f);
                ImGui::TextUnformatted("Confidants");
                hudChanged|=TuningSlider("Confidant size (m)",&hud.cmWidthM,.3f,6.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Confidant distance (m)",&hud.cmDistanceM,.4f,8.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Confidant height (m)",&hud.cmHeightM,-2.f,2.f,"%+.2f",.05f);
                hudChanged|=TuningSlider("Top/bottom split",&hud.cmSplit,.05f,.95f,"%.2f",.01f);
                hudChanged|=TuningSlider("Left/right split",&hud.cmRightX,.3f,.98f,"%.2f",.01f);
                {   const char* names[4]={"Top-left","Top-right","Bottom-left","Bottom-right"};
                    for(int i=0;i<4;++i){ ImGui::PushID(100+i); ImGui::TextUnformatted(names[i]); ImGui::SameLine(110);
                        if(ImGui::RadioButton("With top",hud.cmPart[i]==0)){hud.cmPart[i]=0;hudChanged=true;} ImGui::SameLine();
                        if(ImGui::RadioButton("With bottom",hud.cmPart[i]==1)){hud.cmPart[i]=1;hudChanged=true;} ImGui::SameLine();
                        if(ImGui::RadioButton("Hide",hud.cmPart[i]==2)){hud.cmPart[i]=2;hudChanged=true;} ImGui::PopID(); } }
                hudChanged|=TuningSlider("Top height (m)",&hud.cmTopY,-2.f,2.f,"%+.2f",.05f);

                hudChanged|=TuningSlider("Bottom height (m)",&hud.cmBottomY,-2.f,2.f,"%+.2f",.05f);
                ImGui::TextUnformatted("Cutscenes");
                hudChanged|=TuningSlider("Cutscene size (m)",&hud.evWidthM,.3f,6.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Cutscene distance (m)",&hud.evDistanceM,.4f,8.f,"%.2f",.05f);
                hudChanged|=TuningSlider("Cutscene height (m)",&hud.evHeightM,-2.f,2.f,"%+.2f",.05f);
                if(ImGui::Button("Dock all")){for(int& m:hud.mode)m=HudSettings::Dock;hudChanged=true;HudSettings::redock=true;}
                ImGui::SameLine();if(ImGui::Button("Follow all")){for(int& m:hud.mode)m=HudSettings::Follow;hudChanged=true;}
                ImGui::SameLine();if(ImGui::Button("Re-dock in front"))HudSettings::redock=true;
                ImGui::Separator();
                if(ImGui::BeginTable("hudRegions",4,ImGuiTableFlags_SizingStretchProp)){
                    for(int i=0;i<HudSettings::kRegions;++i){
                        ImGui::PushID(i);ImGui::TableNextRow();
                        ImGui::TableNextColumn();ImGui::TextUnformatted(HudSettings::kRegion[i].name);
                        ImGui::TableNextColumn();if(ImGui::RadioButton("Follow",hud.mode[i]==HudSettings::Follow)){hud.mode[i]=HudSettings::Follow;hudChanged=true;}
                        ImGui::TableNextColumn();if(ImGui::RadioButton("Dock",hud.mode[i]==HudSettings::Dock)){hud.mode[i]=HudSettings::Dock;hudChanged=true;}
                        ImGui::TableNextColumn();if(ImGui::RadioButton("Hide",hud.mode[i]==HudSettings::Hide)){hud.mode[i]=HudSettings::Hide;hudChanged=true;}
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if(hudChanged){HudSettings::Set(hud);status="Applied. Save to keep.";}
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Comfort")) {
                ImGui::TextWrapped("Drag the title bar to move. Drag the bottom-right corner to resize.");
                static float menuSize=.65f;static bool sizing=false;
                if(!sizing)menuSize=MenuLayout::value.scale;
                ImGui::TextUnformatted("Menu size");ImGui::SetNextItemWidth(-1);
                if(ImGui::SliderFloat("##panelSize",&menuSize,.25f,1.5f,"%.2fx"))sizing=true;
                if(sizing&&!ImGui::IsItemActive()){MenuLayout::SetSize(menuSize);layoutDirty=true;sizing=false;}
                if(ImGui::Button("Smaller")){MenuLayout::SetSize(menuSize*.85f);layoutDirty=true;}
                ImGui::SameLine();if(ImGui::Button("Larger")){MenuLayout::SetSize(menuSize*1.15f);layoutDirty=true;}
                ImGui::TextWrapped("Scales the whole panel on release, including controls.");
                if(ImGui::Button("Center menu")){
                    auto& layout=MenuLayout::value;layout.x=(std::max)(0.f,(1-layout.w*layout.scale)*.5f);layout.y=(std::max)(0.f,(1-layout.h*layout.scale)*.5f);
                    MenuLayout::apply=true;layoutDirty=true;
                }
                if(ImGui::CollapsingHeader("Text adjustment"))
                    if(ImGui::SliderFloat("Text size",&MenuLayout::value.font,.6f,1.6f,"%.2fx"))layoutDirty=true;
                if(ImGui::Button("Reset placement (F10)")){MenuLayout::Reset();layoutDirty=true;}
                ImGui::TextWrapped("Placement, size and text size save automatically after release.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        if(changed){CameraControls::Set(value);status="Applied. Save to keep.";}
        ImGui::Separator();
        if(ImGui::Button("Save settings"))status=(CameraControls::Save() && MenuLayout::Save() && HudSettings::Save())?"Saved.":"Save failed: check folder access.";
        ImGui::SameLine();if(ImGui::Button("Load saved")){CameraControls::Load();MenuLayout::Load();HudSettings::Load();status="Loaded.";}
        ImGui::TextWrapped("%s",status);
        ImGui::TextDisabled("Game controls blocked while open.");
    }
    return visible;
}

// Transform vertices, clipping and cursor together, Hinny tha GOAT; input uses the inverse above.
// ImGui still lays out the full panel, so shrinking it never clips away controls.
void ScaleDrawData(ImDrawData* data,float scale,UINT width,UINT height){
    for(int n=0;n<data->CmdListsCount;++n){auto* list=data->CmdLists[n];
        for(auto& vertex:list->VtxBuffer){vertex.pos.x*=scale;vertex.pos.y*=scale;}
        for(auto& command:list->CmdBuffer){command.ClipRect.x*=scale;command.ClipRect.y*=scale;command.ClipRect.z*=scale;command.ClipRect.w*=scale;}
    }
    data->DisplaySize=ImVec2((float)width,(float)height);
}

Actions Begin(ID3D11Device* dev,ID3D11DeviceContext* ctx,HWND hwnd,UINT width,UINT height,bool stereo,bool ready) {
    Scope scope; Actions actions; drawn=false; frameReady=false;
    if(!dev || !ctx || !hwnd || !width || !height) return actions;
    bool focused=GetForegroundWindow()==hwnd;
    bool edge[256]={};
    for(int k=0;k<256;++k){bool down=focused && (GetAsyncKeyState(k)&0x8000)!=0;edge[k]=down&&!keys[k];keys[k]=down;}
    if(edge[VK_F5]){status=P5RInput::ToggleFirstPerson();}   // [R3TOGGLE] same as holding R3
    if(edge[VK_INSERT]) {
        if(!gui && !Initialize(dev,ctx,hwnd)) return actions;
        SetOpen(!IsOpen());
    }
    if(!IsOpen()) return actions;
    if(edge[VK_F10]){MenuLayout::Reset();MenuLayout::Save();}
    if(!focused){SetOpen(false);return actions;}
    if(!Initialize(dev,ctx,hwnd)) return actions;
    ClipCursor(nullptr);
    auto old=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(gui);
    auto& io=ImGui::GetIO(); if(!MenuLayout::loaded)MenuLayout::Load();
    const float panelScale=MenuLayout::value.scale;
    io.DisplaySize=ImVec2(width/panelScale,height/panelScale);
    static ULONGLONG last=0; ULONGLONG now=GetTickCount64();io.DeltaTime=last?std::max(0.001f,std::min(0.1f,(now-last)*0.001f)):1.f/60;last=now;
    io.MouseDrawCursor=true;
    POINT pt;GetCursorPos(&pt);ScreenToClient(hwnd,&pt);RECT client;GetClientRect(hwnd,&client);
    io.AddMousePosEvent(pt.x*(float)width/std::max(1L,client.right)/panelScale,pt.y*(float)height/std::max(1L,client.bottom)/panelScale);
    io.AddMouseButtonEvent(0,keys[VK_LBUTTON]);io.AddMouseButtonEvent(1,keys[VK_RBUTTON]);
    io.AddMouseWheelEvent(0,wheel.exchange(0)/(float)WHEEL_DELTA);
    const int vks[]={VK_TAB,VK_LEFT,VK_RIGHT,VK_UP,VK_DOWN,VK_RETURN,VK_ESCAPE,VK_SPACE,VK_HOME,VK_END,VK_DELETE,VK_BACK};
    const ImGuiKey ik[]={ImGuiKey_Tab,ImGuiKey_LeftArrow,ImGuiKey_RightArrow,ImGuiKey_UpArrow,ImGuiKey_DownArrow,ImGuiKey_Enter,ImGuiKey_Escape,ImGuiKey_Space,ImGuiKey_Home,ImGuiKey_End,ImGuiKey_Delete,ImGuiKey_Backspace};
    for(int i=0;i<12;++i) io.AddKeyEvent(ik[i],keys[vks[i]]);
    io.AddKeyEvent(ImGuiMod_Shift,keys[VK_SHIFT]);
    for(int i='0';i<='9';++i) if(edge[i]) io.AddInputCharacter((unsigned)i);
    for(int i=VK_NUMPAD0;i<=VK_NUMPAD9;++i) if(edge[i]) io.AddInputCharacter((unsigned)('0'+i-VK_NUMPAD0));
    if(edge[VK_OEM_MINUS]||edge[VK_SUBTRACT])io.AddInputCharacter('-');
    if(edge[VK_OEM_PERIOD]||edge[VK_DECIMAL])io.AddInputCharacter('.');
    ImGui_ImplDX11_NewFrame();ImGui::NewFrame();
    bool visible=DrawPanel(actions,stereo,ready,width,height);
    ImGui::End();ImGui::Render();ScaleDrawData(ImGui::GetDrawData(),panelScale,width,height);frameReady=true;ImGui::SetCurrentContext(old);
    if(!visible)SetOpen(false);
    return actions;
}
void Draw(ID3D11Texture2D* left,ID3D11Texture2D* right) {
    if(!frameReady || !gui || !left || drawn) return;
    Scope scope;auto old=ImGui::GetCurrentContext();ImGui::SetCurrentContext(gui);
    ID3DDeviceContextState* previous=nullptr;context->SwapDeviceContextState(cleanState,&previous);
    ID3D11Texture2D* targets[]={left,right};
    for(int i=0;i<2;++i) {
        if(!targets[i] || (i && right==left))continue;
        D3D11_TEXTURE2D_DESC desc;targets[i]->GetDesc(&desc);
        D3D11_RENDER_TARGET_VIEW_DESC vd{};vd.Format=desc.Format;
        if(vd.Format==DXGI_FORMAT_R8G8B8A8_TYPELESS)vd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        if(vd.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS)vd.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        vd.ViewDimension=desc.SampleDesc.Count>1?D3D11_RTV_DIMENSION_TEXTURE2DMS:D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv=nullptr;
        if(SUCCEEDED(device->CreateRenderTargetView(targets[i],&vd,&rtv))) {
            context->OMSetRenderTargets(1,&rtv,nullptr);ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());rtv->Release();drawn=true;
        }
    }
    // Never retain the backbuffer in the menu context state across ResizeBuffers.
    context->ClearState();context->SwapDeviceContextState(previous,nullptr);if(previous)previous->Release();
    ImGui::SetCurrentContext(old);
}
}
