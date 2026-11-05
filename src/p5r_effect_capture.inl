// Single-draw, left-eye evidence. Copies surround the SAME original draw.
// Resource creation and nonblocking readback run at Present; workers only record copies.
struct EffectCapture {
    ID3D11Texture2D* image[3]{}; // before, after, depth (center crops)
    ID3D11Buffer* buffers[28]{};
    ID3D11Query* done=nullptr;
    ID3D11CommandList* list=nullptr;
    ID3D11DeviceContext* context=nullptr;
    D3D11_TEXTURE2D_DESC color{},depth{};
    UINT bytes[28]{},first[28]{},count[28]{};
    uintptr_t binding[28]{};
    CamSet camera{};
    D3D11_BLEND_DESC blend{};
    D3D11_DEPTH_STENCIL_DESC depthState{};
    uintptr_t vs=0,ps=0;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv[16]{};
    D3D11_TEXTURE2D_DESC srvTexture[16]{};
    uintptr_t srvResource[16]{};
    UINT kind=0,args[5]{},x=0,y=0,w=0,h=0;
    uint64_t frame=0;
    unsigned issued=0;
    bool requested=false,ready=false,recording=false,pending=false,executed=false;
} g_effect;
static std::mutex g_effectMx;
static wchar_t g_effectDir[MAX_PATH]{};
static void EffectReleaseResources() {
    for(auto*& p:g_effect.image){if(p)p->Release();p=nullptr;}
    for(auto*& p:g_effect.buffers){if(p)p->Release();p=nullptr;}
    if(g_effect.done)g_effect.done->Release();g_effect.done=nullptr;
    if(g_effect.list)g_effect.list->Release();g_effect.list=nullptr;
    g_effect.ready=g_effect.recording=g_effect.pending=g_effect.executed=false;
    g_effect.context=nullptr;
}
static void EffectReset(){std::lock_guard<std::mutex> lock(g_effectMx);EffectReleaseResources();g_effect.requested=false;}
static bool EffectFormat(DXGI_FORMAT f) {
    return f==DXGI_FORMAT_R8G8B8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_R16G16B16A16_FLOAT ||
        f==DXGI_FORMAT_R32G32B32A32_FLOAT;
}
static bool EffectBuild() {
    EffectReleaseResources();
    auto d=g_effect.color;d.Width=g_effect.w;d.Height=g_effect.h;
    d.MipLevels=d.ArraySize=1;d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(int i=0;i<2;++i)if(FAILED(g_dev->CreateTexture2D(&d,nullptr,&g_effect.image[i])))return false;
    // D3D11 depth/stencil copies must cover the whole subresource.
    if(g_effect.depth.Width){d.Format=g_effect.depth.Format;d.Width=g_effect.depth.Width;d.Height=g_effect.depth.Height;if(FAILED(g_dev->CreateTexture2D(&d,nullptr,&g_effect.image[2])))return false;}
    D3D11_BUFFER_DESC b{};b.ByteWidth=16384;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(auto*& buffer:g_effect.buffers)if(FAILED(g_dev->CreateBuffer(&b,nullptr,&buffer)))return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
    g_effect.ready=SUCCEEDED(g_dev->CreateQuery(&q,&g_effect.done));return g_effect.ready;
}
static ID3D11Texture2D* EffectTexture(ID3D11View* view) {
    if(!view)return nullptr;ID3D11Resource* r=nullptr;view->GetResource(&r);
    ID3D11Texture2D* t=nullptr;if(r){r->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&t);r->Release();}return t;
}
static bool EffectBefore(ID3D11DeviceContext* c,UINT kind,UINT a,UINT b,UINT d,UINT e,UINT f,bool force=false) {
    const uint64_t frame=g_frame.load();
    if(!force && (frame%60 || !FpAuditWanted()))return false;
    std::lock_guard<std::mutex> lock(g_effectMx);
    if(g_effect.pending||g_effect.recording||(!force&&g_effect.issued>=48))return false;
    ID3D11BlendState* bs=nullptr;FLOAT factor[4];UINT mask;c->OMGetBlendState(&bs,factor,&mask);
    D3D11_BLEND_DESC blend{};if(bs){bs->GetDesc(&blend);bs->Release();}
    if(!force&&!blend.RenderTarget[0].BlendEnable)return false;
    ID3D11RenderTargetView* rt=nullptr;ID3D11DepthStencilView* ds=nullptr;c->OMGetRenderTargets(1,&rt,&ds);
    D3D11_RENDER_TARGET_VIEW_DESC rv{};if(rt)rt->GetDesc(&rv);
    ID3D11Texture2D* color=EffectTexture(rt),*depth=EffectTexture(ds);
    D3D11_TEXTURE2D_DESC cd{},dd{};if(color)color->GetDesc(&cd);if(depth)depth->GetDesc(&dd);
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};if(ds)ds->GetDesc(&dv);
    bool valid=color && rv.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && rv.Texture2D.MipSlice==0 &&
        cd.SampleDesc.Count==1 && cd.ArraySize==1 && EffectFormat(cd.Format) && (force||EyeSpaceTarget(rt));
    if(depth && (dv.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D || dv.Texture2D.MipSlice!=0 || dd.SampleDesc.Count!=1 || dd.ArraySize!=1 || dd.MipLevels!=1 || dd.Width!=cd.Width || dd.Height!=cd.Height)) {depth->Release();depth=nullptr;dd={};}
    if(rt)rt->Release();if(ds)ds->Release();
    if(!valid){if(color)color->Release();if(depth)depth->Release();return false;}
    static thread_local uint64_t last=~0ull;static thread_local unsigned ordinal=0;
    if(last!=frame){last=frame;ordinal=0;}
    const unsigned index=ordinal++;
    // If a scene has fewer transparent draws, do not wait forever for an ordinal.
    const unsigned target=frame>g_effect.frame+600 ? 0 : g_effect.issued%16;
    if(!force && index!=target){color->Release();if(depth)depth->Release();return false;}
    if(!g_effect.ready||cd.Format!=g_effect.color.Format||cd.Width!=g_effect.color.Width||cd.Height!=g_effect.color.Height||dd.Format!=g_effect.depth.Format){
        g_effect.color=cd;g_effect.depth=dd;g_effect.w=(std::min)(512u,cd.Width);g_effect.h=(std::min)(512u,cd.Height);
        g_effect.x=(cd.Width-g_effect.w)/2;g_effect.y=(cd.Height-g_effect.h)/2;g_effect.requested=true;g_effect.ready=false;
        color->Release();if(depth)depth->Release();return false;
    }
    g_effect.frame=frame;g_effect.context=c;g_effect.blend=blend;
    g_effect.kind=kind;UINT args[]={a,b,d,e,f};memcpy(g_effect.args,args,sizeof(args));
    g_effect.camera=CamSetsCached()[0];
    ID3D11VertexShader* vs=nullptr;ID3D11PixelShader* ps=nullptr;c->VSGetShader(&vs,nullptr,nullptr);c->PSGetShader(&ps,nullptr,nullptr);
    g_effect.vs=(uintptr_t)vs;g_effect.ps=(uintptr_t)ps;if(vs)vs->Release();if(ps)ps->Release();
    ID3D11DepthStencilState* dss=nullptr;UINT stencil;c->OMGetDepthStencilState(&dss,&stencil);
    g_effect.depthState={};if(dss){dss->GetDesc(&g_effect.depthState);dss->Release();}
    ID3D11DeviceContext1* c1=nullptr;c->QueryInterface(__uuidof(ID3D11DeviceContext1),(void**)&c1);
    memset(g_effect.bytes,0,sizeof(g_effect.bytes));
    for(int stage=0;stage<2;++stage){ID3D11Buffer* cb[14]{};UINT first[14]{},count[14]{};BGetCBs(c,c1,stage!=0,cb,first,count);
        for(int slot=0;slot<14;++slot){int i=stage*14+slot;g_effect.binding[i]=(uintptr_t)cb[slot];g_effect.first[i]=first[slot];g_effect.count[i]=count[slot];
            if(cb[slot]){D3D11_BUFFER_DESC bd{};cb[slot]->GetDesc(&bd);UINT offset=first[slot]*16;
                if(offset<bd.ByteWidth){UINT n=(std::min)(16384u,bd.ByteWidth-offset);if(c1&&count[slot])n=(std::min)(n,count[slot]*16);
                    D3D11_BOX box{offset,0,0,offset+n,1,1};CopySubRegFn(c)(c,g_effect.buffers[i],0,0,0,0,cb[slot],0,&box);g_effect.bytes[i]=n;}
                cb[slot]->Release();}}
    }if(c1)c1->Release();
    ID3D11ShaderResourceView* srvs[16]{};c->PSGetShaderResources(0,16,srvs);
    memset(g_effect.srv,0,sizeof(g_effect.srv));memset(g_effect.srvTexture,0,sizeof(g_effect.srvTexture));memset(g_effect.srvResource,0,sizeof(g_effect.srvResource));
    for(int i=0;i<16;++i)if(srvs[i]){srvs[i]->GetDesc(&g_effect.srv[i]);auto t=EffectTexture(srvs[i]);if(t){t->GetDesc(&g_effect.srvTexture[i]);g_effect.srvResource[i]=(uintptr_t)t;t->Release();}srvs[i]->Release();}
    D3D11_BOX crop{g_effect.x,g_effect.y,0,g_effect.x+g_effect.w,g_effect.y+g_effect.h,1};
    CopySubRegFn(c)(c,g_effect.image[0],0,0,0,0,color,0,&crop);
    if(depth)CopySubRegFn(c)(c,g_effect.image[2],0,0,0,0,depth,0,nullptr);
    color->Release();if(depth)depth->Release();g_effect.recording=true;return true;
}
static void EffectAfter(ID3D11DeviceContext* c,bool active) {
    if(!active)return;std::lock_guard<std::mutex> lock(g_effectMx);
    if(!g_effect.recording||g_effect.context!=c)return;
    ID3D11RenderTargetView* rt=nullptr;c->OMGetRenderTargets(1,&rt,nullptr);auto color=EffectTexture(rt);if(rt)rt->Release();
    if(!color){g_effect.recording=false;return;}
    D3D11_BOX crop{g_effect.x,g_effect.y,0,g_effect.x+g_effect.w,g_effect.y+g_effect.h,1};
    CopySubRegFn(c)(c,g_effect.image[1],0,0,0,0,color,0,&crop);color->Release();c->End(g_effect.done);
    g_effect.recording=false;g_effect.pending=true;g_effect.executed=IsImmediateCtx(c);++g_effect.issued;
}
static void EffectFinish(ID3D11DeviceContext* c,ID3D11CommandList* list) {
    std::lock_guard<std::mutex> lock(g_effectMx);
    if(g_effect.pending&&g_effect.context==c&&!g_effect.list){if(list){list->AddRef();g_effect.list=list;}else{g_effect.pending=false;g_effect.context=nullptr;}}
}
static void EffectExecute(ID3D11CommandList* list){std::lock_guard<std::mutex> lock(g_effectMx);if(list&&list==g_effect.list)g_effect.executed=true;}
static bool EffectDirectory() {
    if(g_effectDir[0])return true;wchar_t base[MAX_PATH]{};GetModuleFileNameW((HMODULE)&__ImageBase,base,MAX_PATH);auto slash=wcsrchr(base,L'\\');if(!slash)return false;*slash=0;
    SYSTEMTIME t{};GetLocalTime(&t);swprintf_s(g_effectDir,L"%s\\p5r_effect_%04u%02u%02u_%02u%02u%02u",base,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    if(!CreateDirectoryW(g_effectDir,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){g_effectDir[0]=0;return false;}return true;
}
static bool EffectWrite(const wchar_t* suffix,const void* bytes,size_t size) {
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\%03u_%s",g_effectDir,g_effect.issued,suffix);FILE* f=_wfopen(path,L"wb");if(!f)return false;
    bool ok=fwrite(bytes,1,size,f)==size;fclose(f);return ok;
}
static void EffectBoundary() {
    std::lock_guard<std::mutex> lock(g_effectMx);
    if(g_effect.pending && !g_effect.executed && g_frame.load()>g_effect.frame+600){
        Log("[effect-capture] discarded unexecuted capture; no GPU evidence claimed");EffectReleaseResources();
    }
    if(g_effect.requested&&!g_effect.pending&&!g_effect.recording){g_effect.requested=false;if(!EffectBuild())Log("[effect-capture] resource build failed; next matching draw retries");}
    if(!g_effect.pending||!g_effect.executed)return;
    BOOL done=FALSE;if(g_ctxImm->GetData(g_effect.done,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||!done)return;
    if(!EffectDirectory()){Log("[effect-capture] directory creation failed");return;}
    bool ok=true;
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\%03u_meta.txt",g_effectDir,g_effect.issued);FILE* meta=_wfopen(path,L"w");
    if(meta){fprintf(meta,"frame=%llu context=%p list=%p VS=%p PS=%p kind=%u args=%u,%u,%u,%u,%u\nsize=%ux%u crop=%u,%u,%u,%u format=%u depthFormat=%u\nblend=%u,%u,%u depth=%u,%u,%u\n",
        (unsigned long long)g_effect.frame,g_effect.context,g_effect.list,(void*)g_effect.vs,(void*)g_effect.ps,g_effect.kind,g_effect.args[0],g_effect.args[1],g_effect.args[2],g_effect.args[3],g_effect.args[4],
        g_effect.color.Width,g_effect.color.Height,g_effect.x,g_effect.y,g_effect.w,g_effect.h,g_effect.color.Format,g_effect.depth.Format,
        g_effect.blend.RenderTarget[0].SrcBlend,g_effect.blend.RenderTarget[0].DestBlend,g_effect.blend.RenderTarget[0].BlendOp,g_effect.depthState.DepthEnable,g_effect.depthState.DepthWriteMask,g_effect.depthState.DepthFunc);
    }else ok=false;
    for(int i=0;i<28;++i)if(g_effect.bytes[i]){
        D3D11_MAPPED_SUBRESOURCE map{};if(SUCCEEDED(OrigMap(g_ctxImm)(g_ctxImm,g_effect.buffers[i],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map))){
            wchar_t name[40];swprintf_s(name,L"%s_b%02d.bin",i<14?L"VS":L"PS",i%14);ok=EffectWrite(name,map.pData,g_effect.bytes[i])&&ok;OrigUnmap(g_ctxImm)(g_ctxImm,g_effect.buffers[i],0);
        }else ok=false;
        if(meta)fprintf(meta,"%s b%d=%p first=%u count=%u capturedBytes=%u\n",i<14?"VS":"PS",i%14,(void*)g_effect.binding[i],g_effect.first[i],g_effect.count[i],g_effect.bytes[i]);
    }
    for(int i=0;i<3;++i)if(g_effect.image[i]){
        D3D11_MAPPED_SUBRESOURCE map{};if(SUCCEEDED(OrigMap(g_ctxImm)(g_ctxImm,g_effect.image[i],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map))){
            const wchar_t* names[]={L"before.raw",L"after.raw",L"depth.raw"};
            UINT pitch=map.RowPitch;
            if(i==2){
                const auto fmt=g_effect.depth.Format;
                const UINT pixel=fmt==DXGI_FORMAT_R32G8X24_TYPELESS||fmt==DXGI_FORMAT_D32_FLOAT_S8X24_UINT?8:
                    (fmt==DXGI_FORMAT_R16_TYPELESS||fmt==DXGI_FORMAT_D16_UNORM?2:4);
                pitch=g_effect.w*pixel;std::vector<uint8_t> cropped(size_t(pitch)*g_effect.h);
                for(UINT y=0;y<g_effect.h;++y)memcpy(cropped.data()+size_t(y)*pitch,(uint8_t*)map.pData+size_t(y+g_effect.y)*map.RowPitch+g_effect.x*pixel,pitch);
                ok=EffectWrite(names[i],cropped.data(),cropped.size())&&ok;
            }else ok=EffectWrite(names[i],map.pData,size_t(pitch)*g_effect.h)&&ok;
            if(meta)fprintf(meta,"image%d rowPitch=%u rows=%u\n",i,pitch,g_effect.h);
            OrigUnmap(g_ctxImm)(g_ctxImm,g_effect.image[i],0);
        }else ok=false;
    }
    ok=EffectWrite(L"camera.bin",&g_effect.camera,sizeof(g_effect.camera))&&ok;
    if(meta){for(int i=0;i<16;++i)if(g_effect.srvResource[i])fprintf(meta,"PS t%d resource=%p dimension=%u format=%u texture=%ux%u samples=%u (metadata only)\n",i,(void*)g_effect.srvResource[i],g_effect.srv[i].ViewDimension,g_effect.srv[i].Format,g_effect.srvTexture[i].Width,g_effect.srvTexture[i].Height,g_effect.srvTexture[i].SampleDesc.Count);fprintf(meta,"complete=%d\n",ok);fclose(meta);}
    Log("[effect-capture] sample=%u frame=%llu %s directory=%ls (same left draw: before/after/depth + GPU VS/PS buffers)",g_effect.issued,(unsigned long long)g_effect.frame,ok?"SAVED":"INCOMPLETE",g_effectDir);
    if(g_effect.list)g_effect.list->Release();g_effect.list=nullptr;g_effect.pending=g_effect.executed=false;g_effect.context=nullptr;
}
