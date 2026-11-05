// Read-only first-person diagnostics. CPU upload evidence is explicitly separate
// from the existing asynchronous GPU draw check. No render state is modified.
static bool FpAuditWanted() {
    const auto s=CameraControls::Get();
    return !g_screenView.load() && g_dupOn.load() && !g_stereoPaused.load() &&
        GfdCam::EventRunning() && (GfdCam::ConfidantRunning()?s.eventFirstPerson:s.cutsceneFirstPerson);
}
static void FpAuditCenter(const float* v,float* p) {
    for(int j=0;j<3;++j)p[j]=-(v[j]*v[3]+v[4+j]*v[7]+v[8+j]*v[11]);
}
static unsigned FpAuditMatrixMask(const float* data,UINT bytes,const CamSet* sets) {
    unsigned mask=0;
    for(UINT o=0;o+64<=bytes;o+=16)for(int k=0;k<2;++k){
        const auto& s=sets[k];if(!s.valid)continue;
        const float* m=data+o/4;
        if(!memcmp(m,s.oV,64)||!memcmp(m,s.oVP,64)||!memcmp(m,s.oVt,64)||!memcmp(m,s.oVPt,64))mask|=1u<<(k*3);
        if(!memcmp(m,s.lV,64)||!memcmp(m,s.lVP,64)||!memcmp(m,s.lVt,64)||!memcmp(m,s.lVPt,64))mask|=2u<<(k*3);
        if(!memcmp(m,s.rV,64)||!memcmp(m,s.rVP,64)||!memcmp(m,s.rVt,64)||!memcmp(m,s.rVPt,64))mask|=4u<<(k*3);
    }
    return mask;
}
static void FpAuditDraw(ID3D11DeviceContext* c) {
    const uint64_t frame=g_frame.load();
    static thread_local uint64_t sampled=~0ull;
    static std::atomic<unsigned> total{0};
    if(frame%120 || sampled==frame || total.load()>=64)return;
    if(!FpAuditWanted())return;
    if(!CtxInScene(c))return;
    ID3D11BlendState* blend=nullptr;FLOAT factor[4];UINT sampleMask;
    c->OMGetBlendState(&blend,factor,&sampleMask);
    D3D11_BLEND_DESC bd{};if(blend){blend->GetDesc(&bd);blend->Release();}
    if(!bd.RenderTarget[0].BlendEnable)return;
    ID3D11RenderTargetView* rt=nullptr;ID3D11DepthStencilView* depth=nullptr;
    c->OMGetRenderTargets(1,&rt,&depth);
    const bool scene=rt&&depth&&EyeSpaceTarget(rt);
    if(rt)rt->Release();if(depth)depth->Release();if(!scene)return;
    sampled=frame;const unsigned id=total.fetch_add(1);if(id>=64)return;
    const CamSet* sets=CamSetsCached();const auto& k=sets[0];
    if(!k.valid)return;
    float native[3],left[3],right[3],inverse[16],projection[16]{};
    FpAuditCenter(k.oV,native);FpAuditCenter(k.lV,left);FpAuditCenter(k.rV,right);
    if(Mat4Inverse(k.oV,inverse))Mat4Mul(k.oVP,inverse,projection);
    ID3D11PixelShader* ps=nullptr;c->PSGetShader(&ps,nullptr,nullptr);
    Log("[fp-audit] sample=%u frame=%llu context=%p deferred=%d ps=%p blend=%u/%u op=%u serial=%u native=(%.3f %.3f %.3f) left=(%.3f %.3f %.3f) right=(%.3f %.3f %.3f) depth-P=(%.7f %.7f) CPU-upload snapshot, NOT GPU proof",
        id,(unsigned long long)frame,c,(int)IsDeferredContext(c),ps,(unsigned)bd.RenderTarget[0].SrcBlend,(unsigned)bd.RenderTarget[0].DestBlend,(unsigned)bd.RenderTarget[0].BlendOp,k.pose.serial,
        native[0],native[1],native[2],left[0],left[1],left[2],right[0],right[1],right[2],projection[10],projection[11]);
    if(ps)ps->Release();
    Batch* batch=BatchOf(c);
    ID3D11DeviceContext1* c1=nullptr;c->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)&c1);
    for(int stage=0;stage<2;++stage){
        ID3D11Buffer* cb[14]{};UINT first[14]{},count[14]{};
        BGetCBs(c,c1,stage!=0,cb,first,count);
        for(int slot=0;slot<14;++slot)if(cb[slot]){
            bool known=false;unsigned mask=0;UINT bytes=0;
            if(batch&&batch->ctx==c){
                auto found=batch->bytes.find(cb[slot]);
                if(found!=batch->bytes.end()){
                    const auto& data=found->second;
                    const size_t offset=size_t(first[slot])*16;
                    if(offset<data.size()){
                        bytes=(UINT)(data.size()-offset);
                        if(c1&&count[slot])bytes=(std::min)(bytes,count[slot]*16);
                        mask=FpAuditMatrixMask((const float*)(data.data()+offset),bytes,sets);known=true;
                    }
                }
            }
            Log("[fp-audit-cb] sample=%u %s b%d=%p first=%u count=%u cached=%d bytes=%u exactMatrixMask=0x%X (current native/L/R=1/2/4; previous=8/16/32; zero=unidentified)",
                id,stage?"PS":"VS",slot,cb[slot],first[slot],count[slot],known,bytes,mask);
            cb[slot]->Release();
        }
    }
    if(c1)c1->Release();
}
