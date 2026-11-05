#include "../p5r_culling.h"
#include <cstdio>
#include <cassert>
// clip = M * p with M row-major (game CB layout). Camera at (100,0,50) looking down -Z (right-handed),
// 90 deg FOV (m00 = m11 = 1), D3D-style projection with w = -z_view.
static void Mul(const float* a, const float* b, float* o) { for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { o[r*4+c] = 0; for (int k = 0; k < 4; ++k) o[r*4+c] += a[r*4+k] * b[k*4+c]; } }
static float Eval(const float* p, float x, float y, float z) { return p[0]*x + p[1]*y + p[2]*z + p[3]; }
static bool Inside(const float* planes, float sign, float x, float y, float z) {
    for (int i = 0; i < 6; ++i) { float d = Eval(planes + i*4, x, y, z); if (sign > 0 ? d < 0 : d > 0) return false; } return true;
}
int main() {
    float V[16] = { 1,0,0,-100, 0,1,0,0, 0,0,1,-50, 0,0,0,1 };            // world -> view (translate by -cam)
    const float n = 10, fr = 1000;
    float P[16] = { 1,0,0,0, 0,1,0,0, 0,0,fr/(n-fr),n*fr/(n-fr), 0,0,-1,0 };
    float M[16]; Mul(P, V, M);
    float pl[24];
    for (float sign : { 1.f, -1.f }) {
        assert(P5RCulling::PlanesFromVP(M, fr, sign, pl));
        assert(Inside(pl, sign, 100, 0, 40));        // 10 in front
        assert(Inside(pl, sign, 100 + 50, 0, -50));  // 100 in front, 50 right (tan .5 < 1)
        assert(!Inside(pl, sign, 100, 0, 60));       // behind the camera
        assert(!Inside(pl, sign, 100 + 300, 0, -50));// 100 in front, 300 right (tan 3 > 1)
        assert(!Inside(pl, sign, 100, 250, -50));    // above the frustum
        assert(!Inside(pl, sign, 100, 0, 50 - 2000));// beyond the far clip
    }
    // game sign detection: GFD-style view (Expand layout: view[j*4+k], translation in [12..14]) and inside-positive planes
    float gview[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, -100,0,-50,1 };
    assert(P5RCulling::PlanesFromVP(M, fr, 1.f, pl));
    assert(P5RCulling::GamePlaneSign(gview, pl, n, fr) == 1.f);
    for (float& x : pl) x = -x;
    assert(P5RCulling::GamePlaneSign(gview, pl, n, fr) == -1.f);
    puts("PASS: precise cull planes (inside/outside/behind/far, both sign conventions) and game sign detection");
    return 0;
}

