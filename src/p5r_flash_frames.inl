// Bounded consecutive final-source images. Readback never waits for the GPU;
// file IO runs on a worker. No game draw, camera, shader or HUD routing is changed.
namespace FlashFrames {
constexpr unsigned W=192,H=108,Channels=4,Bytes=W*H*Channels*4;
struct Packet { uint32_t h[16]{}; std::vector<uint8_t> pixels; };
// Rolling capture: the last Ring consecutive frames are kept in a circular file (position = index % Ring; the
// analyzer orders by the issued counter in the header). The worker watches the HUD channel itself: a panel that
// jumps from near-empty to near-opaque AND matches the left-eye world image is the flash. On that onset it keeps
// writing Tail more frames, closes the capture, and starts a fresh ring in a new folder. No budget can run out.
static std::atomic<uint32_t> lastEyeSpike{0};
static std::atomic<uint32_t> lastHaze{0};   // [HAZE] Present frame of the latest fog/haze step (view unchanged, colour saturation stepped)   // [CAMTRACE] probe dumps camera uploads around it
constexpr unsigned Ring=600,Tail=150,MaxCaptures=16,RecordBytes=64+Bytes;
struct Writer {
    std::mutex mx; std::condition_variable cv; std::deque<Packet> queue;
    std::thread thread; bool stop=false; std::atomic<bool> failed{false};
    unsigned written=0,dropped=0,captures=0; wchar_t base[MAX_PATH]{}; wchar_t directory[MAX_PATH]{};
    FILE* Open() {
        SYSTEMTIME t{};GetLocalTime(&t);wchar_t path[MAX_PATH]{};
        swprintf_s(directory,L"%s\\p5r_flash_%04u%02u%02u_%02u%02u%02u",base,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
        if(!CreateDirectoryW(directory,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return nullptr;
        swprintf_s(path,L"%s\\frames.bin",directory);
        return _wfopen(path,L"wb");
    }
    // returns true when this packet is the flash onset: panel becomes opaque and looks like the world
    static bool Onset(const Packet& p,float& prevAlpha,float& alphaOut,float& corrOut) {
        alphaOut=corrOut=0;
        if(p.pixels.size()!=Bytes||(p.h[5]&5u)!=5u)return false;
        const uint8_t* left=p.pixels.data();const uint8_t* hud=left+size_t(W)*H*4*2;
        const unsigned n=W*H;double sa=0,sx=0,sy=0,sxx=0,syy=0,sxy=0;
        for(unsigned i=0;i<n;++i){
            sa+=hud[i*4+3];
            const double x=0.299*left[i*4]+0.587*left[i*4+1]+0.114*left[i*4+2],y=0.299*hud[i*4]+0.587*hud[i*4+1]+0.114*hud[i*4+2];
            sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;
        }
        const double vx=sxx-sx*sx/n,vy=syy-sy*sy/n;
        corrOut=(vx>1&&vy>1)?float((sxy-sx*sy/n)/sqrt(vx*vy)):0.f;
        alphaOut=float(sa/n/255.0);
        const bool hit=alphaOut>0.6f&&prevAlpha<0.3f&&corrOut>0.5f;
        prevAlpha=alphaOut;return hit;
    }
    bool Start() {
        if(thread.joinable())return !failed;
        wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH);
        auto* slash=wcsrchr(path,L'\\');if(!slash)return false;*slash=0;wcscpy_s(base,path);
        FILE* first=Open();if(!first)return false;
        XrLog("[FLASHFRAMES] ACTIVE: rolling ring of last %u frames (192x108 RGBA L/R/HUD/overlay), flash auto-detect, %u captures max; %ls",Ring,MaxCaptures,directory);
        try { thread=std::thread([this,first]{
            FILE* file=first;unsigned index=0,remaining=0;bool armed=false;float prev=0.f;
            std::vector<uint8_t> e1,e2;uint32_t n1=0,n2=0,f1=0;   // last two left-eye thumbnails: one-frame eye spike detector
            for(;;) {
                Packet p;
                { std::unique_lock<std::mutex> lk(mx);cv.wait(lk,[this]{return stop||!queue.empty();});
                  if(queue.empty()&&stop)break;p=std::move(queue.front());queue.pop_front(); }
                if(failed||!file)continue;
                if(_fseeki64(file,__int64(index%Ring)*RecordBytes,SEEK_SET)!=0||fwrite(p.h,1,sizeof(p.h),file)!=sizeof(p.h)||fwrite(p.pixels.data(),1,p.pixels.size(),file)!=p.pixels.size()||fflush(file)!=0){failed=true;continue;}
                ++written;++index;
                float a=0,c=0;bool eyeSpike=false;
                if(p.pixels.size()==Bytes){
                    if(!e1.empty()&&!e2.empty()&&p.h[14]==n1+1&&n1==n2+1){
                        double into=0,back=0;const size_t cnt=size_t(W)*H*4;
                        for(size_t i=0;i<cnt;i+=4){for(int k=0;k<3;++k){into+=abs(int(e1[i+k])-int(e2[i+k]));back+=abs(int(p.pixels[i+k])-int(e2[i+k]));}}
                        into/=double(W)*H*3;back/=double(W)*H*3;
                        if(into>12.0&&back<0.4*into){eyeSpike=true;a=float(into);c=float(back);f1=p.h[4]-1;lastEyeSpike.store(f1);static unsigned s_n=0;if(s_n++<300)XrLog("[FLASHFRAMES] eye spike f%u size %.1f residual %.1f",f1,a,c);}
                    }
                    {   // [HAZE] fog switch: saturation of lit pixels steps between consecutive frames while the picture
                        // structure stays the same (same view). Uses frames k-1 (e1) and k (this packet).
                        auto sat=[&](const uint8_t* q,double& out){double sum=0;unsigned cnt=0;for(size_t i=0;i<size_t(W)*H*4;i+=4){int mx=(std::max)({q[i],q[i+1],q[i+2]}),mn=(std::min)({q[i],q[i+1],q[i+2]});if(mx>40){sum+=double(mx-mn)/mx;++cnt;}}out=cnt>200?sum/cnt:-1;};
                        static double s1=-1;static uint32_t hzLast=0;static unsigned hzLogs=0;
                        double s0;sat(p.pixels.data(),s0);
                        if(!e1.empty()&&p.h[14]==n1+1&&s0>=0&&s1>=0&&fabs(s0-s1)>0.06){
                            // structure: grey correlation between k-1 and k
                            double sx=0,sy=0,sxx=0,syy=0,sxy=0;const unsigned np_=W*H;
                            for(unsigned i=0;i<np_;++i){double x=e1[i*4]+e1[i*4+1]+e1[i*4+2],y=p.pixels[i*4]+p.pixels[i*4+1]+p.pixels[i*4+2];sx+=x;sy+=y;sxx+=x*x;syy+=y*y;sxy+=x*y;}
                            const double vx=sxx-sx*sx/np_,vy=syy-sy*sy/np_;const double corr=(vx>1&&vy>1)?(sxy-sx*sy/np_)/sqrt(vx*vy):0;
                            if(corr>0.85&&p.h[4]>hzLast+300){hzLast=p.h[4];lastHaze.store(p.h[4]);
                                if(hzLogs++<200)XrLog("[FLASHFRAMES] HAZE step at Present frame %u: saturation %.3f -> %.3f with the view unchanged (structure match %.2f)",p.h[4],s1,s0,corr);
                                if(!armed&&captures<MaxCaptures){armed=true;remaining=Tail;XrLog("[FLASHFRAMES] HAZE capture: saving %u more frames to %ls",Tail,directory);}}
                        }
                        s1=s0;
                    }
                    e2.swap(e1);e1.assign(p.pixels.begin(),p.pixels.begin()+size_t(W)*H*4);n2=n1;n1=p.h[14];
                }
                if(!armed&&captures<MaxCaptures&&eyeSpike){armed=true;remaining=Tail;
                    XrLog("[FLASHFRAMES] EYE SPIKE at Present frame %u (eye image jumps %.1f then returns, residual %.1f): saving %u more frames to %ls",f1,a,c,Tail,directory);}
                else if(!armed&&captures<MaxCaptures&&Onset(p,prev,a,c)){armed=true;remaining=Tail;
                    XrLog("[FLASHFRAMES] FLASH onset at Present frame %u (HUD alpha %.2f, world match %.2f): saving %u more frames to %ls",p.h[4],a,c,Tail,directory);}
                else if(armed&&remaining&&--remaining==0){
                    fclose(file);file=nullptr;++captures;armed=false;index=0;
                    XrLog("[FLASHFRAMES] capture %u closed",captures);
                    if(captures<MaxCaptures)file=Open();
                    if(file)XrLog("[FLASHFRAMES] new ring: %ls",directory);
                }
            }
            if(file)fclose(file);
        }); } catch(...) { fclose(first);return false; }
        return true;
    }
    void Push(Packet&& p) {
        std::lock_guard<std::mutex> lk(mx);
        if(queue.size()>=8||failed){++dropped;return;}
        queue.push_back(std::move(p));cv.notify_one();
    }
    void Stop() {
        if(!thread.joinable())return;
        {std::lock_guard<std::mutex> lk(mx);stop=true;}cv.notify_one();thread.join();
        XrLog("[FLASHFRAMES] writer stopped saved=%u disk-dropped=%u captures=%u error=%d",written,dropped,captures,int(failed.load()));
    }
    ~Writer(){Stop();}
};
// Process-lifetime storage: never join a worker from CRT/DllMain teardown under loader lock.
// Explicit XR shutdown calls Stop; process exit reclaims this small owner.
static Writer& writer=*new Writer;
struct Slot { ID3D11Buffer* read=nullptr; ID3D11Query* done=nullptr; bool pending=false; Packet packet; };
static Slot slots[4];
static ID3D11DeviceContext* recorder=nullptr;
static ID3D11ComputeShader* shader=nullptr;
static ID3D11Buffer* output=nullptr;
static ID3D11UnorderedAccessView* uav=nullptr;
static ID3D11Texture2D* scratch[4]{};
static bool enabled=false,broken=false; static uint64_t frame=0;
static unsigned issued=0,busy=0,rejected=0; static int active=-1;
template<class T> static void Drop(T*& p){if(p)p->Release();p=nullptr;}
static void Release() {
    active=-1;
    for(auto& s:slots){if(s.pending)++busy;Drop(s.read);Drop(s.done);s.pending=false;}
    for(auto& s:scratch)Drop(s);
    Drop(uav);Drop(output);Drop(shader);Drop(recorder);
}
static bool Build() {
    if(recorder)return true;
    if(!g_gameDevice||!g_gameCtx)return false;
    const char* code=
        "Texture2D<float4> a:register(t0);Texture2D<float4> b:register(t1);"
        "Texture2D<float4> c:register(t2);Texture2D<float4> d:register(t3);"
        "RWStructuredBuffer<uint> o:register(u0);"
        "uint pack(float4 v){uint4 p=(uint4)round(saturate(v)*255);return p.x|(p.y<<8)|(p.z<<16)|(p.w<<24);}"
        "[numthreads(8,8,1)] void main(uint3 t:SV_DispatchThreadID){"
        "if(t.x>=192||t.y>=108)return;uint w,h;uint i=t.y*192+t.x;"
        "a.GetDimensions(w,h);o[i]=pack(a.Load(int3((t.xy*2+1)*uint2(w,h)/uint2(384,216),0)));"
        "b.GetDimensions(w,h);o[i+20736]=pack(b.Load(int3((t.xy*2+1)*uint2(w,h)/uint2(384,216),0)));"
        "c.GetDimensions(w,h);o[i+41472]=pack(c.Load(int3((t.xy*2+1)*uint2(w,h)/uint2(384,216),0)));"
        "d.GetDimensions(w,h);o[i+62208]=pack(d.Load(int3((t.xy*2+1)*uint2(w,h)/uint2(384,216),0)));}";
    ID3DBlob* blob=nullptr;
    bool ok=SUCCEEDED(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&blob,nullptr));
    if(ok)ok=SUCCEEDED(g_gameDevice->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&shader));
    Drop(blob);
    D3D11_BUFFER_DESC b{};b.ByteWidth=Bytes;b.Usage=D3D11_USAGE_DEFAULT;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=4;
    if(ok)ok=SUCCEEDED(g_gameDevice->CreateBuffer(&b,nullptr,&output));
    if(ok)ok=SUCCEEDED(g_gameDevice->CreateUnorderedAccessView(output,nullptr,&uav));
    b.Usage=D3D11_USAGE_STAGING;b.BindFlags=b.MiscFlags=b.StructureByteStride=0;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
    for(auto& s:slots)if(ok)ok=SUCCEEDED(g_gameDevice->CreateBuffer(&b,nullptr,&s.read))&&SUCCEEDED(g_gameDevice->CreateQuery(&q,&s.done));
    if(ok)ok=SUCCEEDED(g_gameDevice->CreateDeferredContext(0,&recorder));
    if(!ok){Release();XrLog("[FLASHFRAMES] resource creation FAILED; recording disabled");broken=true;}
    return ok;
}
static DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R8G8B8A8_TYPELESS)return DXGI_FORMAT_R8G8B8A8_UNORM;
    if(f==DXGI_FORMAT_B8G8R8A8_TYPELESS)return DXGI_FORMAT_B8G8R8A8_UNORM;
    return f;
}
static ID3D11ShaderResourceView* View(unsigned channel,ID3D11Texture2D* source) {
    if(!source)return nullptr;
    D3D11_TEXTURE2D_DESC d{};source->GetDesc(&d);
    if(d.SampleDesc.Count!=1||d.ArraySize!=1)return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};v.Format=ViewFormat(d.Format);v.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;v.Texture2D.MipLevels=1;
    ID3D11ShaderResourceView* srv=nullptr;
    if((d.BindFlags&D3D11_BIND_SHADER_RESOURCE)&&SUCCEEDED(g_gameDevice->CreateShaderResourceView(source,&v,&srv)))return srv;
    // Backbuffers need a shader-readable copy. Never bind the source for writing.
    if(scratch[channel]){D3D11_TEXTURE2D_DESC old{};scratch[channel]->GetDesc(&old);if(old.Width!=d.Width||old.Height!=d.Height||old.Format!=d.Format||old.MipLevels!=d.MipLevels)Drop(scratch[channel]);}
    if(!scratch[channel]){d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;d.CPUAccessFlags=d.MiscFlags=0;
        if(FAILED(g_gameDevice->CreateTexture2D(&d,nullptr,&scratch[channel])))return nullptr;}
    recorder->CopyResource(scratch[channel],source);
    if(FAILED(g_gameDevice->CreateShaderResourceView(scratch[channel],&v,&srv)))return nullptr;
    return srv;
}
static void Poll() {
    if(!g_gameCtx)return;
    for(auto& s:slots)if(s.pending){BOOL done=FALSE;
        if(g_gameCtx->GetData(s.done,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK||!done)continue;
        D3D11_MAPPED_SUBRESOURCE m{};
        if(FAILED(g_gameCtx->Map(s.read,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m)))continue;
        s.packet.pixels.resize(Bytes);memcpy(s.packet.pixels.data(),m.pData,Bytes);g_gameCtx->Unmap(s.read,0);
        writer.Push(std::move(s.packet));s.pending=false;
    }
}
static void Begin(ID3D11Texture2D* left,ID3D11Texture2D* right,ID3D11Texture2D* hud,ID3D11Texture2D* overlay) {
    Poll();active=-1;
    if(!enabled||broken||!left||!right)return;
    if(!Build())return;
    for(int i=0;i<4;++i)if(!slots[i].pending){active=i;break;}
    if(active<0){++busy;return;}
    if(!writer.Start()){broken=true;active=-1;XrLog("[FLASHFRAMES] file creation FAILED; disabled");return;}
    recorder->ClearState();
    ID3D11Texture2D* sources[]={left,right,hud,overlay};ID3D11ShaderResourceView* views[4]{};unsigned mask=0;
    for(unsigned i=0;i<4;++i)if((views[i]=View(i,sources[i])))mask|=1u<<i;
    auto& s=slots[active];s.packet.h[0]=0x46523550;s.packet.h[1]=1;s.packet.h[2]=W;s.packet.h[3]=H;
    s.packet.h[4]=unsigned(frame);s.packet.h[5]=mask;s.packet.h[6]=0;s.packet.h[7]=uint32_t(-999);
    s.packet.h[8]=g_renderRecord.serial;s.packet.h[9]=g_renderRecord.spaceEpoch;
    LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);s.packet.h[10]=uint32_t(qpc.QuadPart);s.packet.h[11]=uint32_t(qpc.QuadPart>>32);
    s.packet.h[12]=s.packet.h[13]=0;s.packet.h[14]=++issued;s.packet.h[15]=busy;
    recorder->CSSetShader(shader,nullptr,0);recorder->CSSetShaderResources(0,4,views);recorder->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    recorder->Dispatch(W/8,(H+7)/8,1);
    recorder->CopyResource(s.read,output);recorder->End(s.done);
    ID3D11CommandList* commands=nullptr;HRESULT hr=recorder->FinishCommandList(FALSE,&commands);
    for(auto*& v:views)Drop(v);
    if(FAILED(hr)||!commands){++rejected;active=-1;return;}
    g_gameCtx->ExecuteCommandList(commands,TRUE);commands->Release();s.pending=true;
    if(mask!=((hud?7u:3u)|(overlay?8u:0u))){++rejected;if(rejected<8)XrLog("[FLASHFRAMES] missing source view mask=%u, frame=%llu",mask,frame);}
    if(issued==1||issued%1800==0)XrLog("[FLASHFRAMES] queued=%u (ring %u) GPU-busy=%u incomplete=%u",issued,Ring,busy,rejected);
}
static void End(bool submitted,XrResult result,XrTime display) {
    if(active<0)return;auto& h=slots[active].packet.h;
    h[6]=submitted;h[7]=uint32_t(result);h[12]=uint32_t(display);h[13]=uint32_t(uint64_t(display)>>32);active=-1;
}
}
