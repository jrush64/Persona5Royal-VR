#!/usr/bin/env python3
# v5 capture: main camera CB = VS b2 (buf 60CD3A20). View@0x40, ViewProj@0x00, camPos@0x80,
# all the SAME camera (VP bottom-row xyz == -view forward). Recover projection + FOV.
import math

View = [  # 0x0040
    -0.6000, -0.2588,  0.7570, -134291.5469,
    -0.1608,  0.9659,  0.2028,  -42274.9062,
    -0.7837, -0.0000, -0.6212,  186587.1406,
     0.0000,  0.0000,  0.0000,       1.0000,
]
ViewProj = [  # 0x0000
    -1.9249, -0.8303,  2.4284, -430816.3750,
    -0.9169,  5.5089,  1.1568, -241103.5625,
     0.7837,  0.0000,  0.6212, -186601.7969,
     0.7837,  0.0000,  0.6212, -186587.1406,
]
camPos_logged = (58845.7109, 6077.2344, 226133.5938)  # 0x0080 row0

def M(a): return [a[0:4], a[4:8], a[8:12], a[12:16]]
def mul(A,B): return [[sum(A[i][k]*B[k][j] for k in range(4)) for j in range(4)] for i in range(4)]
def inverse(A):
    n=4; a=[row[:]+[1.0 if i==j else 0.0 for j in range(n)] for i,row in enumerate(A)]
    for col in range(n):
        piv=max(range(col,n), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col])<1e-12: raise ValueError("singular")
        a[col],a[piv]=a[piv],a[col]; f=a[col][col]; a[col]=[x/f for x in a[col]]
        for r in range(n):
            if r!=col: f=a[r][col]; a[r]=[a[r][k]-f*a[col][k] for k in range(2*n)]
    return [row[n:] for row in a]
def show(name,A):
    print(name); [print("   "+"  ".join(f"{x: 12.5f}" for x in r)) for r in A]
def apply(Mx,v): return [sum(Mx[i][j]*v[j] for j in range(4)) for i in range(4)]

Vm=M(View); VPm=M(ViewProj)

# 1) confirm b2 view -> logged camPos (this is THE main camera)
R=[[Vm[i][j] for j in range(3)] for i in range(3)]; t=[Vm[0][3],Vm[1][3],Vm[2][3]]
cam=[-sum(R[k][i]*t[k] for k in range(3)) for i in range(3)]
print("camPos from b2 View :", tuple(round(c,1) for c in cam))
print("camPos logged 0x80  :", camPos_logged)

# 2) matched-pair test: does the eye project to w~=0 under b2's viewproj?
clip=apply(VPm,[camPos_logged[0],camPos_logged[1],camPos_logged[2],1.0])
print(f"b2 VP * (camPos,1)  -> w={clip[3]:.2f}  (should be ~0 for a matched viewproj)")
print()

# 3) recover projection
P = mul(VPm, inverse(Vm))
show("P = ViewProj . View^-1", P)
def n0(x): return abs(x)<0.02
diagish = n0(P[0][1]) and n0(P[0][2]) and n0(P[1][0]) and n0(P[1][2])
print("diagonal-ish (perspective)?", diagish)
sx,sy=P[0][0],P[1][1]
if abs(sy)>1e-6 and abs(sx)>1e-6:
    print(f"sx={sx:.5f} sy={sy:.5f}  FOV_y={math.degrees(2*math.atan(1/abs(sy))):.2f}deg "
          f"FOV_x={math.degrees(2*math.atan(1/abs(sx))):.2f}deg  aspect={abs(sy/sx):.4f}")
