// Bounded, asynchronous GPU evidence at the actual VS b2 draw binding, including
// the batched right-eye snapshot. Two 208-byte copies per sample; never wait/map
// on a game worker. Captures retain list identity through Finish/Execute.
static std::mutex g_hgMx;
static std::atomic<bool> g_hgArmed{false};
struct HeadGpuSample {
    ID3D11Buffer* frozen[2]{}; ID3D11Buffer* staging[2]{}; ID3D11Query* query=nullptr;
    ID3D11DeviceContext* context=nullptr; ID3D11CommandList* list=nullptr;
    bool captured[2]{}, executed=false, pending=false;
    float expected[2][32]{}; P5RXr::Pose drawPose{}, tag{}; bool headKnown=false, tagKnown=false;
    float historyExpected[2][16]{}, historyError[2]{}; uint32_t historySerial=0;
    uint64_t armedFrame=0; unsigned count=0;
    float matrixError[2]{}, angleError[2]{}, lensError[2]{}, tagError=0; bool resultReady=false;
} g_hg;
static void HeadGpuClearCapture() {
    if(g_hg.list)g_hg.list->Release(); g_hg.list=nullptr; g_hg.context=nullptr;
    g_hg.captured[0]=g_hg.captured[1]=false; g_hg.executed=g_hg.pending=false;
}
static void HeadGpuReset() {
    g_hgArmed.store(false);
    std::lock_guard<std::mutex> lock(g_hgMx); HeadGpuClearCapture();
    for(int e=0;e<2;++e) { if(g_hg.frozen[e])g_hg.frozen[e]->Release(); if(g_hg.staging[e])g_hg.staging[e]->Release(); g_hg.frozen[e]=g_hg.staging[e]=nullptr; }
    if(g_hg.query)g_hg.query->Release(); g_hg.query=nullptr;
}
static bool HeadGpuBuild() {
    if(g_hg.query)return true;
    if(!g_dev)return false;
    D3D11_BUFFER_DESC d{}; d.ByteWidth=208; d.Usage=D3D11_USAGE_DEFAULT;
    for(int e=0;e<2;++e) {
        if(!g_hg.frozen[e] && FAILED(g_dev->CreateBuffer(&d,nullptr,&g_hg.frozen[e])))return false;
        D3D11_BUFFER_DESC s=d; s.Usage=D3D11_USAGE_STAGING; s.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(!g_hg.staging[e] && FAILED(g_dev->CreateBuffer(&s,nullptr,&g_hg.staging[e])))return false;
    }
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0}; return SUCCEEDED(g_dev->CreateQuery(&q,&g_hg.query));
}
static void HeadGpuCapture(ID3D11DeviceContext* c, ID3D11Buffer* buffer, int eye) {
    if(!g_hgArmed.load(std::memory_order_relaxed)||!buffer)return;
    std::lock_guard<std::mutex> lock(g_hgMx);
    if(!g_hgArmed.load()||g_hg.captured[eye]||!g_hg.query)return;
    if(eye==1&&(!g_hg.captured[0]||g_hg.context!=c))return;
    D3D11_BUFFER_DESC d{};buffer->GetDesc(&d);if(d.ByteWidth<208)return;
    if(eye==0) {
        CamSet set,previous; AcquireSRWLockShared(&g_camLock);set=g_camCur;previous=g_camPrev;ReleaseSRWLockShared(&g_camLock);
        if(!set.valid)return;
        // A shared b2 buffer is also reused by shadow passes BEFORE this list's
        // head-camera upload. Match this recording's pose, not just buffer identity.
        if(!IsImmediateCtx(c)) {
            std::lock_guard<std::mutex> tagLock(g_tagMx);
            const auto it=g_ctxPose.find(c);
            if(it==g_ctxPose.end()||!it->second.uploads||it->second.last.serial!=set.pose.serial)return;
        }
        memcpy(g_hg.expected[0],set.lVP,64);memcpy(g_hg.expected[0]+16,set.lV,64);
        memcpy(g_hg.expected[1],set.rVP,64);memcpy(g_hg.expected[1]+16,set.rV,64);
        const CamSet& history=previous.valid&&previous.headValid?previous:set;
        memcpy(g_hg.historyExpected[0],history.lVP,64);memcpy(g_hg.historyExpected[1],history.rVP,64);
        g_hg.historySerial=history.pose.serial;
        g_hg.drawPose=set.pose;g_hg.headKnown=set.headValid;g_hg.context=c;
        g_hg.executed=IsImmediateCtx(c);
    }
    D3D11_BOX box{0,0,0,208,1,1};
    CopySubRegFn(c)(c,g_hg.frozen[eye],0,0,0,0,buffer,0,&box);
    g_hg.captured[eye]=true;
    if(eye==1)g_hgArmed.store(false);
}
static void HeadGpuBeforeDraw(ID3D11DeviceContext* c,int eye) {
    if(!g_hgArmed.load(std::memory_order_relaxed))return;
    if(!eye) {
        if(!CtxInScene(c))return;
        ID3D11RenderTargetView* target=nullptr;ID3D11DepthStencilView* depth=nullptr;
        c->OMGetRenderTargets(1,&target,&depth);
        const bool scene=target&&depth&&EyeSpaceTarget(target);
        if(target)target->Release();if(depth)depth->Release();if(!scene)return;
    }
    ID3D11Buffer* b=nullptr;c->VSGetConstantBuffers(2,1,&b);
    if(b && (eye || b==g_mainCamBuf.load()))HeadGpuCapture(c,b,eye);
    if(b)b->Release();
}
static void HeadGpuFinish(ID3D11DeviceContext* c,ID3D11CommandList* cl) {
    std::lock_guard<std::mutex> lock(g_hgMx);
    if(g_hg.context!=c||!g_hg.captured[0]||g_hg.list||g_hg.pending)return;
    if(cl){g_hg.list=cl;cl->AddRef();}
}
static void HeadGpuExecute(ID3D11CommandList* cl) {
    std::lock_guard<std::mutex> lock(g_hgMx);
    if(cl && g_hg.list==cl)g_hg.executed=true;
}
static float HeadGpuViewAngle(const float* a,const float* b) {
    double trace=0;for(int r=0;r<3;++r){double dot=0,aa=0,bb=0;for(int c=0;c<3;++c){dot+=(double)a[r*4+c]*b[r*4+c];aa+=(double)a[r*4+c]*a[r*4+c];bb+=(double)b[r*4+c]*b[r*4+c];}if(aa<1e-12||bb<1e-12)return -1;trace+=dot/sqrt(aa*bb);}
    double cosine=(trace-1)*.5;if(cosine>1)cosine=1;if(cosine< -1)cosine=-1;
    return (float)(acos(cosine)*57.29578);
}
static void HeadGpuBoundary(uint64_t f) {
    std::lock_guard<std::mutex> lock(g_hgMx);
    if(g_hg.pending) {
        BOOL done=FALSE;
        if(g_ctxImm->GetData(g_hg.query,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&done) {
            bool valid=true;
            for(int e=0;e<2;++e) {
                D3D11_MAPPED_SUBRESOURCE m{};
                if(FAILED(OrigMap(g_ctxImm)(g_ctxImm,g_hg.staging[e],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m))){valid=false;break;}
                const float* actual=(const float*)m.pData;
                float error=0;for(int i=0;i<32;++i)error=fmaxf(error,fabsf(actual[i]-g_hg.expected[e][i]));
                g_hg.matrixError[e]=error;g_hg.angleError[e]=HeadGpuViewAngle(actual+16,g_hg.expected[e]+16);
                float historyError=0;for(int i=0;i<16;++i)historyError=fmaxf(historyError,fabsf(actual[36+i]-g_hg.historyExpected[e][i]));
                g_hg.historyError[e]=historyError;
                Log("[headgpu] sample=%u eye=%c GPU 0x90 vs previous rendered VP maxerr=%.6f previous serial=%u (requires camhistory ACTIVE to establish slot)",g_hg.count,e?'R':'L',historyError,g_hg.historySerial);
                float inv[16],p[16]{};if(Mat4Inverse(actual+16,inv))Mat4Mul(actual,inv,p);
                g_hg.lensError[e]=-1.f;
                if(g_hg.tagKnown&&g_hg.tag.renderedFovValid&&p[0]>.01f&&p[5]>.01f) {
                    const auto& tag=g_hg.tag.renderedFov[e];
                    const float gpu[4]={atanf((p[2]-1)/p[0]),atanf((p[2]+1)/p[0]),atanf((p[6]+1)/p[5]),atanf((p[6]-1)/p[5])};
                    const float tagged[4]={tag.angleLeft,tag.angleRight,tag.angleUp,tag.angleDown};
                    float lensError=0;for(int i=0;i<4;++i)lensError=fmaxf(lensError,fabsf(gpu[i]-tagged[i]));
                    g_hg.lensError[e]=lensError*57.29578f;
                }
                Log("[headgpu] sample=%u eye=%c GPU-draw vs captured camera: matrix maxerr=%.6f view angle=%.4f deg | actual P=(%.6f,%.6f,%.6f,%.6f)",
                    g_hg.count,e?'R':'L',error,g_hg.angleError[e],p[0],p[5],p[2],p[6]);
                Log("[headgpu] sample=%u eye=%c GPU lens vs submitted lens maxerr=%.5f deg (-1=unknown)",g_hg.count,e?'R':'L',g_hg.lensError[e]);
                OrigUnmap(g_ctxImm)(g_ctxImm,g_hg.staging[e],0);
            }
            g_hg.tagError=g_hg.headKnown&&g_hg.tagKnown?QuatAngleDeg(g_hg.drawPose.orientation,g_hg.tag.orientation):-1.f;
            Log("[headgpu] sample=%u draw serial=%u submit serial=%u draw-label/tag angle=%.4f deg | known=%d/%d | GPU result=%s",
                g_hg.count,g_hg.drawPose.serial,g_hg.tag.serial,g_hg.tagError,g_hg.headKnown,g_hg.tagKnown,valid?"READ":"NOT READY");
            g_hg.resultReady=valid;HeadGpuClearCapture();
        }
        return;
    }
    if(g_hg.executed&&g_hg.captured[0]&&g_hg.captured[1]) {
        g_hg.tagKnown=P5RXr::GetRenderPose(&g_hg.tag);
        for(int e=0;e<2;++e)CopyResFn(g_ctxImm)(g_ctxImm,g_hg.staging[e],g_hg.frozen[e]);
        g_ctxImm->End(g_hg.query);g_hg.pending=true;++g_hg.count;return;
    }
    if(g_hg.context && f>g_hg.armedFrame+180) {
        Log("[headgpu] capture expired: pair=%d/%d executed=%d (no GPU correspondence claim)",g_hg.captured[0],g_hg.captured[1],g_hg.executed);
        HeadGpuClearCapture();g_hgArmed.store(false);
    }
    if(!g_hg.context&&!g_hgArmed.load()&&g_hg.count<32&&f%30==0&&P5RXr::IsRunning()&&g_dupOn.load()&&!g_stereoPaused.load()&&HeadGpuBuild()) {
        g_hg.armedFrame=f;g_hg.resultReady=false;g_hgArmed.store(true);
        Log("[headgpu] armed bounded draw capture %u/32 (VS b2, actual left draw and actual right replay)",g_hg.count+1);
    }
}
