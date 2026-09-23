#pragma once

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
};

struct TrackingVerdict {
    // Why the head pose is standing down, or nullptr when nothing is in its way.
    const char* reason = nullptr;
    // The head pose still reaches the camera this frame.
    bool poseApplies = false;
};

// Aiming down sights is deliberately absent: head tracking carries straight on
// through the aim, so the sights being up is never a reason to stand down.
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

    v.poseApplies = true;
    return v;
}

}  // namespace NMSHT
