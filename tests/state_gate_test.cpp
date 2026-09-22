// Gate suite: the tracking-state verdict walk, including where ADS sits in it.

#include "core/tracking_verdict.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using cameraunlock::ads::AdsMode;
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

// In gameplay, nothing up, sights down.
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
    Check(!v.aiming, "plain gameplay is not aiming");
}

void AnUnlocatedFsmStandsDown() {
    TrackingInputs in = Playing();
    in.fsmLocated = false;
    in.aiming = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "the application state machine has not been located",
                "an unlocated FSM reports its own reason");
    Check(!v.poseApplies, "an unlocated FSM takes the pose away");
    Check(!v.aiming, "an early return leaves no stale ADS flag");
}

void AnUnknownStateStandsDown() {
    TrackingInputs in = Playing();
    in.stateRecognised = false;
    in.aiming = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "the application is in a state this mod does not recognise",
                "an unrecognised state reports its own reason");
    Check(!v.poseApplies, "an unrecognised state takes the pose away");
    Check(!v.aiming, "an early return leaves no stale ADS flag");
}

void ANonGameplayStateReportsItsClassName() {
    TrackingInputs in = Playing();
    in.gameplayState = false;
    in.stateName = "cGcApplicationLocalLoadState";
    in.aiming = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "cGcApplicationLocalLoadState",
                "a loading screen reports the engine's own state name");
    Check(!v.poseApplies, "a loading screen takes the pose away");
    Check(!v.aiming, "an early return leaves no stale ADS flag");
}

// A menu outranks ADS: both can be true at once, and the reason a player needs
// is the menu. It also clears the ADS flag, so nothing render-side keeps running
// ADS behaviour behind a full-screen page.
void AMenuOutranksAds() {
    TrackingInputs in = Playing();
    in.menuUp = true;
    in.aiming = true;
    in.adsMode = AdsMode::Paused;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "an in-game menu is open", "a menu outranks ADS in the reason");
    Check(!v.poseApplies, "a menu takes the pose away");
    Check(!v.aiming, "a menu clears the ADS flag");
}

void MultiplayerOutranksAds() {
    TrackingInputs in = Playing();
    in.multiplayer = true;
    in.aiming = true;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "another player is in the session",
                "another explorer outranks ADS in the reason");
    Check(!v.poseApplies, "another explorer takes the pose away");
    Check(!v.aiming, "another explorer clears the ADS flag");
}

// `paused` names the ADS reason and still reports the sights up. It does NOT
// take the pose away: this mod keeps writing the camera through the aim and
// lets the fade run the pose down, because a gate that shut on the first aiming
// frame would cut the pose in one frame.
void PausedNamesAdsAndKeepsFeedingTheFade() {
    TrackingInputs in = Playing();
    in.aiming = true;
    in.adsMode = AdsMode::Paused;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, "aiming down sights", "paused reports the ADS reason");
    Check(v.aiming, "paused still reports the sights up");
    Check(v.poseApplies, "paused keeps feeding the camera so the fade can run down");
}

void TrackedModesKeepTheGateOpen() {
    for (const AdsMode mode : { AdsMode::Marker, AdsMode::Tracked }) {
        TrackingInputs in = Playing();
        in.aiming = true;
        in.adsMode = mode;
        const TrackingVerdict v = EvaluateTracking(in);
        CheckReason(v.reason, nullptr, "a tracked mode reports no reason while aiming");
        Check(v.aiming, "a tracked mode reports the sights up");
        Check(v.poseApplies, "a tracked mode keeps the pose");
    }
}

// The state is polled, not latched, so it heals with no exit edge: one frame
// says aiming, the next says not, and the verdict follows without anything
// having to observe a transition.
void TheAdsStateHealsWithoutAnExitEdge() {
    TrackingInputs in = Playing();
    in.aiming = true;
    in.adsMode = AdsMode::Paused;
    CheckReason(EvaluateTracking(in).reason, "aiming down sights", "sights up");

    in.aiming = false;
    const TrackingVerdict v = EvaluateTracking(in);
    CheckReason(v.reason, nullptr, "the verdict heals with no exit event");
    Check(!v.aiming, "the flag heals with no exit event");
    Check(v.poseApplies, "the pose is back with no exit event");
}

}  // namespace

int main() {
    GameplayTracksWithNoReason();
    AnUnlocatedFsmStandsDown();
    AnUnknownStateStandsDown();
    ANonGameplayStateReportsItsClassName();
    AMenuOutranksAds();
    MultiplayerOutranksAds();
    PausedNamesAdsAndKeepsFeedingTheFade();
    TrackedModesKeepTheGateOpen();
    TheAdsStateHealsWithoutAnExitEdge();

    if (g_failures != 0) {
        std::printf("state_gate_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("state_gate_test: all cases passed\n");
    return EXIT_SUCCESS;
}
