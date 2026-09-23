// Gate suite: the tracking-state verdict walk.

#include "core/tracking_verdict.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using NMSHT::EvaluateTracking;
using NMSHT::TrackingInputs;
using NMSHT::TrackingVerdict;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

void CheckReason(const char* actual, const char* expected, const char* what) {
    const bool ok = (expected == nullptr)
                        ? (actual == nullptr)
                        : (actual != nullptr && std::strcmp(actual, expected) == 0);
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s (expected \"%s\", got \"%s\")\n", what,
                expected ? expected : "<none>", actual ? actual : "<none>");
}

// In gameplay, nothing up.
TrackingInputs Playing() {
    TrackingInputs in;
    in.fsmLocated = true;
    in.stateRecognised = true;
    in.gameplayState = true;
    in.stateName = "cGcApplicationSimulationState";
    return in;
}

void GameplayTracksWithNoReason() {
    const TrackingVerdict v = EvaluateTracking(Playing());
    CheckReason(v.reason, nullptr, "plain gameplay reports no reason");
    Check(v.poseApplies, "plain gameplay applies the pose");
}

void AnUnlocatedFsmStandsDown() {
    TrackingInputs in = Playing();
    in.fsmLocated = false;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "the application state machine has not been located",
                "an unlocated FSM reports its own reason");
    Check(!v.poseApplies, "an unlocated FSM takes the pose away");
}

void AnUnknownStateStandsDown() {
    TrackingInputs in = Playing();
    in.stateRecognised = false;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "the application is in a state this mod does not recognise",
                "an unrecognised state reports its own reason");
    Check(!v.poseApplies, "an unrecognised state takes the pose away");
}

void ANonGameplayStateReportsItsClassName() {
    TrackingInputs in = Playing();
    in.gameplayState = false;
    in.stateName = "cGcApplicationLocalLoadState";
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "cGcApplicationLocalLoadState",
                "a loading screen reports the engine's own state name");
    Check(!v.poseApplies, "a loading screen takes the pose away");
}

void AMenuStandsDown() {
    TrackingInputs in = Playing();
    in.menuUp = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "an in-game menu is open", "a menu reports its own reason");
    Check(!v.poseApplies, "a menu takes the pose away");
}

void MultiplayerStandsDown() {
    TrackingInputs in = Playing();
    in.multiplayer = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "another player is in the session",
                "another explorer reports its own reason");
    Check(!v.poseApplies, "another explorer takes the pose away");
}

}  // namespace

int main() {
    GameplayTracksWithNoReason();
    AnUnlocatedFsmStandsDown();
    AnUnknownStateStandsDown();
    ANonGameplayStateReportsItsClassName();
    AMenuStandsDown();
    MultiplayerStandsDown();

    if (g_failures != 0) {
        std::printf("state_gate_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("state_gate_test: all cases passed\n");
    return EXIT_SUCCESS;
}
