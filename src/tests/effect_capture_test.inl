static void TestEffectCapture(ID3D11DeviceContext* c,const char* label) {
    EffectReset();
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=32;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex=nullptr;ID3D11RenderTargetView* rtv=nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&td,nullptr,&tex)));assert(SUCCEEDED(g_dev->CreateRenderTargetView(tex,nullptr,&rtv)));
    auto dt=td;dt.Format=DXGI_FORMAT_R24G8_TYPELESS;dt.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth=nullptr;ID3D11DepthStencilView* dsv=nullptr;
    assert(SUCCEEDED(g_dev->CreateTexture2D(&dt,nullptr,&depth)));
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT;dv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
    assert(SUCCEEDED(g_dev->CreateDepthStencilView(depth,&dv,&dsv)));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=256;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    float original[64];for(int i=0;i<64;++i)original[i]=float(i+10);
    D3D11_SUBRESOURCE_DATA init{original,0,0};ID3D11Buffer* cb=nullptr;
    assert(SUCCEEDED(g_dev->CreateBuffer(&bd,&init,&cb)));
    c->OMSetRenderTargets(1,&rtv,dsv);c->VSSetConstantBuffers(2,1,&cb);c->PSSetConstantBuffers(3,1,&cb);c->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,0.25f,0);
    float red[4]={1,0,0,1},green[4]={0,1,0,1},blue[4]={0,0,1,1};c->ClearRenderTargetView(rtv,red);
    assert(!EffectBefore(c,0,6,0,0,0,0,true));assert(g_effect.requested);
    {std::lock_guard<std::mutex> lock(g_effectMx);g_effect.requested=false;assert(EffectBuild());}
    assert(EffectBefore(c,0,6,0,0,0,0,true));
    c->ClearRenderTargetView(rtv,green);EffectAfter(c,true);
    c->ClearRenderTargetView(rtv,blue);float changed[64]{};c->UpdateSubresource(cb,0,nullptr,changed,0,0);
    c->ClearDepthStencilView(dsv,D3D11_CLEAR_DEPTH,0.75f,0);
    ID3D11CommandList* list=nullptr;
    if(c!=g_ctxImm){assert(!g_effect.executed);assert(SUCCEEDED(c->FinishCommandList(FALSE,&list)));EffectFinish(c,list);
        EffectExecute(nullptr);assert(!g_effect.executed);g_ctxImm->ExecuteCommandList(list,FALSE);EffectExecute(list);assert(g_effect.executed);}
    g_ctxImm->Flush();BOOL done=FALSE;for(int tries=0;tries<1000;++tries){if(g_ctxImm->GetData(g_effect.done,&done,sizeof(done),0)==S_OK&&done)break;Sleep(1);}assert(done);
    for(int i=0;i<2;++i){D3D11_MAPPED_SUBRESOURCE m{};assert(SUCCEEDED(OrigMap(g_ctxImm)(g_ctxImm,g_effect.image[i],0,D3D11_MAP_READ,0,&m)));
        auto p=(unsigned char*)m.pData;assert(p[0]==(i?0:255)&&p[1]==(i?255:0)&&p[2]==0);OrigUnmap(g_ctxImm)(g_ctxImm,g_effect.image[i],0);}
    for(int i:{2,17}){D3D11_MAPPED_SUBRESOURCE m{};assert(g_effect.bytes[i]==256);assert(SUCCEEDED(OrigMap(g_ctxImm)(g_ctxImm,g_effect.buffers[i],0,D3D11_MAP_READ,0,&m)));
        assert(!memcmp(m.pData,original,256));OrigUnmap(g_ctxImm)(g_ctxImm,g_effect.buffers[i],0);}
    ID3D11RenderTargetView* bound=nullptr;c->OMGetRenderTargets(1,&bound,nullptr);
    if(c==g_ctxImm)assert(bound==rtv);if(bound)bound->Release();
    D3D11_MAPPED_SUBRESOURCE dm{};assert(SUCCEEDED(OrigMap(g_ctxImm)(g_ctxImm,g_effect.image[2],0,D3D11_MAP_READ,0,&dm)));
    assert(abs(int((*(uint32_t*)dm.pData)&0xffffff)-4194304)<3);OrigUnmap(g_ctxImm)(g_ctxImm,g_effect.image[2],0);
    EffectBoundary();assert(!g_effect.pending);
    c->ClearState();EffectReset();if(list)list->Release();cb->Release();rtv->Release();tex->Release();dsv->Release();depth->Release();
    printf("PASS: effect capture %s preserves exact before/after pixels and GPU VS/PS bytes across overwrite, execution ownership and cleanup\n",label);
}
