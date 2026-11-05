// Final pixels at the XR ownership boundary, not another CPU pose-label audit.
// Four asynchronous slots. Never flush/wait for diagnostics; busy slots drop a sample.
// Capture 96 consecutive world submissions after head motion, then stop permanently.
struct FinalTraceSlot {
    ID3D11Texture2D* pixels[2][2]{}; // eye, source/destination
    ID3D11Query* query=nullptr;
    bool pending=false, left=false;
    P5RXr::Pose pose{}; uint32_t index[2]{};
    unsigned id=0,w=0,h=0,fullW=0,fullH=0,x=0,y=0;
    long long qpc=0;
    unsigned mismatched[2]{},maxByte[2]{};
    bool layerSubmitted=false; XrResult endResult=static_cast<XrResult>(-999); XrTime displayTime=0;
};
static FinalTraceSlot g_finalTrace[4];
static unsigned g_finalIssued=0,g_finalRead=0,g_finalDrop=0;
static unsigned g_finalRejected=0;
static int g_finalActive=-1;
static int g_finalSubmission=-1;
static bool g_finalStarted=false;
static P5RXr::Pose g_finalLast{};
static P5RXr::Pose g_finalLastQueued{};
static wchar_t g_finalDirectory[MAX_PATH]{};
static void FinalTraceRelease() {
    unsigned discarded=0;for(auto& s:g_finalTrace)discarded+=s.pending;
    if(discarded){g_finalDrop+=discarded;XrLog("[FINALPIXELS] resize/shutdown discarded %u pending captures; read=%u issued=%u",discarded,g_finalRead,g_finalIssued);}
    for(auto& s:g_finalTrace) {
        for(auto& eye:s.pixels)for(auto*& p:eye){if(p)p->Release();p=nullptr;}
        if(s.query)s.query->Release();s.query=nullptr;s.pending=s.left=false;
    }
    g_finalActive=g_finalSubmission=-1;
}
static bool FinalTraceDirectory() {
    if(g_finalDirectory[0])return true;
    EnsureSelfModule();wchar_t mod[MAX_PATH]{};GetModuleFileNameW(g_selfModule,mod,MAX_PATH);
    wchar_t* slash=wcsrchr(mod,L'\\');if(!slash)return false;*slash=0;
    SYSTEMTIME t{};GetLocalTime(&t);
    swprintf_s(g_finalDirectory,L"%s\\p5r_finaltrace_%04u%02u%02u_%02u%02u%02u",mod,t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    if(!CreateDirectoryW(g_finalDirectory,nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){g_finalDirectory[0]=0;return false;}
    XrLog("[FINALPIXELS] ACTIVE: 96 bounded world pairs; final source and XR destination crops, exact submit metadata; no GPU waits/flushes; directory=%ls",g_finalDirectory);
    return true;
}
static bool FinalTraceBuild(FinalTraceSlot& s,unsigned w,unsigned h) {
    if(s.query&&s.w==w&&s.h==h)return true;
    if(s.query)s.query->Release();s.query=nullptr;
    for(auto& eye:s.pixels)for(auto*& p:eye){if(p)p->Release();p=nullptr;}
    D3D11_TEXTURE2D_DESC d{};d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_STAGING;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    for(auto& eye:s.pixels)for(auto*& p:eye)if(FAILED(g_gameDevice->CreateTexture2D(&d,nullptr,&p)))return false;
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT,0};
    if(FAILED(g_gameDevice->CreateQuery(&q,&s.query)))return false;
    s.w=w;s.h=h;return true;
}
static int FinalTraceRead(FinalTraceSlot& s) {
    LARGE_INTEGER began{},finished{},frequency{};QueryPerformanceCounter(&began);
    D3D11_MAPPED_SUBRESOURCE mapped[2][2]{};bool ready=true;
    for(int e=0;e<2;++e)for(int k=0;k<2;++k)
        if(FAILED(g_gameCtx->Map(s.pixels[e][k],0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped[e][k])))ready=false;
    if(!ready) {
        for(int e=0;e<2;++e)for(int k=0;k<2;++k)if(mapped[e][k].pData)g_gameCtx->Unmap(s.pixels[e][k],0);
        return 0;
    }
    bool textured=true;
    for(int eye=0;eye<2;++eye) {
        double sum=0,square=0;unsigned count=0;
        for(unsigned y=0;y<s.h;y+=8) {
            const auto* p=(const unsigned char*)mapped[eye][0].pData+y*mapped[eye][0].RowPitch;
            for(unsigned x=0;x<s.w;x+=8) {const double value=(p[x*4]+p[x*4+1]+p[x*4+2])/3.;sum+=value;square+=value*value;++count;}
        }
        const double variance=count?square/count-(sum/count)*(sum/count):0;
        if(variance<16)textured=false;
    }
    if(!textured||!s.layerSubmitted||!XrSucceeded(s.endResult)) {
        for(int eye=0;eye<2;++eye)for(int k=0;k<2;++k)g_gameCtx->Unmap(s.pixels[eye][k],0);
        ++g_finalRejected;
        if(!g_finalRead)g_finalStarted=false; // loading must not start the live capture window
        if(g_finalRejected<4||g_finalRejected%120==0)XrLog("[FINALPIXELS] skipped nontextured/unsuccessful submission serial=%u; usable=%u budget remains",s.pose.serial,g_finalRead);
        return 2;
    }
    unsigned mismatch[2]{},maxDifference[2]{};bool saved=true;
    for(int e=0;e<2;++e) {
        wchar_t path[MAX_PATH]{};swprintf_s(path,L"%s\\%03u_%c.ppm",g_finalDirectory,s.id,e?'R':'L');
        FILE* file=_wfopen(path,L"wb");if(!file)saved=false;
        if(file)fprintf(file,"P6\n%u %u\n255\n",s.w,s.h);
        std::vector<unsigned char> row(s.w*3);
        for(unsigned y=0;y<s.h;++y) {
            const auto* a=(const unsigned char*)mapped[e][0].pData+y*mapped[e][0].RowPitch;
            const auto* b=(const unsigned char*)mapped[e][1].pData+y*mapped[e][1].RowPitch;
            for(unsigned x=0;x<s.w;++x) {
                bool different=false;
                for(unsigned c=0;c<3;++c) {
                    row[x*3+c]=a[x*4+c];
                    const unsigned delta=unsigned(abs(int(a[x*4+c])-int(b[x*4+c])));
                    if(delta){different=true;if(delta>maxDifference[e])maxDifference[e]=delta;}
                }
                mismatch[e]+=different;
            }
            if(file&&fwrite(row.data(),1,row.size(),file)!=row.size())saved=false;
        }
        if(file)fclose(file);
    }
    wchar_t path[MAX_PATH]{};swprintf_s(path,L"%s\\frames.csv",g_finalDirectory);
    FILE* csv=_wfopen(path,L"a");if(!csv)saved=false;
    if(csv) {
        // No header: id,serial,time,epoch,QPC,fullW/H,cropX/Y/W/H,L/R index,
        // L/R mismatched pixels,L/R max byte delta,quaternion,position,
        // two FOVs (left/right/up/down),two row-major rendered views.
        fprintf(csv,"%u,%u,%lld,%u,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",s.id,s.pose.serial,s.pose.time,s.pose.spaceEpoch,s.qpc,s.fullW,s.fullH,s.x,s.y,s.w,s.h,s.index[0],s.index[1],mismatch[0],mismatch[1],maxDifference[0],maxDifference[1]);
        for(float v:s.pose.orientation)fprintf(csv,",%.9g",v);
        for(float v:s.pose.position)fprintf(csv,",%.9g",v);
        for(auto& f:s.pose.renderedFov)fprintf(csv,",%.9g,%.9g,%.9g,%.9g",f.angleLeft,f.angleRight,f.angleUp,f.angleDown);
        for(auto& v:s.pose.renderedView)for(float x:v)fprintf(csv,",%.9g",x);
        fprintf(csv,",%d,%d,%lld",s.layerSubmitted,(int)s.endResult,s.displayTime);
        fprintf(csv,"\n");fclose(csv);
    }
    for(int e=0;e<2;++e)for(int k=0;k<2;++k)g_gameCtx->Unmap(s.pixels[e][k],0);
    memcpy(s.mismatched,mismatch,sizeof(mismatch));memcpy(s.maxByte,maxDifference,sizeof(maxDifference));
    QueryPerformanceCounter(&finished);QueryPerformanceFrequency(&frequency);
    XrLog("[FINALPIXELS] pair=%u serial=%u src->XR RGB mismatch L=%u R=%u max-byte L=%u R=%u indices=%u/%u saved=%d read/save CPU=%.2fms",s.id,s.pose.serial,mismatch[0],mismatch[1],maxDifference[0],maxDifference[1],s.index[0],s.index[1],saved,1000.*(finished.QuadPart-began.QuadPart)/frequency.QuadPart);
    return 1;
}
static void FinalTracePoll() {
    if(!g_gameCtx)return;
    for(auto& s:g_finalTrace)if(s.pending) {
        BOOL done=FALSE;
        if(g_gameCtx->GetData(s.query,&done,sizeof(done),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&done) {
            const int result=FinalTraceRead(s);if(!result)continue;
            s.pending=false;if(result==1)++g_finalRead;
            if(g_finalRead==96)XrLog("[FINALPIXELS] COMPLETE: read=%u issued=%u dropped=%u; copied final pixels ready for image/pose analysis",g_finalRead,g_finalIssued,g_finalDrop);
        }
    }
}
static void FinalTraceBeforeCopy(int eye,ID3D11Texture2D* src,const P5RXr::Pose* pose) {
    if(eye==0)g_finalActive=-1;
    if(eye>1||!pose||!pose->renderedViewValid||!pose->renderedFovValid||g_finalRead>=96)return;
    if(eye==0) {
        FinalTracePoll();g_finalActive=-1;
        unsigned pending=0;for(auto& s:g_finalTrace)pending+=s.pending;
        if(g_finalRead+pending>=96)return;
        if(g_finalLastQueued.spaceEpoch==pose->spaceEpoch&&pose->serial<=g_finalLastQueued.serial)return;
        double dot=0,aa=0,bb=0;for(int i=0;i<4;++i){dot+=(double)pose->orientation[i]*g_finalLast.orientation[i];aa+=(double)pose->orientation[i]*pose->orientation[i];bb+=(double)g_finalLast.orientation[i]*g_finalLast.orientation[i];}
        const double cosine=aa*bb>1e-12?fmin(1.,fabs(dot)/sqrt(aa*bb)):1.;
        const double delta=2*acos(cosine)*57.29578;g_finalLast=*pose;
        if(!g_finalStarted) {
            if(pose->serial<120||delta<.08)return;
            if(!FinalTraceDirectory())return;g_finalStarted=true;
        }
        D3D11_TEXTURE2D_DESC d{};src->GetDesc(&d);
        if(d.SampleDesc.Count!=1||d.ArraySize!=1||(d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM&&d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))return;
        const unsigned w=d.Width<640?d.Width:640,h=d.Height<480?d.Height:480;
        for(int i=0;i<4;++i)if(!g_finalTrace[i].pending) {g_finalActive=i;break;}
        if(g_finalActive<0){++g_finalDrop;return;}
        auto& s=g_finalTrace[g_finalActive];
        if(!FinalTraceBuild(s,w,h)){g_finalActive=-1;return;}
        s.pose=*pose;s.fullW=d.Width;s.fullH=d.Height;s.x=(d.Width-w)/2;s.y=(d.Height-h)/2;s.left=false;
        LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);s.qpc=qpc.QuadPart;
    }
    if(g_finalActive<0)return;
    auto& s=g_finalTrace[g_finalActive];
    if(s.pose.serial!=pose->serial||s.pose.spaceEpoch!=pose->spaceEpoch){g_finalActive=-1;return;}
    D3D11_TEXTURE2D_DESC d{};src->GetDesc(&d);if(d.Width!=s.fullW||d.Height!=s.fullH){g_finalActive=-1;return;}
    D3D11_BOX box{s.x,s.y,0,s.x+s.w,s.y+s.h,1};
    g_gameCtx->CopySubresourceRegion(s.pixels[eye][0],0,0,0,0,src,0,&box);
    if(eye==0)g_finalLastQueued=*pose;
}
static void FinalTraceAfterCopy(int eye,ID3D11Texture2D* dst,uint32_t index,bool copied) {
    if(eye>1||g_finalActive<0)return;
    auto& s=g_finalTrace[g_finalActive];
    if(!copied){g_finalActive=-1;++g_finalDrop;return;}
    D3D11_BOX box{s.x,s.y,0,s.x+s.w,s.y+s.h,1};
    g_gameCtx->CopySubresourceRegion(s.pixels[eye][1],0,0,0,0,dst,0,&box);s.index[eye]=index;
    if(eye==0)s.left=true;
    else if(s.left) {s.id=++g_finalIssued;s.layerSubmitted=false;s.endResult=static_cast<XrResult>(-999);g_gameCtx->End(s.query);s.pending=true;g_finalSubmission=g_finalActive;g_finalActive=-1;}
}
static void FinalTraceEndSubmission(bool layer,XrResult result,XrTime displayTime) {
    if(g_finalSubmission<0)return;
    auto& s=g_finalTrace[g_finalSubmission];s.layerSubmitted=layer;s.endResult=result;s.displayTime=displayTime;g_finalSubmission=-1;
}
