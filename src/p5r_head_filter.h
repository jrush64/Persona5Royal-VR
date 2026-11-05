#pragma once
// One orientation per locate serial. Render and submit consume the SAME result;
// never smooth Euler camera angles separately from the declared XR quaternion.
struct HeadOrientationFilter {
    bool initialized=false; P5RXr::Pose filtered{};
    P5RXr::Pose Apply(const P5RXr::Pose& raw) {
        if(initialized&&raw.serial==filtered.serial&&raw.spaceEpoch==filtered.spaceEpoch)return filtered;
        const bool reset=!initialized||raw.spaceEpoch!=filtered.spaceEpoch||raw.time<filtered.time||raw.time-filtered.time>250000000LL;
        float dot=0;for(int i=0;i<4;++i)dot+=filtered.orientation[i]*raw.orientation[i];
        float absolute=fabsf(dot);if(absolute>1)absolute=1;
        float angle=2*acosf(absolute)*57.29578f;
        // Existing ME/Outlast strength=.4: base follow=.62, rising to 1 by 5deg.
        float ramp=fminf(1.f,angle/5.f);float follow=.62f+.38f*ramp;
        P5RXr::Pose next=raw;
        if(!reset)for(int i=0;i<4;++i)next.orientation[i]=filtered.orientation[i]*(1-follow)+raw.orientation[i]*(dot<0?-follow:follow);
        float length=0;for(float x:next.orientation)length+=x*x;
        length=sqrtf(length);if(!std::isfinite(length)||length<1e-6f){initialized=false;return raw;}
        for(float& x:next.orientation)x/=length;
        filtered=next;initialized=true;return filtered;
    }
};
