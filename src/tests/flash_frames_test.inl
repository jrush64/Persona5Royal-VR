static void TestFlashFrames() {
    using namespace FlashFrames;
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&g_gameDevice,&level,&g_gameCtx)));
    g_context=g_gameCtx;
    D3D11_TEXTURE2D_DESC d{};d.Width=384;d.Height=216;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS; // actual game format rejected by the previous diagnostic
    std::vector<uint32_t> red(d.Width*d.Height,0x800000FF),green(red.size(),0xFF00FF00),blue(red.size(),0xFFFF0000);
    ID3D11Texture2D *left=nullptr,*right=nullptr,*hud=nullptr;
    D3D11_SUBRESOURCE_DATA data{red.data(),d.Width*4,0};
    assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,&data,&left))); // no SRV flag: exercise fallback
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE;data.pSysMem=green.data();
    assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,&data,&right)));
    data.pSysMem=blue.data();assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,&data,&hud)));
    // Sentinel immediate state must survive private deferred execution.
    D3D11_VIEWPORT vp{3,4,31,27,0,1};g_gameCtx->RSSetViewports(1,&vp);
    g_gameCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    enabled=false;Begin(left,right,hud,nullptr);assert(issued==0);
    enabled=true;frame=700;Begin(left,right,hud,nullptr);assert(active>=0&&issued==1);
    const int captured=active;End(true,XR_SUCCESS_VALUE,12345);
    D3D11_VIEWPORT after{};UINT count=1;g_gameCtx->RSGetViewports(&count,&after);assert(!memcmp(&vp,&after,sizeof(vp)));
    D3D11_PRIMITIVE_TOPOLOGY topology;g_gameCtx->IAGetPrimitiveTopology(&topology);assert(topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    g_gameCtx->UpdateSubresource(left,0,nullptr,green.data(),d.Width*4,0); // source changes before readback
    g_gameCtx->Flush(); // test only; production never flushes or waits
    for(int i=0;i<1000&&slots[captured].pending;++i){Poll();if(slots[captured].pending)Sleep(1);}
    assert(!slots[captured].pending);
    writer.Stop();assert(writer.written==1&&!writer.failed);
    wchar_t path[MAX_PATH];swprintf_s(path,L"%s\\frames.bin",writer.directory);
    FILE* f=_wfopen(path,L"rb");assert(f);uint32_t header[16];assert(fread(header,1,sizeof(header),f)==sizeof(header));
    std::vector<uint32_t> pixels(Bytes/4);assert(fread(pixels.data(),1,Bytes,f)==Bytes);fclose(f);
    assert(header[0]==0x46523550&&header[4]==700&&header[5]==7&&header[6]==1&&header[12]==12345);
    for(unsigned i=0;i<W*H;++i){assert(pixels[i]==0x800000FF);assert(pixels[i+W*H]==0xFF00FF00);assert(pixels[i+2*W*H]==0xFFFF0000);assert(pixels[i+3*W*H]==0);}
    {   // no budget lock-out any more: later frames are still accepted (rolling ring)
        issued=5000;enabled=true;Begin(left,right,hud,nullptr);assert(active>=0&&issued==5001);End(true,XR_SUCCESS_VALUE,1);
        for(int i=0;i<200;++i){Poll();Sleep(1);}writer.Stop();
    }
    {   // flash onset detector: panel goes opaque AND matches the world = flash; opaque unrelated art / slow fade = not
        auto make=[&](uint8_t hudAlpha,bool copyWorld){
            Packet p;p.h[5]=7;p.pixels.assign(Bytes,0);
            for(unsigned i=0;i<W*H;++i){
                const uint8_t g=uint8_t((i*37u+(i/W)*11u)&255u);
                uint8_t* l=&p.pixels[i*4];l[0]=g;l[1]=uint8_t(g/2);l[2]=uint8_t(255-g);l[3]=255;
                uint8_t* h=&p.pixels[size_t(W)*H*4*2+i*4];
                if(copyWorld){h[0]=g;h[1]=uint8_t(g/2);h[2]=uint8_t(255-g);}else{h[0]=h[1]=h[2]=uint8_t(((i/W)>54)?200:30);}
                h[3]=hudAlpha;}
            return p;};
        float prev=0.04f,a=0,c=0;
        assert(Writer::Onset(make(255,true),prev,a,c)&&a>0.99f&&c>0.9f);          // world copy, alpha 0.04 -> 1.0
        prev=0.04f;assert(!Writer::Onset(make(255,false),prev,a,c));             // opaque dialogue art: low match
        prev=0.9f;assert(!Writer::Onset(make(255,true),prev,a,c));               // already opaque: no new onset
        prev=0.04f;assert(!Writer::Onset(make(90,true),prev,a,c));               // slow/low-alpha: below 0.6
        Packet noHud=make(255,true);noHud.h[5]=3;prev=0.04f;assert(!Writer::Onset(noHud,prev,a,c));   // HUD channel missing
    }
    enabled=false;Release();left->Release();right->Release();hud->Release();
    g_gameCtx->Release();g_gameDevice->Release();g_gameCtx=g_context=nullptr;g_gameDevice=nullptr;
    puts("PASS: flash recorder captures typeless/full-frame L/R/UI with alpha, retains pre-overwrite pixels, preserves immediate state, obeys gate, has no budget lock-out, detects the world-copy flash onset and writes async output");
}
