#pragma once
#include "p5r_camera_controls.h"
namespace MenuLayout {
struct Layout {float x=.36f,y=.30f,w=.28f,h=.40f,font=1.f,scale=.65f;};
inline Layout value;
inline bool loaded=false,apply=true;
inline UINT lastWidth=0,lastHeight=0;
inline float Bound(float v,float lo,float hi,float fallback){return std::isfinite(v)?(std::max)(lo,(std::min)(hi,v)):fallback;}
inline Layout Valid(Layout v){v.scale=Bound(v.scale,.25f,1.5f,.65f);v.w=Bound(v.w,.12f,.95f,.28f);v.h=Bound(v.h,.12f,.95f,.4f);v.x=Bound(v.x,0,(std::max)(0.f,1-v.w*v.scale),.36f);v.y=Bound(v.y,0,(std::max)(0.f,1-v.h*v.scale),.3f);v.font=Bound(v.font,.6f,1.6f,1);return v;}
inline float Read(const wchar_t* key,float fallback){wchar_t b[64]={},d[64];swprintf_s(d,L"%.6f",fallback);GetPrivateProfileStringW(L"Menu",key,d,b,64,CameraControls::iniPath);wchar_t* end;float v=wcstof(b,&end);return end!=b && !*end && std::isfinite(v)?v:fallback;}
inline void Load(){value=Valid({Read(L"X",.36f),Read(L"Y",.3f),Read(L"Width",.28f),Read(L"Height",.4f),Read(L"TextScale",1),Read(L"PanelScale",.65f)});loaded=true;apply=true;}
inline bool Save(){if(!CameraControls::iniPath[0])return false;const wchar_t* keys[]={L"X",L"Y",L"Width",L"Height",L"TextScale",L"PanelScale"};float vals[]={value.x,value.y,value.w,value.h,value.font,value.scale};bool ok=true;for(int i=0;i<6;++i){wchar_t s[64];swprintf_s(s,L"%.6f",vals[i]);if(!WritePrivateProfileStringW(L"Menu",keys[i],s,CameraControls::iniPath))ok=false;}return ok;}
// Scale the complete rendered panel and invert the same transform for mouse input.
inline void SetSize(float scale){value.scale=Bound(scale,.25f,1.5f,.65f);apply=true;}
inline void Reset(){value={};apply=true;}
}
