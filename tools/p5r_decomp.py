#!/usr/bin/env python3
# Recover Persona 5 Royal projection from the probe-logged View + ViewProj (camCB 0, buffer 60B79360).
# Layout established from the log: each 64-byte block = 16 floats printed as 4 rows of 4 (m0..m15),
# row-major with translation in the 4th COLUMN (m3,m7,m11) and bottom row (m12..15)=0,0,0,1 for View.
import math

# ---- exact numbers from p5rprobe_log.txt, camCB 0 (dst=60B79360) ----
View = [  # off=0x0040  [A]
    -0.6000, -0.2588,  0.7570, -134607.0000,
    -0.1608,  0.9659,  0.2028,  -42383.0781,
    -0.7837, -0.0000, -0.6212,  187034.5000,
     0.0000,  0.0000,  0.0000,       1.0000,
]
ViewProj = [  # off=0x0000  [D]
     2.1998, -1.1274, -2.7679,  504528.0312,
     1.3695,  0.4635,  0.8974, -290716.8750,
     0.4562, -0.7945,  0.6855, -175565.1875,
     0.8048,  0.2724,  0.5274, -165008.2812,
]
camPos_logged = (58989.6172, 6100.0703, 226672.2812)  # off=0x0080 first float3

def M(a): return [a[0:4], a[4:8], a[8:12], a[12:16]]
def mul(A, B):
    return [[sum(A[i][k]*B[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def transpose(A): return [[A[j][i] for j in range(4)] for i in range(4)]
def inverse(A):
    # Gauss-Jordan on [A | I]
    n=4
    a=[row[:]+[1.0 if i==j else 0.0 for j in range(n)] for i,row in enumerate(A)]
    for col in range(n):
        piv=max(range(col,n), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col])<1e-12: raise ValueError("singular")
        a[col],a[piv]=a[piv],a[col]
        f=a[col][col]
        a[col]=[x/f for x in a[col]]
        for r in range(n):
            if r!=col:
                f=a[r][col]
                a[r]=[a[r][k]-f*a[col][k] for k in range(2*n)]
    return [row[n:] for row in a]
def show(name,A):
    print(f"{name}:")
    for r in A: print("   " + "  ".join(f"{x: 12.5f}" for x in r))

Vm=M(View); VPm=M(ViewProj)

# ---- test: does the 0x0000 block belong to the SAME camera as 0x0040/0x0080? ----
# A view-projection maps the eye (camPos) to clip w~=0. And its bottom-row xyz = camera forward axis.
def apply(Mx, v): return [sum(Mx[i][j]*v[j] for j in range(4)) for i in range(4)]
clip = apply(VPm, [camPos_logged[0], camPos_logged[1], camPos_logged[2], 1.0])
print(f"0x0000 * (camPos,1) -> clip = ({clip[0]:.1f}, {clip[1]:.1f}, {clip[2]:.1f}, w={clip[3]:.1f})")
print("   (if 0x0000 were THIS camera's viewproj, w would be ~0; the eye projects to infinity)")
fwd_vp = (VPm[3][0], VPm[3][1], VPm[3][2])
fwd_v  = (Vm[2][0],  Vm[2][1],  Vm[2][2])
print(f"0x0000 bottom-row axis = {tuple(round(x,4) for x in fwd_vp)}")
print(f"0x0040 view fwd  axis  = {tuple(round(x,4) for x in fwd_v)}")
print("   (equal => same camera; different => 0x0000 is a DIFFERENT view than 0x0040)")
print()

# 1) confirm camPos = -R^T . t  (validates row-major/[R|t] reading of View)
R=[[Vm[i][j] for j in range(3)] for i in range(3)]
t=[Vm[0][3],Vm[1][3],Vm[2][3]]
cam=[-sum(R[k][i]*t[k] for k in range(3)) for i in range(3)]  # -R^T t
print("camPos recovered from View : ", tuple(round(c,1) for c in cam))
print("camPos logged at 0x0080    : ", camPos_logged)
print()

# 2) recover projection under the plausible conventions and see which is canonical perspective
Vinv=inverse(Vm)
cands={
  "P = ViewProj . View^-1        (row-major, M*p)": mul(VPm, Vinv),
  "P = View^-1 . ViewProj        (alt order)"     : mul(Vinv, VPm),
  "P = (VP^T . V^-T) [col-major]"                 : transpose(mul(inverse(transpose(Vm)), transpose(VPm))),
}
for name,P in cands.items():
    show(name,P)
    # canonical perspective (row-major, M*p, D3D 0..1 depth) looks like:
    #  [ sx  0   0   0 ]
    #  [ 0   sy  0   0 ]
    #  [ 0   0   A   B ]   with A=f/(f-n) or zf/(zf-zn), B=-n*A     (or transposed variants)
    #  [ 0   0   1   0 ]   (w = +z_view)
    # detect by counting near-zero off-diagonal structure
    def near0(x): return abs(x)<0.01
    diagish = near0(P[0][1]) and near0(P[0][2]) and near0(P[1][0]) and near0(P[1][2])
    print(f"   -> looks diagonal-ish (sx,sy isolated)? {diagish}")
    if diagish:
        sx,sy=P[0][0],P[1][1]
        # vertical FOV from sy = 1/tan(fovY/2); horizontal from sx
        try:
            fovY=math.degrees(2*math.atan(1.0/abs(sy)))
            fovX=math.degrees(2*math.atan(1.0/abs(sx)))
            print(f"   -> sx={sx:.5f} sy={sy:.5f}  => FOV_y={fovY:.2f} deg  FOV_x={fovX:.2f} deg  aspect={abs(sy/sx):.4f}")
        except Exception as e:
            print("   -> fov calc failed:",e)
    print()
