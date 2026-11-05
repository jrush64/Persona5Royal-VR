#pragma once
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <windows.h>
namespace P5RCulling {
// Expand normalized world-space planes to contain a sphere around the camera.
// Accept only a consistent six-plane orientation at a known interior point.
inline bool Expand(const float* view,const float* planes,float nearClip,float farClip,float padding,float* output){
 if(!std::isfinite(nearClip)||!std::isfinite(farClip)||nearClip<=0||farClip<=nearClip||farClip>1e7f)return false;
 for(int i=0;i<16;++i)if(!std::isfinite(view[i]))return false;
 for(int row=0;row<3;++row){
  float len=0;for(int col=0;col<3;++col)len+=view[col*4+row]*view[col*4+row];
  if(fabsf(len-1)>.02f)return false;
 }
 float center[3],inside[3];
 for(int j=0;j<3;++j){
  center[j]=-(view[j*4]*view[12]+view[j*4+1]*view[13]+view[j*4+2]*view[14]);
  inside[j]=center[j]-view[j*4+2]*(nearClip+farClip)*.5f;
 }
 float orientation=0;
 for(int i=0;i<6;++i){
  const float* p=planes+i*4;
  for(int j=0;j<4;++j)if(!std::isfinite(p[j]))return false;
  float len=sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
  if(fabsf(len-1)>.02f)return false;
  float d=p[0]*inside[0]+p[1]*inside[1]+p[2]*inside[2]+p[3];
  if(fabsf(d)<.01f)return false;
  float sign=d>0?1.f:-1.f;if(i&&sign!=orientation)return false;orientation=sign;
 }
 if(!std::isfinite(padding)||padding<0||padding>1e5f)return false;
 for(int i=0;i<6;++i){
  const float* p=planes+i*4;float* q=output+i*4;
  for(int j=0;j<3;++j)q[j]=p[j];
  float len=sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
  float target=(farClip+padding)*len-orientation*(p[0]*center[0]+p[1]*center[1]+p[2]*center[2]);
  q[3]=orientation*(std::max)(orientation*p[3],target);
 }
 return true;
}
// [PRECISECULL] Perf run 4: the accepted head-turn fix turned field culling OFF (every rejected
// object forced visible), so the whole district was drawn for both eyes (~4,500 draws per eye vs
// ~2,000 for the game's own view; GPU time tracks draw count). Instead, objects the game rejects
// are re-tested against the frusta actually rendered: each eye's head-rotated, headset-filled
// view-projection, widened for head-motion latency. Shared between the camera hooks (writer) and
// the field visibility predicate (reader).
inline std::atomic<bool> precise{true};
inline constexpr float kMargin = 1.35f;           // tan widening for 1-2 frames of head motion
inline SRWLOCK eyeLock = SRWLOCK_INIT;
inline float eyeVP[2][16] = {};                   // widened clip = VP * p, row-major (CB layout)
inline float eyePos[3] = {};                      // last eye position in world units (sanity check vs the game camera)
inline std::atomic<uint32_t> eyeVersion{0};
inline std::atomic<unsigned long long> eyeTime{0}; // GetTickCount64 of the last store
inline void StoreEyeVP(int eye, const float* vp, const float* pos) {  // eye 0 left, 1 right, 2 both
    AcquireSRWLockExclusive(&eyeLock);
    if (pos) memcpy(eyePos, pos, sizeof(eyePos));
    if (eye != 1) memcpy(eyeVP[0], vp, sizeof(eyeVP[0]));
    if (eye != 0) memcpy(eyeVP[1], vp, sizeof(eyeVP[1]));
    ReleaseSRWLockExclusive(&eyeLock);
    eyeVersion.fetch_add(1, std::memory_order_release);
    eyeTime.store(GetTickCount64(), std::memory_order_relaxed);
}
// Six normalized world planes from clip = M*p (D3D, w = view depth), inside >= 0 before `sign`.
inline bool PlanesFromVP(const float* M, float farClip, float sign, float* out) {
    const float* r0 = M; const float* r1 = M + 4; const float* r3 = M + 12;
    float P[6][4];
    for (int c = 0; c < 4; ++c) {
        P[0][c] = r3[c] + r0[c]; P[1][c] = r3[c] - r0[c];
        P[2][c] = r3[c] + r1[c]; P[3][c] = r3[c] - r1[c];
        P[4][c] = r3[c];                                   // in front of the eye
        P[5][c] = -r3[c];                                  // closer than the far clip
    }
    P[5][3] += farClip;
    for (int i = 0; i < 6; ++i) {
        const float len = sqrtf(P[i][0] * P[i][0] + P[i][1] * P[i][1] + P[i][2] * P[i][2]);
        if (!std::isfinite(len) || len < 1e-6f) return false;
        for (int c = 0; c < 4; ++c) { out[i * 4 + c] = sign * P[i][c] / len; if (!std::isfinite(out[i * 4 + c])) return false; }
    }
    return true;
}
// Sign convention of the game's planes: +1 if its interior evaluates positive, -1 if negative.
inline float GamePlaneSign(const float* view, const float* planes, float nearClip, float farClip) {
    if (!std::isfinite(nearClip) || !std::isfinite(farClip) || nearClip <= 0 || farClip <= nearClip) return 0;
    float center[3], inside[3];
    for (int j = 0; j < 3; ++j) {
        center[j] = -(view[j * 4] * view[12] + view[j * 4 + 1] * view[13] + view[j * 4 + 2] * view[14]);
        inside[j] = center[j] - view[j * 4 + 2] * (nearClip + farClip) * .5f;
    }
    float orientation = 0;
    for (int i = 0; i < 6; ++i) {
        const float* p = planes + i * 4;
        const float d = p[0] * inside[0] + p[1] * inside[1] + p[2] * inside[2] + p[3];
        if (!std::isfinite(d) || fabsf(d) < .01f) return 0;
        const float s = d > 0 ? 1.f : -1.f;
        if (i && s != orientation) return 0;
        orientation = s;
    }
    return orientation;
}
}
