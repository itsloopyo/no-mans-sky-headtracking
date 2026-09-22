#pragma once

#include <cameraunlock/ads/ads_mode.h>

namespace NMSHT {

// Everything the tracking-state walk reads, sampled once so the verdict is a
// pure function of one frame's view of the game and can be exercised without
// NMS running.
struct TrackingInputs {
    // The FSM slot holding the current cGcApplicationXxxState has been found.
    bool fsmLocated = false;
    // The object in that slot has a vtable this mod knows.
    bool stateRecognised = false;
    // That state is cGcApplicationSimulationState.
    bool gameplayState = false;
    // Class name of the state, used verbatim as the reason when it is not
    // gameplay.
    const char* stateName = nullptr;
    // cGcGenericSectionConditionInventoryOpen's answer.
    bool menuUp = false;
    // cGcGenericSectionConditionInMultiplayer's answer.
    bool multiplayer = false;
    // The game's own weapon-zoom state, polled fresh this frame.
    bool aiming = false;
    cameraunlock::ads::AdsMode adsMode = cameraunlock::ads::kDefaultAdsMode;
};

struct TrackingVerdict {
    // Why the head pose is standing down, or nullptr when nothing is in its way.
    const char* reason = nullptr;
    // The sights are up. Reported in every mode, including `paused` where the
    // pose is on its way to nothing: the reason says whether tracking applies,
    // this says whether the sights are up, and the frame code needs both.
    bool aiming = false;
    // The head pose still reaches the camera this frame.
    bool poseApplies = false;
};

// ADS is tested LAST, so a menu, a loading screen or another explorer in the
// session still reports its own reason when both are true at once, and every
// earlier return leaves `aiming` false rather than leaking a stale flag into
// the render-side code.
//
// The ADS branch names a reason without taking the pose away. This mod keeps
// writing the camera through the aim and lets AdsFade run the pose down to
// nothing over kLowerMs; a gate that stopped the writes on the first aiming
// frame would cut the pose in one frame, which is the jolt the fade exists to
// remove. Every other reason is a real suppression and takes the pose with it.
inline TrackingVerdict EvaluateTracking(const TrackingInputs& in) {
    TrackingVerdict v;
    if (!in.fsmLocated) {
        v.reason = "the application state machine has not been located";
        return v;
    }
    if (!in.stateRecognised) {
        v.reason = "the application is in a state this mod does not recognise";
        return v;
    }
    if (!in.gameplayState) {
        v.reason = in.stateName;
        return v;
    }
    if (in.menuUp) {
        v.reason = "an in-game menu is open";
        return v;
    }
    if (in.multiplayer) {
        v.reason = "another player is in the session";
        return v;
    }

    v.aiming = in.aiming;
    v.poseApplies = true;
    if (in.aiming && cameraunlock::ads::AdsSuspendsTracking(in.adsMode)) {
        v.reason = "aiming down sights";
    }
    return v;
}

}  // namespace NMSHT
