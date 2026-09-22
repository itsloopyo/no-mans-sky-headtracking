#pragma once

#include <cameraunlock/ads/ads_blend.h>
#include <cameraunlock/ads/ads_fade.h>
#include <cameraunlock/ads/ads_mode.h>
#include <cameraunlock/ads/entry_pose.h>

namespace NMSHT {

// One frame of the aim-down-sights pose pipeline: the fade that shapes the
// transition, the entry-relative pose the tracked modes ride, and the blend
// that picks between them for the current mode.
//
// No clock, no game, no logging - nowMs comes from the caller. That is what
// lets an entire aim be driven frame by frame in a test, which is the only way
// the two things that keep regressing here get covered: the entry pose being
// captured off a stale rotation, and yaw taking the long way round the seam.
struct AdsPipeline {
    using Pose = cameraunlock::ads::AdsEntryPose::Pose;

    // `aiming` is the game's own sights-up state for this frame, polled rather
    // than latched and never derived from the tracking verdict. `live` says the
    // rotation is a real sample rather than the nothing a suppressed frame
    // publishes.
    Pose Frame(cameraunlock::ads::AdsMode mode, bool aiming, bool live,
               unsigned long long nowMs, const Pose& absolute) {
        const float scale = fade.Update(aiming, nowMs);
        const Pose relative = entry.Relative(aiming, live, absolute);
        return cameraunlock::ads::BlendAdsPose(mode, scale, absolute, relative);
    }

    // Called on every frame the head pose is suppressed for a reason other than
    // the sights being up, so the next aim re-enters against a fresh entry pose.
    void Reset() {
        fade.Reset();
        entry.Reset();
    }

    cameraunlock::ads::AdsFade fade;
    cameraunlock::ads::AdsEntryPose entry;
};

}  // namespace NMSHT
