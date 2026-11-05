// Real WARP D3D11 contexts; no game, injection, headset, or runtime launch.
#include "../p5r_probe.cpp"
#include <cassert>
#include <d3dcompiler.h>
static void CameraPayload(float (&data)[64], float yaw, float translation) {
    memset(data, 0, sizeof(data));
    float* v = data + 16;
    v[0] = cosf(yaw); v[2] = sinf(yaw); v[5] = 1;
    v[8] = -sinf(yaw); v[10] = cosf(yaw); v[15] = 1;
    v[3] = translation; v[7] = translation * 0.5f; v[11] = -translation;
    float projection[16] = {};
    projection[0] = 1.2f; projection[5] = 2.1f; projection[10] = -1.01f;
    projection[11] = -202.f; projection[14] = -1.f;
    Mat4Mul(projection, v, data);
    data[35] = 1;
}
static void TestCameraHistory() {
    float original[64],left[64],right[64]; CameraPayload(original,0,5000);
    CameraPayload(left,.1f,5010);CameraPayload(right,.1f,4990);
    P5RXr::Pose a{};a.serial=100;a.spaceEpoch=1;a.orientation[3]=1;
    g_camCur={};g_camPrev={};g_camSetVer.fetch_add(1);
    CamSetStore(original,left,right,&a);
    float previousLeft[16];memcpy(previousLeft,left,64);
    a.serial=101;CameraPayload(left,.2f,5010);CameraPayload(right,.2f,4990);
    CamSetStore(original,left,right,&a);
    // The game camera is stationary; only the headset changes. History must advance.
    assert(g_camPrev.valid&&g_camPrev.pose.serial==100);
    assert(!memcmp(g_camPrev.lVP,previousLeft,64));
    memcpy(original+36,original,64);
    memcpy(left+36,original,64);memcpy(right+36,original,64);
    assert(PatchMainCameraHistory(original,left,right,sizeof(original)));
    assert(!memcmp(left+36,previousLeft,64));
    assert(!memcmp(right+36,g_camPrev.rVP,64));
    // Head motion must produce a real current-to-previous projected displacement;
    // the history projection must equal the prior actual render, not the body view.
    auto screenX=[](const float* m){const float z=-10000;return (m[2]*z+m[3])/(m[14]*z+m[15]);};
    assert(fabsf(screenX(left+36)-screenX(previousLeft))<1e-6f);
    assert(fabsf(screenX(left)-screenX(left+36))>.01f);
    CamSetStore(original,left,right,&a);assert(g_camPrev.pose.serial==100); // duplicate upload
    float unknown[64];memcpy(unknown,original,sizeof(unknown));unknown[36]+=1;
    float unchangedLeft[64];memcpy(unchangedLeft,left,sizeof(left));
    assert(!PatchMainCameraHistory(unknown,left,right,sizeof(unknown)));
    assert(!memcmp(left,unchangedLeft,sizeof(left)));
    // Mixed current/previous matrices in a separate CB: a current hit must not
    // prevent the old frame's block from getting its own rendered-eye replacement.
    float next[64],nextL[64],nextR[64];CameraPayload(next,.3f,6000);
    CameraPayload(nextL,.5f,6010);CameraPayload(nextR,.5f,5990);a.serial=102;
    CamSetStore(next,nextL,nextR,&a);
    float payloadL[32],payloadR[32];memcpy(payloadL,next,64);memcpy(payloadL+16,original,64);memcpy(payloadR,payloadL,sizeof(payloadL));
    assert(PatchCamPayload(payloadL,payloadR,sizeof(payloadL))==2);
    assert(!memcmp(payloadL,nextL,64)&&!memcmp(payloadL+16,g_camPrev.lVP,64));
    a.serial=103;a.spaceEpoch=2;CamSetStore(next,nextL,nextR,&a);
    assert(!g_camPrev.valid);
    memcpy(next+36,next,64);
    assert(PatchMainCameraHistory(next,nextL,nextR,sizeof(next)));
    assert(!memcmp(nextL+36,nextL,64)); // reset history at recenter
    puts("PASS: stationary-body head motion advances camera history, exact 0x90 history maps to prior eyes, duplicate uploads retain history, unknown bytes stay untouched, mixed payloads and recenter reset");
}
static void TestSharedHeadFilter() {
    HeadOrientationFilter filter;
    P5RXr::Pose raw{};raw.orientation[3]=1;raw.serial=1;raw.time=10000000;raw.spaceEpoch=1;
    auto first=filter.Apply(raw);assert(first.orientation[3]==1);
    raw.serial=2;raw.time+=10000000;raw.orientation[1]=sinf(.005f);raw.orientation[3]=cosf(.005f);raw.position[0]=.2f;
    auto smooth=filter.Apply(raw);
    assert(smooth.orientation[1]>0&&smooth.orientation[1]<raw.orientation[1]&&smooth.position[0]==raw.position[0]);
    auto repeated=filter.Apply(raw);assert(!memcmp(repeated.orientation,smooth.orientation,sizeof(smooth.orientation)));
    float length=0;for(float x:smooth.orientation)length+=x*x;assert(fabsf(length-1)<1e-6f);
    // Rendering and the tag use that exact filtered value; no independent Euler filter.
    CameraControls::Set({});float camera[64];CameraPayload(camera,0,0);
    HeadXf h{};h.rot=true;const float y=smooth.orientation[1],w=smooth.orientation[3];
    h.RT[0]=h.RT[8]=1-2*y*y;h.RT[2]=-2*y*w;h.RT[4]=1;h.RT[6]=2*y*w;
    InjectEyeShift(camera,0,&h);
    float inverse[16];assert(Mat4Inverse(camera+16,inverse));
    assert(fabsf(inverse[0]-(1-2*y*y))<1e-6f&&fabsf(inverse[2]-2*y*w)<1e-6f);
    raw.serial=3;raw.time+=10000000;raw.orientation[1]=sinf(.7f);raw.orientation[3]=cosf(.7f);
    auto fast=filter.Apply(raw);assert(fabsf(fast.orientation[1]-raw.orientation[1])<1e-6f);
    raw.serial=4;raw.spaceEpoch=2;raw.orientation[1]=0;raw.orientation[3]=1;
    auto recentered=filter.Apply(raw);assert(recentered.orientation[1]==0&&recentered.orientation[3]==1);
    raw.serial=5;raw.time+=300000000;raw.orientation[1]=sinf(.02f);raw.orientation[3]=cosf(.02f);
    auto resumed=filter.Apply(raw);assert(fabsf(resumed.orientation[1]-raw.orientation[1])<1e-6f);
    raw.serial=6;raw.time+=10000000;for(float& q:raw.orientation)q=-q;
    auto flipped=filter.Apply(raw);assert(fabsf(flipped.orientation[1]+raw.orientation[1])<1e-6f);
    puts("PASS: shared quaternion filter attenuates small noise, renders the identical declared rotation, passes fast turns, handles sign, duplicate serial, recenter and tracking gaps");
}
static void TestFullRenderedLens() {
    CameraControls::Set({});
    const bool fill=g_hmdFill.exchange(false);
    for(float offset:{-.13f,0.f,.17f}) {
        float data[64];CameraPayload(data,.3f,5000);
        float inverse[16],p[16];assert(Mat4Inverse(data+16,inverse));Mat4Mul(data,inverse,p);
        p[2]=offset;p[6]=-offset*.7f;Mat4Mul(p,data+16,data);
        P5RXr::Fov lens{};InjectEyeShift(data,0,nullptr,&lens);
        // Project each reported boundary ray through the ACTUAL rendered VP.
        // With w=-z these must land exactly on the texture's NDC edges.
        assert(fabsf(p[0]*tanf(lens.angleLeft)-p[2]+1)<.00002f);
        assert(fabsf(p[0]*tanf(lens.angleRight)-p[2]-1)<.00002f);
        assert(fabsf(p[5]*tanf(lens.angleDown)-p[6]+1)<.00002f);
        assert(fabsf(p[5]*tanf(lens.angleUp)-p[6]-1)<.00002f);
    }
    g_hmdFill.store(fill);
    puts("PASS: exact rendered off-axis lens rays map to all four texture boundaries");
}
static void TestCameraClassifier() {
    float data[64];
    for (float yaw : {0.f, 0.3f, 1.5707963f}) for (float translation : {0.f, 10.f, 5000.f}) {
        CameraPayload(data, yaw, translation);
        assert(IsMainCameraCB(data, sizeof(data)));
    }
    CameraPayload(data, 0, 0); assert(!IsMainCameraCB(data, 64));
    data[14] = 0; data[15] = 1; assert(!IsMainCameraCB(data, sizeof(data))); // ortho/UI
    CameraPayload(data, 0.3f, 10); data[16] *= 2; assert(!IsMainCameraCB(data, sizeof(data))); // not a view
    CameraPayload(data, 0, 0); data[1] = 0.8f; assert(!IsMainCameraCB(data, sizeof(data))); // skew
    CameraPayload(data, 0, 0); data[0] = NAN; assert(!IsMainCameraCB(data, sizeof(data)));
}
namespace GfdCam {void TestSetView(const float*); }
static void TestHeadCameraPosition(){
    CameraControls::Settings manual;manual.enabled=true;manual.forward=440;manual.right=-30;manual.yaw=17;CameraControls::Set(manual);
    assert(!CameraControls::Settings{}.headPosition);
    for(bool pitch:{false,true})for(float translation:{0.f,5000.f}){
        float data[64];CameraPayload(data,.35f,translation);GfdCam::TestSetView(data+16);
        float neutral[16];memcpy(neutral,data+16,sizeof(neutral));CameraControls::Apply(neutral,manual);
        float center[3];for(int j=0;j<3;++j)center[j]=-(neutral[j]*neutral[3]+neutral[4+j]*neutral[7]+neutral[8+j]*neutral[11]);
        HeadXf head{};head.rot=true;
        if(pitch){head.RT[0]=1;head.RT[5]=1;head.RT[7]=-1;}else{head.RT[2]=-1;head.RT[4]=1;head.RT[6]=1;}
        float l[64],r[64];memcpy(l,data,sizeof(l));memcpy(r,data,sizeof(r));
        InjectEyeShift(l,-32,&head);InjectEyeShift(r,32,&head);
        for(int j=0;j<3;++j)assert(fabsf((l[32+j]+r[32+j])*.5f-center[j])<.002f);
        head.pos=true;head.t[0]=100;head.t[1]=50;head.t[2]=-25;
        memcpy(l,data,sizeof(l));memcpy(r,data,sizeof(r));InjectEyeShift(l,-32,&head);InjectEyeShift(r,32,&head);
        float separation=0;
        for(int j=0;j<3;++j){
            float expected=center[j]+neutral[j]*100+neutral[4+j]*50-neutral[8+j]*25;
            assert(fabsf((l[32+j]+r[32+j])*.5f-expected)<.003f);
            separation+=(r[32+j]-l[32+j])*(r[32+j]-l[32+j]);
        }
        assert(fabsf(sqrtf(separation)-64)<.003f);
        for(auto eye:{l,r})for(int row=0;row<3;++row){
            float* v=eye+16;assert(fabsf(v[row*4]*eye[32]+v[row*4+1]*eye[33]+v[row*4+2]*eye[34]+v[row*4+3])<.003f);
        }
    }
    CameraControls::Set({});GfdCam::TestSetView(nullptr);
    puts("PASS: head rotation pivots at manual eye center, lean and eye camera positions agree with views, including world origin");
}
static void TestManualCamera() {
    float original[64], left[64], right[64];
    CameraPayload(original, 0.3f, 5000);
    for (int j=0;j<3;++j) original[32+j] = -(original[16+j]*original[19]+original[20+j]*original[23]+original[24+j]*original[27]);
    CameraControls::Set({});
    memcpy(left,original,sizeof(left)); InjectEyeShift(left,0);
    for(int i=16;i<36;++i) assert(fabsf(left[i]-original[i])<0.001f);
    CameraControls::Settings value; value.enabled=true; value.right=100; value.up=200; value.forward=300; value.yaw=30; value.pitch=20;
    CameraControls::Set(value);
    GfdCam::TestSetView(original+16);
    memcpy(left,original,sizeof(left)); memcpy(right,original,sizeof(right));
    InjectEyeShift(left,-32); InjectEyeShift(right,32);
    float distance=0;
    for(int j=0;j<3;++j) {
        float center=original[32+j]+100*original[16+j]+200*original[20+j]-300*original[24+j];
        assert(fabsf((left[32+j]+right[32+j])*0.5f-center)<0.002f);
        float reconstructed=-(left[16+j]*left[19]+left[20+j]*left[23]+left[24+j]*left[27]);
        assert(fabsf(left[32+j]-reconstructed)<0.003f);
        distance+=(right[32+j]-left[32+j])*(right[32+j]-left[32+j]);
    }
    assert(fabsf(sqrtf(distance)-64)<0.003f);
    // The source snapshot can precede a moving render camera. The same field
    // context must retain the offset; do not compare frame-varying matrices.
    float moving[64];CameraPayload(moving,.7f,6100);
    memcpy(left,moving,sizeof(left));
    float expected[16];memcpy(expected,moving+16,sizeof(expected));CameraControls::Apply(expected,value);
    InjectEyeShift(left,0);
    for(int i=0;i<16;++i)assert(fabsf(left[16+i]-expected[i])<.002f);
    GfdCam::TestSetView(nullptr);
    memcpy(left,original,sizeof(left));InjectEyeShift(left,0);
    for(int i=16;i<36;++i)assert(fabsf(left[i]-original[i])<.001f);
    value.enabled=false; CameraControls::Set(value);
    memcpy(left,original,sizeof(left)); InjectEyeShift(left,0);
    for(int i=16;i<36;++i) assert(fabsf(left[i]-original[i])<0.001f);
    wchar_t temp[MAX_PATH]; assert(GetTempPathW(MAX_PATH,temp));
    assert(GetTempFileNameW(temp,L"p5c",0,CameraControls::iniPath));
    value.enabled=true; CameraControls::Set(value); assert(CameraControls::Save());
    CameraControls::Set({}); CameraControls::Load();
    auto loaded=CameraControls::Get();
    assert(loaded.right==100 && loaded.up==200 && loaded.forward==300 && loaded.yaw==30 && loaded.pitch==20 && loaded.enabled);
    assert(WritePrivateProfileStringW(L"Camera",L"Right",L"nan",CameraControls::iniPath));
    CameraControls::Load(); assert(CameraControls::Get().right==0);
    DeleteFileW(CameraControls::iniPath); CameraControls::iniPath[0]=0;
    value.right=INFINITY; value.pitch=1000; CameraControls::Set(value);
    assert(CameraControls::Get().right==0 && CameraControls::Get().pitch==89);
    CameraControls::Set({});
    puts("PASS: manual camera identity, local movement, rotation pivot, stereo separation, disable, INI roundtrip and invalid settings");
}


// [BATCH] real draws: four pixels, each drawn with its own colour uploaded by Map(DISCARD) into ONE
// dynamic constant buffer (the per-draw upload pattern). The batched right eye must come out exactly
// like the per-draw replay, the game's bindings must be intact after every flush, and a rewrite while
// draws are pending must either flush (no pooled buffer yet) or snapshot (pool built).
static void BatchPixels(ID3D11DeviceContext* c, ID3D11Texture2D* tex, uint8_t out[4]) {
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st)));
    g_ctxImm->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE m{}; assert(SUCCEEDED(g_ctxImm->Map(st, 0, D3D11_MAP_READ, 0, &m)));
    for (int i = 0; i < 4; ++i) out[i] = ((uint8_t*)m.pData)[i * 4];
    g_ctxImm->Unmap(st, 0); st->Release();
}
static void TestBatchReplay(ID3D11DeviceContext* c, const char* label) {
    const char* src =
        "cbuffer C : register(b0) { float4 col; };"
        "float4 vs(uint i : SV_VertexID) : SV_Position { float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
        "float4 ps() : SV_Target { return col; }";
    ID3DBlob *vb = nullptr, *pb = nullptr;
    assert(SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr)));
    assert(SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr)));
    ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    assert(SUCCEEDED(g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs)));
    assert(SUCCEEDED(g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps)));
    vb->Release(); pb->Release();
    D3D11_TEXTURE2D_DESC td{}; td.Width = 16; td.Height = 9; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;   // 16:9 = eye-space target
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex))); assert(SUCCEEDED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv)));
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr; assert(SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &cb)));
    g_mirrorActive = true; g_presentTid.store(GetCurrentThreadId()); g_mirBatch.store(true); g_smartMirror.store(true);
    t_mirCreating = true;
    TwinRes* twin = nullptr; auto* trtv = (ID3D11RenderTargetView*)TwinViewOf(rtv, 0, true, &twin);
    assert(trtv && twin);
    t_mirCreating = false;
    const FLOAT black[4] = { 0, 0, 0, 1 };
    for (int pass = 0; pass < 3; ++pass) {
        c->ClearRenderTargetView(rtv, black); c->ClearRenderTargetView(trtv, black);
        OrigOMSetRTs(c)(c, 1, &rtv, nullptr);
        c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->IASetInputLayout(nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); OrigPSSetCB(c)(c, 0, 1, &cb);
        const uint32_t snaps0 = g_bSnaps.load(), noPool0 = g_bWhy[BW_NOPOOL].load(), gpu0 = g_bSnapGpu.load();
        for (int i = 0; i < 4; ++i) {
            D3D11_VIEWPORT vp{ (float)i, 0, 1, 1, 0, 1 }; c->RSSetViewports(1, &vp);
            D3D11_MAPPED_SUBRESOURCE up{};
            assert(SUCCEEDED(HookMap(c, cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &up)));
            float col[16] = { (i + 1) * 50 / 255.f, 0, 0, 1 }; memcpy(up.pData, col, sizeof(col)); HookUnmap(c, cb, 0);
            MirDraw md; assert(MirrorPreDraw(c, md));
            OrigDraw(c)(c, 3, 0);
            MirrorDraw(c, md, BCall{ 1, 3, 0, 0, 0, 0 }, [&] { OrigDraw(c)(c, 3, 0); });
            ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, nullptr);
            assert(cur == rtv); cur->Release();                               // the game's target, never the twin
        }
        assert(!BatchOf(c)->draws.empty());
        BatchFlush(c, BW_END);
        assert(BatchOf(c)->draws.empty());
        { ID3D11RenderTargetView* cur = nullptr; c->OMGetRenderTargets(1, &cur, nullptr); assert(cur == rtv); cur->Release();
          D3D11_VIEWPORT vp{}; UINT n = 1; c->RSGetViewports(&n, &vp); assert(n == 1 && vp.TopLeftX == 3.f);
          ID3D11Buffer* bound = nullptr; c->PSGetConstantBuffers(0, 1, &bound); assert(bound == cb); bound->Release(); }
        if (c != g_ctxImm) { ID3D11CommandList* cl = nullptr; assert(SUCCEEDED(c->FinishCommandList(FALSE, &cl))); g_ctxImm->ExecuteCommandList(cl, FALSE); cl->Release(); }
        uint8_t L[4], R[4]; BatchPixels(c, tex, L); BatchPixels(c, (ID3D11Texture2D*)twin->twin, R);
        for (int i = 0; i < 4; ++i) { assert(L[i] == (i + 1) * 50); assert(R[i] == (i + 1) * 50); }
        if (pass < 2) { if (pass == 0) assert(g_bWhy[BW_NOPOOL].load() - noPool0 == 3); BatchPoolBuild(); }   // warm-up: rewrites flush, pool requested and built
        else { assert(g_bSnaps.load() - snaps0 == 3); assert(g_bSnapGpu.load() == gpu0); assert(g_bWhy[BW_NOPOOL].load() == noPool0); }     // each rewrite snapshotted from known bytes (no GPU copy), one replay
    }
    printf("PASS: [BATCH] %s: per-pass right-eye replay == per-draw colours, rewrite flush then snapshot, game bindings restored\n", label);
    cb->Release(); rtv->Release(); tex->Release(); vs->Release(); ps->Release();
    BatchPoolReset();
}

// [HEADTAG] frame N's camera list (pose A) is executed and submitted while a worker has already
// uploaded frame N+1's camera (pose B) on another deferred context: the tag must be A (what was
// rendered), never B. The old behaviour (latest upload) would tag B.
static void TestHeadTag(ID3D11DeviceContext* defA, ID3D11DeviceContext* defB) {
    P5RXr::Pose A{}, B{}; A.orientation[3] = 1.f; A.serial = 7;
    A.time=101;A.renderedFovValid=true;A.renderedFov[0]={-.8f,.9f,1.f,-1.f};A.renderedFov[1]={-.9f,.8f,1.f,-1.f};
    A.renderedViewValid=true;A.renderedView[0][0]=.5f;A.renderedView[1][0]=.6f;
    B.orientation[1] = 0.0871557f; B.orientation[3] = 0.9961947f; B.serial = 8;   // 10 deg yaw
    g_exactTag.store(true);
    NoteFramePose(defA, A);
    ID3D11CommandList* clA = nullptr; assert(SUCCEEDED(defA->FinishCommandList(FALSE, &clA)));
    HeadTagFinish(defA, clA);
    NoteFramePose(defB, B);                     // next frame's upload, not executed yet
    HeadTagExecute(clA);                         // this frame executes list A
    HeadTagFrame(1, true);
    P5RXr::Pose tag{}; assert(P5RXr::GetRenderPose(&tag));
    assert(fabsf(tag.orientation[3] - 1.f) < 1e-6f && fabsf(tag.orientation[1]) < 1e-6f);
    assert(tag.serial==7&&tag.time==101&&tag.renderedFovValid&&tag.renderedFov[0].angleRight==.9f&&tag.renderedFov[1].angleLeft==-.9f);
    assert(tag.renderedViewValid&&tag.renderedView[0][0]==.5f&&tag.renderedView[1][0]==.6f);
    HeadTagFrame(2, true);                       // a frame that executed no camera list keeps the last real tag
    assert(P5RXr::GetRenderPose(&tag) && fabsf(tag.orientation[3] - 1.f) < 1e-6f);
    HeadTagFrame(3, false);                      // menu/mono frame: plain located pose
    assert(!P5RXr::GetRenderPose(&tag));
    clA->Release();
    { std::lock_guard<std::mutex> lk(g_tagMx); g_ctxPose.clear(); g_clPose.clear(); }
    puts("PASS: [HEADTAG] submit tag = pose of the executed camera list, not a later upload; no-camera frames keep it; mono frames clear it");
}
static void TestHeadGpu(ID3D11DeviceContext* c,const char* name) {
    HeadGpuReset();g_hg.count=0;assert(HeadGpuBuild());
    float original[64],left[64],right[64],overwrite[64];
    CameraPayload(original,0,5000);CameraPayload(left,.25f,5010);CameraPayload(right,.25f,4990);CameraPayload(overwrite,.7f,6000);
    P5RXr::Pose A{},B{};A.orientation[3]=1;A.serial=41;A.time=100;
    B=A;B.orientation[1]=sinf(.1f);B.orientation[3]=cosf(.1f);B.serial=42;B.time=200;
    g_camCur={};g_camPrev={};g_camSetVer.fetch_add(1);
    CamSetStore(original,left,right,&A);
    memcpy(original+36,original,64);assert(PatchMainCameraHistory(original,left,right,sizeof(original)));
    if(c!=g_ctxImm)NoteFramePose(c,A);
    D3D11_BUFFER_DESC d{};d.ByteWidth=256;d.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* source=nullptr;assert(SUCCEEDED(g_dev->CreateBuffer(&d,nullptr,&source)));
    g_hgArmed.store(true);
    OrigUpdateSubres(c)(c,source,0,nullptr,left,0,0);HeadGpuCapture(c,source,0);
    OrigUpdateSubres(c)(c,source,0,nullptr,right,0,0);HeadGpuCapture(c,source,1);
    // A later write must not corrupt either draw-time snapshot.
    OrigUpdateSubres(c)(c,source,0,nullptr,overwrite,0,0);
    if(c!=g_ctxImm) {
        ID3D11CommandList* list=nullptr;assert(SUCCEEDED(c->FinishCommandList(FALSE,&list)));
        HeadGpuFinish(c,list);assert(!g_hg.executed);
        g_ctxImm->ExecuteCommandList(list,FALSE);HeadGpuExecute(list);list->Release();
    }
    P5RXr::SetRenderPose(&B);HeadGpuBoundary(1);assert(g_hg.pending);
    g_ctxImm->Flush();
    for(unsigned i=0;i<2000&&!g_hg.resultReady;++i){HeadGpuBoundary(2);if(!g_hg.resultReady)Sleep(1);}
    assert(g_hg.resultReady&&g_hg.matrixError[0]<.002f&&g_hg.matrixError[1]<.002f);
    assert(g_hg.historyError[0]<.002f&&g_hg.historyError[1]<.002f);
    assert(g_hg.tag.serial==42&&g_hg.drawPose.serial==41&&g_hg.tagError>11&&g_hg.tagError<12);
    HeadGpuReset();g_hg.count=0;P5RXr::SetRenderPose(nullptr);source->Release();
    {std::lock_guard<std::mutex> lock(g_tagMx);g_ctxPose.erase(c);}
    printf("PASS: [HEADGPU] %s GPU draw snapshots survive overwrite and detect wrong submitted pose, with Finish/Execute ownership\n",name);
}

// [UICOVER] an opaque UI draw whose shader writes alpha 0 (P5R menus) must end up alpha 1 on the panel,
// only where it drew; pixels it did not touch stay transparent.
static void TestUiCover() {
    assert(UiCoverInit());
    D3D11_TEXTURE2D_DESC td{}; td.Width = 8; td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv)));
    D3D11_TEXTURE2D_DESC sd = td; sd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; sd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* ds = nullptr; ID3D11DepthStencilView* dsv = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&sd, nullptr, &ds)) && SUCCEEDED(g_dev->CreateDepthStencilView(ds, nullptr, &dsv)));
    g_hudDSV.store(dsv);
    g_ctxImm->ClearRenderTargetView(rtv, kHudKey); g_ctxImm->ClearDepthStencilView(dsv, D3D11_CLEAR_STENCIL | D3D11_CLEAR_DEPTH, 1.f, 0);
    const char* src = "float4 vs(uint i : SV_VertexID) : SV_Position { float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
                      "float4 ps() : SV_Target { return float4(1, 0, 0, 0); }";   // red, alpha 0: the game's opaque UI
    ID3DBlob *vb = nullptr, *pb = nullptr; ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    assert(SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr)) && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr)));
    assert(SUCCEEDED(g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs)) && SUCCEEDED(g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps)));
    vb->Release(); pb->Release();
    // the game's draw, as redirected: blending off, our stencil marking, left half of the target only
    g_ctxImm->OMSetRenderTargets(1, &rtv, dsv); g_ctxImm->OMSetDepthStencilState(g_dssMark, 1); g_ctxImm->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{ 0, 0, 4, 8, 0, 1 }; g_ctxImm->RSSetViewports(1, &vp); g_ctxImm->RSSetState(g_rsNoCull);
    g_ctxImm->VSSetShader(vs, nullptr, 0); g_ctxImm->PSSetShader(ps, nullptr, 0); g_ctxImm->IASetInputLayout(nullptr); g_ctxImm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctxImm->Draw(3, 0);
    g_ctxImm->OMSetRenderTargets(0, nullptr, nullptr);
    UiCoverFixup(tex, rtv, true);
    D3D11_TEXTURE2D_DESC rd = td; rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* rb = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&rd, nullptr, &rb)));
    g_ctxImm->CopyResource(rb, tex);
    D3D11_MAPPED_SUBRESOURCE m{}; assert(SUCCEEDED(g_ctxImm->Map(rb, 0, D3D11_MAP_READ, 0, &m)));
    const uint8_t* row = (const uint8_t*)m.pData + 4 * m.RowPitch;
    assert(row[1 * 4 + 0] == 255 && row[1 * 4 + 3] == 255);   // drawn pixel: red kept, alpha now opaque
    assert(row[6 * 4 + 0] == 0 && row[6 * 4 + 1] == 0 && row[6 * 4 + 3] == 0);   // untouched (key) pixel: transparent black
    g_ctxImm->Unmap(rb, 0); rb->Release();
    g_hudDSV.store(nullptr); dsv->Release(); ds->Release(); rtv->Release(); tex->Release(); vs->Release(); ps->Release();
    puts("PASS: [KEYCOVER] menu UI drawn with scratch alpha 0 becomes opaque where it drew, key-coloured pixels become transparent, colour kept");
}
// [COVREPLAY] field: drawn pixels take the replayed coverage alpha, undrawn (key) pixels are transparent
static void TestCovField() {   // [TWINKEY] exact alpha from the key-start panel and the white-start twin
    assert(UiCoverInit());
    D3D11_TEXTURE2D_DESC td{}; td.Width = 8; td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr, *twin = nullptr; ID3D11RenderTargetView *rtv = nullptr, *trtv = nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv)));
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &twin)) && SUCCEEDED(g_dev->CreateRenderTargetView(twin, nullptr, &trtv)) && SUCCEEDED(g_dev->CreateShaderResourceView(twin, nullptr, &g_covSRV)));
    g_ctxImm->ClearRenderTargetView(rtv, kHudKey); const FLOAT white[4] = { 1, 1, 1, 1 }; g_ctxImm->ClearRenderTargetView(trtv, white);
    // column 0-1: opaque grey 100 on both; column 2-3: 50% white (P1 = 128+key/2, P2 = 255); column 4-5: 80% black box
    auto fill = [&](ID3D11Texture2D* t, UINT x0, uint8_t r, uint8_t g, uint8_t b) { std::vector<uint8_t> px(2 * 8 * 4); for (size_t i = 0; i < px.size(); i += 4) { px[i] = r; px[i+1] = g; px[i+2] = b; px[i+3] = 255; }
        D3D11_BOX box{ x0, 0, 0, x0 + 2, 8, 1 }; g_ctxImm->UpdateSubresource(t, 0, &box, px.data(), 8, 0); };
    fill(tex, 0, 100, 100, 100); fill(twin, 0, 100, 100, 100);
    fill(tex, 2, 129, 130, 130); fill(twin, 2, 255, 255, 255);
    fill(tex, 4, 1, 1, 1); fill(twin, 4, 51, 51, 51);
    UiCoverFixup(tex, rtv, false);
    D3D11_TEXTURE2D_DESC rd = td; rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* rb = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&rd, nullptr, &rb)));
    g_ctxImm->CopyResource(rb, tex);
    D3D11_MAPPED_SUBRESOURCE m{}; assert(SUCCEEDED(g_ctxImm->Map(rb, 0, D3D11_MAP_READ, 0, &m)));
    const uint8_t* row = (const uint8_t*)m.pData + 4 * m.RowPitch;
    assert(row[0 * 4 + 0] == 100 && row[0 * 4 + 3] == 255);                       // opaque content
    assert(row[2 * 4 + 3] >= 124 && row[2 * 4 + 3] <= 131 && row[2 * 4 + 0] >= 124 && row[2 * 4 + 0] <= 130);   // 50% white: a 0.5, premultiplied 0.5
    assert(row[4 * 4 + 3] == 255 && row[4 * 4 + 0] <= 2);                          // 80% black text box: solid black
    assert(row[6 * 4 + 3] == 0 && row[6 * 4 + 0] == 0);                            // untouched: transparent
    g_ctxImm->Unmap(rb, 0); rb->Release();
    g_covSRV->Release(); g_covSRV = nullptr; trtv->Release(); twin->Release(); rtv->Release(); tex->Release();
    puts("PASS: [TWINKEY] exact alpha + premultiplied colour from key/white twins; near-solid dark boxes solid; untouched transparent");
}

// [BARPRED] a draw confined to the top strip is skipped on the GPU; one reaching the middle still draws
static void TestBarPred() {
    assert(UiCoverInit());
    D3D11_TEXTURE2D_DESC td{}; td.Width = 64; td.Height = 64; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(g_dev->CreateRenderTargetView(tex, nullptr, &rtv)));
    g_hudTex = tex;
    const char* src = "float4 vs(uint i : SV_VertexID) : SV_Position { float2 p = float2((i << 1) & 2, i & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0, 1); }"
                      "float4 ps() : SV_Target { return float4(1, 0, 0, 1); }";
    ID3DBlob *vb = nullptr, *pb = nullptr; ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr;
    assert(SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vb, nullptr)) && SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pb, nullptr)));
    assert(SUCCEEDED(g_dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs)) && SUCCEEDED(g_dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps)));
    vb->Release(); pb->Release();
    auto run = [&](float vy, float vh) {
        const FLOAT zero[4] = {}; g_ctxImm->ClearRenderTargetView(rtv, zero);
        g_ctxImm->OMSetRenderTargets(1, &rtv, nullptr); g_ctxImm->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); g_ctxImm->OMSetDepthStencilState(nullptr, 0);
        D3D11_VIEWPORT vp{ 0, vy, 64, vh, 0, 1 }; g_ctxImm->RSSetViewports(1, &vp); g_ctxImm->RSSetState(g_rsNoCull);
        g_ctxImm->VSSetShader(vs, nullptr, 0); g_ctxImm->PSSetShader(ps, nullptr, 0); g_ctxImm->IASetInputLayout(nullptr); g_ctxImm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        HudState hs; hs.redirect = true; hs.barCand = true; BarSave sv;
        assert(BarTestBegin(g_ctxImm, hs, sv)); g_ctxImm->Draw(3, 0); BarTestEnd(g_ctxImm, sv);
        g_ctxImm->Draw(3, 0); BarPredClear(g_ctxImm);
        D3D11_TEXTURE2D_DESC rd = td; rd.Usage = D3D11_USAGE_STAGING; rd.BindFlags = 0; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* rb = nullptr; assert(SUCCEEDED(g_dev->CreateTexture2D(&rd, nullptr, &rb))); g_ctxImm->CopyResource(rb, tex);
        D3D11_MAPPED_SUBRESOURCE m{}; assert(SUCCEEDED(g_ctxImm->Map(rb, 0, D3D11_MAP_READ, 0, &m)));
        const uint8_t red = ((const uint8_t*)m.pData + 2 * m.RowPitch)[4 * 10];   // a pixel in the top strip (row 2)
        g_ctxImm->Unmap(rb, 0); rb->Release(); return red;
    };
    assert(run(0, 5) == 0);      // top strip only (rows 0-4 of 64): skipped
    assert(run(0, 64) == 255);   // full height: drawn
    g_hudTex = nullptr; rtv->Release(); tex->Release(); vs->Release(); ps->Release();
    puts("PASS: [BARPRED] draws confined to the top strip are skipped on the GPU; draws reaching the middle still draw");
}
// [BARPRED] a draw confined to the top strip is skipped on the GPU; one reaching the middle still draws
static void TestStereoTuning() {
    CameraControls::Settings v;
    assert(CameraControls::HalfEye(v)==32);
    v.worldSize=2;
    assert(CameraControls::HalfEye(v)==16 && v.eyeSeparationMm==64);
    assert(CameraControls::UnitsPerMeter(v)==500);
    for(float shift:{-1.f,0.f,1.f}){
        v.convergenceShift=shift;float left[16]={},right[16]={};left[0]=right[0]=1;
        CameraControls::Converge(left,-16,v);CameraControls::Converge(right,16,v);
        assert(fabsf(left[2]+shift*.06f)<.00001f && fabsf(right[2]-shift*.06f)<.00001f);
        assert(left[0]==1 && right[0]==1);
    }
    v.convergenceShift=0;
    v.convergenceMeters=3; v.fillHeadset=false; v.swapEyes=true;
    CameraControls::Set(v);
    float a[64],b[64];CameraPayload(a,0,0);memcpy(b,a,sizeof(b));
    InjectEyeShift(a,-16);InjectEyeShift(b,16);
    auto ndc=[](float* m,float z){return (m[2]*z+m[3])/(m[14]*z+m[15]);};
    assert(fabsf(ndc(a,-1500)-ndc(b,-1500))<0.00001f);
    assert(ndc(a,-500)>ndc(b,-500));
    assert(ndc(a,-3000)<ndc(b,-3000));
    // Rotational basis and eye baseline are unchanged by convergence.
    assert(a[16]==b[16] && a[21]==b[21] && a[26]==b[26]);
    assert(fabsf(a[19]-b[19]-32)<0.001f);
    wchar_t temp[MAX_PATH];assert(GetTempPathW(MAX_PATH,temp));
    assert(GetTempFileNameW(temp,L"p5s",0,CameraControls::iniPath));
    auto signedSaved=CameraControls::Get();signedSaved.convergenceShift=-1;CameraControls::Set(signedSaved);
    assert(CameraControls::Save());CameraControls::Set({});CameraControls::Load();
    auto saved=CameraControls::Get();assert(saved.worldSize==2 && saved.eyeSeparationMm==64 && saved.convergenceMeters==3 && !saved.fillHeadset && saved.swapEyes && saved.convergenceShift==-1);
    DeleteFileW(CameraControls::iniPath);CameraControls::iniPath[0]=0;CameraControls::Set({});
    puts("PASS: independent scale/separation settings, convergence plane and near/far disparity, stereo INI roundtrip");
}

int main() {
    TestStereoTuning();
    int l=1,r=2;CameraControls::Settings order;
    auto eyes=CameraControls::SelectEyes(&l,&r,order);assert(eyes.left==&l && eyes.right==&r);
    order.swapEyes=true;eyes=CameraControls::SelectEyes(&l,&r,order);assert(eyes.left==&r && eyes.right==&l);
    order.eyePreview=1;eyes=CameraControls::SelectEyes(&l,&r,order);assert(eyes.left==&l && eyes.right==&l);
    order.eyePreview=2;eyes=CameraControls::SelectEyes(&l,&r,order);assert(eyes.left==&r && eyes.right==&r);
    float lens[16]={};lens[0]=2; lens[5]=4;
    CameraControls::FillProjection(lens,1.3f,1.1f);
    assert(1.f/lens[0]>=1.3f-0.0001f && 1.f/lens[5]>=1.1f-0.0001f);
    assert(fabsf(lens[0]/lens[5]-0.5f)<0.0001f);
    float h=lens[0],v=lens[5];CameraControls::FillProjection(lens,0,0);
    assert(lens[0]==h && lens[5]==v);
    CameraControls::FillProjection(lens,0.5f,0.5f);assert(lens[0]==h && lens[5]==v);
    {   // exact per-axis fill: each axis equals the headset half-angle; invalid input leaves the lens alone
        float exact[16]={};exact[0]=2;exact[5]=3.5f;
        CameraControls::FillProjectionExact(exact,1.28f,1.11f);
        assert(fabsf(1.f/exact[0]-1.28f)<1e-5f && fabsf(1.f/exact[5]-1.11f)<1e-5f);
        CameraControls::FillProjectionExact(exact,0,1.f);assert(fabsf(1.f/exact[0]-1.28f)<1e-5f);
        puts("PASS: exact per-axis headset fill sets each axis to the headset lens and ignores invalid FOV");
    }
    puts("PASS: headset fill covers horizontal/vertical bounds, preserves aspect, waits for valid FOV and never narrows");
    g_titleTransitionComplete.store(true); g_frame.store(100); g_stereoArmAfter.store(0);
    g_resizeCooldownUntil.store(200); g_xrSuspendUntil.store(200);
    SetStereoEnabled(true); assert(!g_dupOn.load());
    g_frame.store(201); SetStereoEnabled(true);
    assert(g_dupOn.load() && g_xrSuspendUntil.load()==200);
    SetStereoEnabled(false); assert(!g_dupOn.load());
    g_titleTransitionComplete.store(false); g_frame.store(0);
    g_resizeCooldownUntil.store(0);g_xrSuspendUntil.store(0);g_xrStartRequested.store(false);
    TestManualCamera();
    TestHeadCameraPosition();
    TestCameraClassifier();
    TestFullRenderedLens();
    TestSharedHeadFilter();
    TestCameraHistory();
    ID3D11Device *foreign = nullptr;
    ID3D11DeviceContext *foreignImm = nullptr, *foreignDef = nullptr, *gameDef = nullptr;
    D3D_FEATURE_LEVEL fl;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &g_dev, &fl, &g_ctxImm)));
    assert(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &foreign, &fl, &foreignImm)));
    assert(SUCCEEDED(g_dev->CreateDeferredContext(0, &gameDef)));
    assert(SUCCEEDED(foreign->CreateDeferredContext(0, &foreignDef)));
    P5RMenu::internalCall=true; assert(BypassGameHooks(g_ctxImm)); P5RMenu::internalCall=false;
    assert(!BypassGameHooks(g_ctxImm));
    assert(!BypassGameHooks(gameDef));
    assert(BypassGameHooks(foreignImm));
    assert(BypassGameHooks(foreignDef));
    const UINT runtime = 1;
    assert(SUCCEEDED(gameDef->SetPrivateData(kRuntimeContextTag, sizeof(runtime), &runtime)));
    assert(BypassGameHooks(gameDef));
    // Simulate a released deferred-context address reused by an immediate context.
    RegisterDeferredContext(foreignImm);
    assert(!IsDeferredContext(foreignImm));
    assert(IsDeferredContext(gameDef));
    assert(!IsDeferredContext(nullptr));
    assert(SUCCEEDED(gameDef->SetPrivateData(kRuntimeContextTag, 0, nullptr)));
    void** vt = *(void***)g_ctxImm;
    g_origCopyRes = reinterpret_cast<PFN_CopyRes>(vt[47]);
    g_origOMSetRTs = reinterpret_cast<PFN_OMSetRTs>(vt[33]);
    g_origVSSetCB = reinterpret_cast<PFN_SetCBs>(vt[7]);
    g_origPSSetCB = reinterpret_cast<PFN_SetCBs>(vt[16]);
    g_origMap = reinterpret_cast<PFN_Map>(vt[14]);
    g_origUnmap = reinterpret_cast<PFN_Unmap>(vt[15]);
    g_origUpdateSubres = reinterpret_cast<PFN_UpdateSubres>(vt[48]);
    D3D11_BUFFER_DESC legacyDesc{}; legacyDesc.ByteWidth = 256;
    legacyDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    assert(SUCCEEDED(g_dev->CreateBuffer(&legacyDesc, nullptr, &g_rightCamCB)));
    g_camCBSize = 256; g_dupOn.store(true); g_stereoPaused.store(false); g_xrWasRunning = true;
    g_lDrawIdx = 2000; UpdateMirrorActivation(100); assert(g_mirrorActive);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 8; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* original = nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &original)));
    ID3D11RenderTargetView *view1 = nullptr, *view2 = nullptr;
    assert(SUCCEEDED(g_dev->CreateRenderTargetView(original, nullptr, &view1)));
    assert(SUCCEEDED(g_dev->CreateRenderTargetView(original, nullptr, &view2)));
    g_mirFrame = 100;
    t_mirCreating = true;
    TwinRes* twin = nullptr;
    auto* twinView = (ID3D11RenderTargetView*)TwinViewOf(view1, 0, true, &twin);
    assert(twinView && twin);
    g_lDrawIdx = 8; UpdateMirrorActivation(101);
    assert(g_mirrorActive && !g_twRes.empty()); // quiet room must not flush or disable stereo
    t_mirCreating = false;
    const FLOAT red[4] = {1,0,0,1}, green[4] = {0,1,0,1};
    g_ctxImm->ClearRenderTargetView(view1, red);
    t_mirCtx = gameDef;
    const auto queuedBefore = g_mirQueued.load();
    assert(TwinViewOf(view2, 0, true) == twinView);
    assert(g_mirQueued.load() == queuedBefore); // new view address must not skip the write
    assert(twin->synced == g_mirFrame);
    assert(g_mirSyncDef.load() == 1);
    gameDef->ClearRenderTargetView(twinView, green);
    ID3D11CommandList* commands = nullptr;
    assert(SUCCEEDED(gameDef->FinishCommandList(FALSE, &commands)));
    g_ctxImm->ExecuteCommandList(commands, FALSE); commands->Release();
    t_mirCtx = nullptr;
    twin->written=g_mirFrame;
    ++g_mirFrame; // keep RIGHT history even when the left image was updated for the next frame
    MirSync(twin, original); // must not overwrite the deferred green right eye with red left eye
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* readback = nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &readback)));
    g_ctxImm->CopyResource(readback, twin->twin);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    assert(SUCCEEDED(g_ctxImm->Map(readback, 0, D3D11_MAP_READ, 0, &mapped)));
    auto* pixel = (unsigned char*)mapped.pData;
    assert(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0);
    g_ctxImm->Unmap(readback, 0); readback->Release();
    view1->Release(); view2->Release(); original->Release();
    g_stereoPaused.store(true); UpdateMirrorActivation(102); assert(!g_mirrorActive);
    GraveSweep(100, true); g_stereoPaused.store(false);
    assert(g_sharedTwinViews.empty() && g_twView.empty() && g_twRes.empty() && g_grave.empty());

    D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 256; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* camera = nullptr;
    assert(SUCCEEDED(g_dev->CreateBuffer(&cbd, nullptr, &camera)));
    assert(CamBufRight(camera, cbd.ByteWidth) == nullptr); // hook only queues
    CamBuildQueued();
    ID3D11Buffer* rightCamera = CamBufRight(camera, cbd.ByteWidth);
    assert(rightCamera);
    assert(CamBufRightIfCam(camera) == nullptr); // no uninitialized binding
    float cameraBytes[64] = {}; cameraBytes[0] = 2.0f;
    g_ctxImm->UpdateSubresource(rightCamera, 0, nullptr, cameraBytes, 0, 0);
    CamBufMark(camera, true);
    ID3D11Buffer* bindings[] = {camera}; bool changed = false;
    MirSwapCBs(bindings, 1, changed);
    assert(changed && bindings[0] == rightCamera);
    camera->Release(); // resize below must release the retained source and twin

    // Discover an axis-aligned primary camera via Map, then retain its right eye when
    // another primary camera uploads through UpdateSubresource.
    D3D11_BUFFER_DESC dynamicDesc = cbd;
    dynamicDesc.Usage = D3D11_USAGE_DYNAMIC; dynamicDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *mappedCamera = nullptr, *updatedCamera = nullptr;
    assert(SUCCEEDED(g_dev->CreateBuffer(&dynamicDesc, nullptr, &mappedCamera)));
    assert(SUCCEEDED(g_dev->CreateBuffer(&cbd, nullptr, &updatedCamera)));
    float data[64]; CameraPayload(data, 0, 10);
    for (int attempt = 0; attempt < 2; ++attempt) {
        D3D11_MAPPED_SUBRESOURCE upload{};
        assert(SUCCEEDED(HookMap(g_ctxImm, mappedCamera, 0, D3D11_MAP_WRITE_DISCARD, 0, &upload)));
        memcpy(upload.pData, data, sizeof(data)); HookUnmap(g_ctxImm, mappedCamera, 0);
        CamBuildQueued();
    }
    assert(g_cbHitMap.load() >= 2);
    ID3D11Buffer* mappedRight = CamBufRightIfCam(mappedCamera); assert(mappedRight);
    CameraPayload(data, 0.3f, 20);
    HookUpdateSubres(g_ctxImm, updatedCamera, 0, nullptr, data, 0, 0); CamBuildQueued();
    HookUpdateSubres(g_ctxImm, updatedCamera, 0, nullptr, data, 0, 0);
    ID3D11Buffer* updatedRight = CamBufRightIfCam(updatedCamera);
    assert(updatedRight && updatedRight != mappedRight);
    ID3D11Buffer* pooled[] = {mappedCamera, updatedCamera}; changed = false;
    MirSwapCBs(pooled, 2, changed);
    assert(changed && pooled[0] == mappedRight && pooled[1] == updatedRight);
    D3D11_BUFFER_DESC cpuDesc = cbd; cpuDesc.Usage = D3D11_USAGE_STAGING;
    cpuDesc.BindFlags = 0; cpuDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* cpuBuffer = nullptr;
    assert(SUCCEEDED(g_dev->CreateBuffer(&cpuDesc, nullptr, &cpuBuffer)));
    g_ctxImm->CopyResource(cpuBuffer, mappedRight);
    assert(SUCCEEDED(g_ctxImm->Map(cpuBuffer, 0, D3D11_MAP_READ, 0, &mapped)));
    assert(fabsf(((float*)mapped.pData)[19] - (10.f - g_stereoSep.load())) < 0.001f);
    g_ctxImm->Unmap(cpuBuffer, 0); cpuBuffer->Release();
    mappedCamera->Release(); updatedCamera->Release();
    {   // [BATCH] on the immediate context and on a game deferred context (deferred-variant trampolines)
        void** dvt = *(void***)gameDef;
        g_origDefDraw = reinterpret_cast<PFN_Draw>(dvt[VT_CTX_Draw]); g_origDefOMSetRTs = reinterpret_cast<PFN_OMSetRTs>(dvt[33]);
        g_origDefVSSetCB = reinterpret_cast<PFN_SetCBs>(dvt[7]); g_origDefPSSetCB = reinterpret_cast<PFN_SetCBs>(dvt[16]);
        g_origDefMap = reinterpret_cast<PFN_Map>(dvt[14]); g_origDefUnmap = reinterpret_cast<PFN_Unmap>(dvt[15]);
        g_origDefUpdateSubres = reinterpret_cast<PFN_UpdateSubres>(dvt[48]); g_origDefCopyRes = reinterpret_cast<PFN_CopyRes>(dvt[47]);
        g_origDraw = reinterpret_cast<PFN_Draw>(vt[VT_CTX_Draw]);
        TestBatchReplay(g_ctxImm, "immediate");
        TestBatchReplay(gameDef, "deferred");
        g_origCopySubReg=reinterpret_cast<PFN_CopySubReg>(vt[46]);
        g_origDefCopySubReg=reinterpret_cast<PFN_CopySubReg>(dvt[46]);
        TestHeadGpu(g_ctxImm,"immediate");TestHeadGpu(gameDef,"deferred");
        ID3D11DeviceContext* otherDef = nullptr; assert(SUCCEEDED(g_dev->CreateDeferredContext(0, &otherDef)));
        TestHeadTag(gameDef, otherDef); otherDef->Release();
        TestUiCover();
        TestCovField();
        TestBarPred();
        g_origDefCopyRes = nullptr;
    }

    // A retained command list referencing a swapchain buffer must be released before resize.
    HWND window = CreateWindowExW(0, L"STATIC", L"P5R offline resize test", WS_OVERLAPPED,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    assert(window);
    IDXGIDevice* dxdev = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory* factory = nullptr;
    assert(SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)));
    assert(SUCCEEDED(dxdev->GetAdapter(&adapter)));
    assert(SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory)));
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferDesc.Width = sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 1;
    sd.OutputWindow = window; sd.Windowed = TRUE;
    IDXGISwapChain* sc = nullptr;
    assert(SUCCEEDED(factory->CreateSwapChain(g_dev, &sd, &sc)));
    factory->Release(); adapter->Release(); dxdev->Release();
    ID3D11Texture2D* bb = nullptr; ID3D11RenderTargetView* bbview = nullptr;
    assert(SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)));
    assert(SUCCEEDED(g_dev->CreateRenderTargetView(bb, nullptr, &bbview)));
    gameDef->ClearRenderTargetView(bbview, red);
    assert(SUCCEEDED(gameDef->FinishCommandList(FALSE, &commands)));
    FrameCL retained; retained.cl = commands; g_frameCLs.push_back(std::move(retained));
    Bury(bbview); bb->Release();
    g_origResizeBuffers = reinterpret_cast<PFN_ResizeBuffers>((*(void***)sc)[13]);
    assert(SUCCEEDED(HookResizeBuffers(sc, 1, 128, 128, DXGI_FORMAT_UNKNOWN, 0)));
    assert(g_frameCLs.empty() && g_grave.empty());
    assert(g_camBufs.empty());
    assert(SUCCEEDED(HookResizeBuffers(sc, 1, 64, 64, DXGI_FORMAT_UNKNOWN, 0)));
    sc->Release(); DestroyWindow(window);
    gameDef->Release(); foreignDef->Release(); foreignImm->Release(); foreign->Release();
    g_ctxImm->Release(); g_dev->Release(); g_ctxImm = nullptr; g_dev = nullptr;
    puts("PASS: game/foreign immediate and deferred isolation, runtime tagging, stale registry dispatch");
    puts("PASS: transient view reuse, deferred right-eye contents survive immediate writes, paired resize cleanup");
    puts("PASS: queued secondary camera creation, initialization gate, right-eye binding, resize cleanup");
    puts("PASS: fixed/near-origin cameras, low-draw stereo retention, mapped camera discovery, pooled-camera readback");
}
