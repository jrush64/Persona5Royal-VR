#pragma once
#include "p5r_xr.h"
#include <array>

// Measured final-pixel lag is one render frame, despite a current scene-camera
// binding. Keep source-camera identity separate from the pose of its final image.
// Cache the association so held/frozen images retain the same tag indefinitely.
class FinalImagePoseAssociation {
    struct Entry { uint32_t serial=0,epoch=0; bool valid=false,delayed=false; P5RXr::Pose image{}; };
    std::array<Entry,64> entries_{};
    P5RXr::Pose previous_{};
    bool valid_=false;
    unsigned next_=0;
public:
    void Reset() {entries_={};previous_={};valid_=false;next_=0;}
    P5RXr::Pose Apply(const P5RXr::Pose& source,bool& delayed) {
        delayed=false;
        if(!source.renderedViewValid||!source.renderedFovValid){Reset();return source;}
        if(valid_&&source.spaceEpoch!=previous_.spaceEpoch)Reset();
        for(const auto& e:entries_)if(e.valid&&e.serial==source.serial&&e.epoch==source.spaceEpoch){delayed=e.delayed;return e.image;}
        // An unknown old image must not advance or shift the current render history.
        if(valid_&&source.serial<=previous_.serial)return source;
        delayed=valid_&&source.serial==previous_.serial+1&&source.time>previous_.time&&source.time-previous_.time<250000000;
        P5RXr::Pose image=delayed?previous_:source;
        entries_[next_]={source.serial,source.spaceEpoch,true,delayed,image};next_=(next_+1)%entries_.size();
        previous_=source;valid_=true;return image;
    }
};
