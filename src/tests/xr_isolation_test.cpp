// No headset or OpenXR runtime: exercise the real wrappers with mocked XR calls.
#include "../p5r_xr.cpp"
#include <cassert>
#include <thread>

static bool fault = false;
static bool waitGood = false;
static unsigned waits = 0, begins = 0, ends = 0;
static XrTime endedTime = 0;
static XrResult Poll(XrInstance, XrEventDataBuffer*) {
    assert(P5RXr::IsInternalCall());
    return XR_EVENT_UNAVAILABLE_VALUE;
}
static XrResult Wait(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    assert(P5RXr::IsInternalCall());
    assert(strcmp(P5RXr::ActiveCall(), "xrWaitFrame") == 0);
    // A runtime callback can enter another wrapper without clearing the outer exclusion.
    assert(!P5RXr::Init(nullptr));
    assert(P5RXr::IsInternalCall());
    std::thread other([] { assert(!P5RXr::IsInternalCall()); });
    other.join();
    if (fault) RaiseException(0xE0420001, 0, 0, nullptr);
    if (waitGood) {
        ++waits;
        state->predictedDisplayTime = 1000000000LL + waits * 10000000LL;
        state->predictedDisplayPeriod = 10000000LL;
        state->shouldRender = 1;
        return XR_SUCCESS_VALUE;
    }
    return static_cast<XrResult>(-1);
}
static XrResult Begin(XrSession, const XrFrameBeginInfo*) {
    assert(P5RXr::IsInternalCall()); ++begins; return XR_SUCCESS_VALUE;
}
static XrResult Locate(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                      uint32_t capacity, uint32_t* count, XrView* views) {
    assert(capacity == 2 && info->displayTime == g_frameState.predictedDisplayTime);
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE;
    *count = 2;
    for (int i = 0; i < 2; ++i) {
        views[i].pose = IdentityPose();
        views[i].pose.orientation.y = sinf(waits * .05f);
        views[i].pose.orientation.w = cosf(waits * .05f);
        views[i].fov = {-.9f, .9f, .9f, -.9f};
    }
    return XR_SUCCESS_VALUE;
}
static XrResult End(XrSession, const XrFrameEndInfo* info) {
    assert(P5RXr::IsInternalCall());
    assert(strcmp(P5RXr::ActiveCall(), "xrEndFrame") == 0);
    assert(info->layerCount == 0);
    ++ends; endedTime = info->displayTime;
    return XR_SUCCESS_VALUE;
}
static XrResult AuditAcquire(XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index){*index=0;return XR_SUCCESS_VALUE;}
static XrResult AuditWait(XrSwapchain,const XrSwapchainImageWaitInfo*){return XR_SUCCESS_VALUE;}
static XrResult AuditRelease(XrSwapchain,const XrSwapchainImageReleaseInfo*){return XR_SUCCESS_VALUE;}
static void STDMETHODCALLTYPE SkipCopy(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*){}
static P5RXr::Pose ImageSource(unsigned serial,unsigned epoch=3) {
    P5RXr::Pose p{};p.serial=serial;p.spaceEpoch=epoch;p.time=1000000000LL+serial*10000000LL;
    p.orientation[1]=sinf(serial*.01f);p.orientation[3]=cosf(serial*.01f);p.position[0]=serial*.001f;
    p.renderedViewValid=p.renderedFovValid=true;
    for(int eye=0;eye<2;++eye){p.renderedFov[eye]={-.9f,.9f,.8f,-.8f};p.renderedView[eye][0]=float(serial);}
    return p;
}
static void TestImagePoseAssociation() {
    FinalImagePoseAssociation history;bool delayed;
    auto p=history.Apply(ImageSource(41),delayed);assert(p.serial==41&&!delayed);
    p=history.Apply(ImageSource(42),delayed);assert(p.serial==41&&delayed);
    assert(p.time==ImageSource(41).time&&p.orientation[1]==ImageSource(41).orientation[1]);
    assert(p.position[0]==ImageSource(41).position[0]&&p.renderedView[1][0]==41&&p.renderedFovValid);
    for(int i=0;i<1000;++i){p=history.Apply(ImageSource(42),delayed);assert(p.serial==41&&delayed);}
    p=history.Apply(ImageSource(43),delayed);assert(p.serial==42&&delayed); // raw previous, never recursive delay
    p=history.Apply(ImageSource(41),delayed);assert(p.serial==41&&!delayed); // cached older source
    p=history.Apply(ImageSource(46),delayed);assert(p.serial==46&&!delayed); // missed renders re-prime
    p=history.Apply(ImageSource(47),delayed);assert(p.serial==46&&delayed);
    p=history.Apply(ImageSource(48,4),delayed);assert(p.serial==48&&!delayed); // recenter boundary
    p=history.Apply(ImageSource(49,4),delayed);assert(p.serial==48&&delayed);
    auto invalid=ImageSource(50,4);invalid.renderedViewValid=false;
    p=history.Apply(invalid,delayed);assert(p.serial==50&&!delayed);
    p=history.Apply(ImageSource(51,4),delayed);assert(p.serial==51&&!delayed);
    auto paused=ImageSource(52,4);paused.time+=1000000000LL;
    p=history.Apply(paused,delayed);assert(p.serial==52&&!delayed);
    puts("PASS: final-image pose offset retains raw history, full camera metadata, frozen identity, and resets on gaps/recenter/untracked/pause");
}
static P5RXr::Pose expectedImage;
static XrResult EndImage(XrSession,const XrFrameEndInfo* info) {
    assert(info->layerCount==1);
    auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
    assert(projection->viewCount==2);
    for(int eye=0;eye<2;++eye){const auto& v=projection->views[eye];
        assert(v.pose.orientation.y==expectedImage.orientation[1]);
        assert(v.pose.position.x==expectedImage.position[0]);
        assert(v.fov.angleUp==expectedImage.renderedFov[eye].angleUp);
    }
    return XR_SUCCESS_VALUE;
}
static void TestFinalPixels() {
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&g_gameDevice,&level,&g_gameCtx)));
    g_context=g_gameCtx;
    D3D11_TEXTURE2D_DESC d{};d.Width=64;d.Height=48;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    std::vector<uint32_t> a(64*48,0xFF604020),b(64*48,0xFF103080);
    for(unsigned y=0;y<48;++y)for(unsigned x=0;x<64;++x)if((x/16+y/16)%2){a[y*64+x]=0xFFCCAA88;b[y*64+x]=0xFF8855DD;}
    D3D11_SUBRESOURCE_DATA initial{a.data(),64*4,0};
    ID3D11Texture2D *source=nullptr,*destination[2]{};
    assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,&initial,&source)));
    initial.pSysMem=b.data();
    for(int eye=0;eye<2;++eye){assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,&initial,&destination[eye])));g_eyes[eye].swapchain=reinterpret_cast<XrSwapchain>(static_cast<uintptr_t>(eye+1));g_eyes[eye].imageCount=1;g_eyes[eye].images[0]=destination[eye];}
    g_fn.acquireSwapchainImage=AuditAcquire;g_fn.waitSwapchainImage=AuditWait;g_fn.releaseSwapchainImage=AuditRelease;
    wchar_t directory[MAX_PATH];GetTempPathW(MAX_PATH,directory);swprintf_s(g_finalDirectory,L"%sP5R-final-pixel-test-%lu",directory,GetCurrentProcessId());CreateDirectoryW(g_finalDirectory,nullptr);
    g_finalStarted=true;g_finalIssued=g_finalRead=0;
    P5RXr::Pose pose{};pose.orientation[3]=1;pose.serial=125;pose.spaceEpoch=3;pose.renderedViewValid=pose.renderedFovValid=true;
    for(int eye=0;eye<2;++eye){pose.renderedFov[eye]={-1,1,1,-1};for(int i=0;i<4;++i)pose.renderedView[eye][i*4+i]=1;}
    assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));
    FinalTraceEndSubmission(true,XR_SUCCESS_VALUE,12345);
    assert(g_finalIssued==1&&g_finalTrace[0].pending&&g_finalTrace[0].pose.serial==125);
    assert(g_finalTrace[0].layerSubmitted&&g_finalTrace[0].displayTime==12345);
    // Rewrite the source AFTER the copy: the snapshot must keep the earlier pixels.
    g_gameCtx->UpdateSubresource(source,0,nullptr,b.data(),64*4,0);g_gameCtx->Flush();
    for(int i=0;i<1000&&!g_finalRead;++i){FinalTracePoll();if(!g_finalRead)Sleep(1);}
    assert(g_finalRead==1&&g_finalTrace[0].mismatched[0]==0&&g_finalTrace[0].mismatched[1]==0);
    // Deliberately skip the real GPU copy: source and old XR image must disagree.
    g_gameRawCopy=SkipCopy;pose.serial++;
    assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));FinalTraceEndSubmission(true,XR_SUCCESS_VALUE,12346);g_gameCtx->Flush();
    for(int i=0;i<1000&&g_finalRead<2;++i){FinalTracePoll();if(g_finalRead<2)Sleep(1);}
    assert(g_finalRead==2&&g_finalTrace[0].mismatched[0]==64*48&&g_finalTrace[0].mismatched[1]==64*48);
    assert(g_finalTrace[0].maxByte[0]>0);g_gameRawCopy=nullptr;
    const unsigned issuedBeforeFreeze=g_finalIssued;
    assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));
    assert(g_finalIssued==issuedBeforeFreeze); // held pose must not consume capture budget
    std::vector<uint32_t> black(64*48,0xFF000000);g_gameCtx->UpdateSubresource(source,0,nullptr,black.data(),64*4,0);++pose.serial;
    assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));FinalTraceEndSubmission(true,XR_SUCCESS_VALUE,12347);g_gameCtx->Flush();
    for(int i=0;i<1000&&!g_finalRejected;++i){FinalTracePoll();if(!g_finalRejected)Sleep(1);}
    assert(g_finalRejected==1&&g_finalRead==2); // black loading cannot spend the 96 useful pairs
    g_finalIssued=1000;g_finalRead=95;++pose.serial;
    g_gameCtx->UpdateSubresource(source,0,nullptr,a.data(),64*4,0);
    assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));FinalTraceEndSubmission(true,XR_SUCCESS_VALUE,12348);g_gameCtx->Flush();
    for(int i=0;i<1000&&g_finalRead<96;++i){FinalTracePoll();if(g_finalRead<96)Sleep(1);}
    assert(g_finalRead==96&&g_finalIssued==1001); // rejected attempts never exhaust the useful budget
    ++pose.serial;assert(CopyToEye(0,source,&pose)&&CopyToEye(1,source,&pose));assert(g_finalIssued==1001);
    // A size-incompatible image is rejected instead of being labelled with a new pose.
    d.Width=32;ID3D11Texture2D* wrong=nullptr;assert(SUCCEEDED(g_gameDevice->CreateTexture2D(&d,nullptr,&wrong)));
    assert(!CopyToEye(0,wrong,&pose));
    g_fn.endFrame=End;g_viewsValid=true;g_frameBegun=true;
    const unsigned beforeEnds=ends;P5RXr::SubmitEyes(wrong,source);
    assert(ends==beforeEnds+1&&!g_frameBegun);g_viewsValid=false;wrong->Release();
    // Exercise the actual stereo-copy -> projection-layer path with held raw tags.
    g_finalImagePose.Reset();g_viewsValid=true;g_fn.endFrame=EndImage;
    for(int eye=0;eye<2;++eye){g_eyes[eye].width=64;g_eyes[eye].height=48;}
    for(unsigned serial: {41u,42u,42u,43u}){
        auto raw=ImageSource(serial);P5RXr::SetRenderPose(&raw);
        expectedImage=ImageSource(serial==41?41:serial-1);g_frameBegun=true;
        P5RXr::SubmitEyes(source,source);assert(!g_frameBegun);
        P5RXr::Pose retained{};assert(P5RXr::GetRenderPose(&retained)&&retained.serial==serial);
    }
    g_renderPoseValid=false;g_viewsValid=false;g_fn.endFrame=End;g_finalImagePose.Reset();
    FinalTraceRelease();for(int eye=0;eye<2;++eye){g_eyes[eye]={};g_eyeAcq[eye]={};destination[eye]->Release();}
    source->Release();g_gameCtx->Release();g_gameDevice->Release();g_context=g_gameCtx=nullptr;g_gameDevice=nullptr;
    puts("PASS: actual final pixels at XR copy retain pose/index, detect stale pixels, reject bad sizes, skip held poses/black loading, and stop only after 96 useful captures");
}
#include "flash_frames_test.inl"
int main() {
    TestImagePoseAssociation();
    TestFinalPixels();
    TestFlashFrames();
    XrPosef left=IdentityPose(),right=IdentityPose();
    left.position={.968f,1.6f,2};right.position={1.032f,1.6f,2};
    auto center=CenterEyePose(left,right);
    assert(fabsf(center.position.x-1)<1e-6f&&center.position.y==1.6f&&center.position.z==2);
    left.orientation={0,.70710678f,0,.70710678f};
    right.orientation={0,-.70710678f,0,-.70710678f};
    center=CenterEyePose(left,right);
    assert(fabsf(center.orientation.y-.70710678f)<1e-6f);
    auto yaw=YawOnlyQuat(center.orientation);assert(fabsf(yaw.y-.70710678f)<1e-6f);
    puts("PASS: recenter uses eye midpoint and handles equivalent quaternion signs");
    assert(!P5RXr::IsInternalCall());
    assert(!P5RXr::Init(nullptr));
    assert(!P5RXr::IsInternalCall());
    g_ready.store(true);
    g_sessionRunning = true;
    g_session = reinterpret_cast<XrSession>(1);
    g_fn.pollEvent = Poll;
    g_fn.waitFrame = Wait;
    g_fn.endFrame = End;
    assert(!P5RXr::BeginFrame());
    assert(!P5RXr::IsInternalCall());
    assert(strcmp(P5RXr::ActiveCall(), "idle") == 0);
    fault = true;
    assert(!P5RXr::BeginFrame());
    assert(!P5RXr::IsInternalCall());
    assert(strcmp(P5RXr::ActiveCall(), "xrWaitFrame") == 0); // fault breadcrumb retained
    g_frameBegun = true;
    P5RXr::SubmitEyes(nullptr, nullptr);
    assert(!g_frameBegun);
    assert(!P5RXr::IsInternalCall());
    puts("PASS: XR exclusion, nesting, thread isolation, SEH cleanup, call breadcrumbs, empty submission");
    // Rendering reads a prediction prepared BEFORE upload. Present consumes it;
    // asking again at Present must not locate a newer pose or discard the frame.
    fault = false; waitGood = true; waits = begins = ends = 0;
    g_fn.beginFrame = Begin; g_fn.locateViews = Locate;
    assert(P5RXr::PrepareRenderFrame() && P5RXr::HasRenderFrame());
    P5RXr::Pose rendered{}; assert(P5RXr::GetHeadPose(&rendered, nullptr));
    assert(rendered.time == 1010000000LL && waits == 1 && begins == 1);
    P5RXr::SetRenderPose(&rendered);
    assert(P5RXr::PrepareRenderFrame()); // ready-to-submit; no second wait/locate
    P5RXr::Pose atSubmit{}; assert(P5RXr::GetHeadPose(&atSubmit, nullptr));
    assert(waits == 1 && atSubmit.serial == rendered.serial);
    P5RXr::SubmitEyes(nullptr, nullptr);
    assert(!P5RXr::HasRenderFrame() && ends == 1 && endedTime == rendered.time);
    assert(P5RXr::PrepareRenderFrame()); // next game render
    P5RXr::Pose next{}; assert(P5RXr::GetHeadPose(&next, nullptr));
    assert(next.time == 1020000000LL && next.serial == rendered.serial + 1);
    assert(next.orientation[1] != rendered.orientation[1]);
    // Resize cancels a prepared frame before releasing runtime textures.
    P5RXr::ReleaseDeviceResources();
    assert(!P5RXr::HasRenderFrame() && ends == 2 && endedTime == next.time);
    assert(P5RXr::PrepareRenderFrame());
    P5RXr::SubmitScreen(nullptr, 2.f, 2.f, false);
    assert(!P5RXr::HasRenderFrame() && ends == 3);
    puts("PASS: render-before-submit prediction ownership, idempotent preparation, paired frame lifecycle for world/screen/resize");
}
