// Entry-pose suite: what the tracked ADS modes actually feed the camera.
//
// Every case here is a frame the player either sees their head in or does not,
// and none of it is reachable from a settings or a gate test. The seam and the
// capture timing were both wrong in the first cut of the reference mod.

#include "core/ads_pipeline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using cameraunlock::ads::AdsFade;
using cameraunlock::ads::AdsMode;
using NMSHT::AdsPipeline;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    if (std::fabs(actual - expected) <= tolerance) return;
    ++g_failures;
    std::printf("FAIL: %s (expected %.4f, got %.4f)\n", what, expected, actual);
}

AdsPipeline::Pose MakePose(float pitch, float yaw, float roll,
                           float x = 0.0f, float y = 0.0f, float z = 0.0f) {
    AdsPipeline::Pose p;
    p.pitch = pitch;
    p.yaw = yaw;
    p.roll = roll;
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

// Runs the pipeline to the far end of a transition, so a case that is about the
// settled pose is not reading a frame mid-fade.
AdsPipeline::Pose Settle(AdsPipeline& pipe, AdsMode mode, bool aiming, bool live,
                         unsigned long long& nowMs, const AdsPipeline::Pose& absolute) {
    AdsPipeline::Pose out;
    for (int i = 0; i < 8; ++i) {
        out = pipe.Frame(mode, aiming, live, nowMs, absolute);
        nowMs += AdsFade::kRaiseMs;
    }
    return out;
}

// Hip fire passes the absolute pose straight through, in every mode.
void HipFireIsUntouched() {
    for (const AdsMode mode : { AdsMode::Paused, AdsMode::Marker, AdsMode::Tracked }) {
        AdsPipeline pipe;
        unsigned long long now = 1000;
        const AdsPipeline::Pose in = MakePose(7.0f, -21.0f, 4.0f, 0.05f, -0.02f, 0.11f);
        const AdsPipeline::Pose out = pipe.Frame(mode, false, true, now, in);
        CheckNear(out.pitch, in.pitch, 1e-5f, "hip fire keeps pitch");
        CheckNear(out.yaw, in.yaw, 1e-5f, "hip fire keeps yaw");
        CheckNear(out.roll, in.roll, 1e-5f, "hip fire keeps roll");
        CheckNear(out.x, in.x, 1e-5f, "hip fire keeps x");
        CheckNear(out.y, in.y, 1e-5f, "hip fire keeps y");
        CheckNear(out.z, in.z, 1e-5f, "hip fire keeps z");
    }
}

// `paused` fades the pose to nothing and holds it there, and leaves roll alone
// the whole way: a head tilt moves no aim point, so levelling it on the way in
// and leaning it back on the way out is two horizon jolts for nothing.
void PausedFadesToNothingButKeepsRoll() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    const AdsPipeline::Pose in = MakePose(12.0f, 30.0f, 9.0f, 0.10f, 0.03f, -0.20f);

    const AdsPipeline::Pose first = pipe.Frame(AdsMode::Paused, true, true, now, in);
    Check(std::fabs(first.yaw) > 0.5f * std::fabs(in.yaw),
          "the first aiming frame is still most of the way to the head pose");

    const AdsPipeline::Pose settled = Settle(pipe, AdsMode::Paused, true, true, now, in);
    CheckNear(settled.pitch, 0.0f, 1e-4f, "paused settles pitch at nothing");
    CheckNear(settled.yaw, 0.0f, 1e-4f, "paused settles yaw at nothing");
    CheckNear(settled.x, 0.0f, 1e-4f, "paused settles x at nothing");
    CheckNear(settled.y, 0.0f, 1e-4f, "paused settles y at nothing");
    CheckNear(settled.z, 0.0f, 1e-4f, "paused settles z at nothing");
    CheckNear(settled.roll, in.roll, 1e-5f, "paused never touches roll");
}

// A tracked mode settles on the pose measured from the frame the sights came up
// on, so the swing onto the aim is the same one paused makes and head tracking
// carries on from there.
void TrackedIsRelativeToTheEntryFrame() {
    for (const AdsMode mode : { AdsMode::Marker, AdsMode::Tracked }) {
        AdsPipeline pipe;
        unsigned long long now = 1000;
        const AdsPipeline::Pose entry = MakePose(10.0f, 25.0f, 6.0f, 0.08f, 0.02f, -0.10f);
        pipe.Frame(mode, true, true, now, entry);
        now += AdsFade::kLowerMs;

        const AdsPipeline::Pose moved = MakePose(14.0f, 31.0f, 6.0f, 0.13f, 0.05f, -0.04f);
        const AdsPipeline::Pose out = Settle(pipe, mode, true, true, now, moved);
        CheckNear(out.pitch, 4.0f, 1e-3f, "tracked pitch is measured from the entry frame");
        CheckNear(out.yaw, 6.0f, 1e-3f, "tracked yaw is measured from the entry frame");
        CheckNear(out.x, 0.05f, 1e-4f, "tracked x is measured from the entry frame");
        CheckNear(out.y, 0.03f, 1e-4f, "tracked y is measured from the entry frame");
        CheckNear(out.z, 0.06f, 1e-4f, "tracked z is measured from the entry frame");
        CheckNear(out.roll, moved.roll, 1e-5f, "tracked roll stays absolute");
    }
}

// Roll is never made relative even when it is the axis that moved, so a tilt
// held through the aim keeps showing.
void RollIsNeverMadeRelative() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 0.0f, 15.0f));
    now += AdsFade::kLowerMs;
    const AdsPipeline::Pose out =
        Settle(pipe, AdsMode::Tracked, true, true, now, MakePose(0.0f, 0.0f, 15.0f));
    CheckNear(out.roll, 15.0f, 1e-5f, "a tilt held across the entry is not levelled");
}

// Yaw arrives wrapped into -180..180, so a move across the seam has to be read
// the short way round. A plain subtraction reads this 10 degree move as -350 and
// whips the view a full turn the wrong way.
void YawCrossesTheSeamTheShortWay() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 175.0f, 0.0f));
    now += AdsFade::kLowerMs;
    const AdsPipeline::Pose out =
        Settle(pipe, AdsMode::Tracked, true, true, now, MakePose(0.0f, -175.0f, 0.0f));
    CheckNear(out.yaw, 10.0f, 1e-3f, "a 10 degree move across the seam reads as 10");
}

// Interpolators return nothing on suppressed frames, so capturing then would
// freeze a pre-suppression pose and hold the entire aim at that offset. The path
// that hits it: aim, open a menu, move your head, come back with the sights up.
void EntryCaptureIsGatedOnALiveRotation() {
    AdsPipeline pipe;
    unsigned long long now = 1000;

    // Sights up while the rotation is not live: nothing is captured, and the
    // absolute pose passes through.
    const AdsPipeline::Pose stale = MakePose(0.0f, 40.0f, 0.0f);
    const AdsPipeline::Pose out = pipe.Frame(AdsMode::Tracked, true, false, now, stale);
    CheckNear(out.yaw, 40.0f, 1e-4f, "a dead frame is passed through, not captured");
    now += AdsFade::kLowerMs;

    // The first live frame is the entry frame.
    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 60.0f, 0.0f));
    now += AdsFade::kLowerMs;
    const AdsPipeline::Pose after =
        Settle(pipe, AdsMode::Tracked, true, true, now, MakePose(0.0f, 65.0f, 0.0f));
    CheckNear(after.yaw, 5.0f, 1e-3f, "the entry frame is the first LIVE aiming frame");
}

// Lowering the weapon drops the entry pose, so the next aim measures from where
// the head is then rather than from the previous aim.
void LoweringTheWeaponDropsTheEntryPose() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 20.0f, 0.0f));
    now += AdsFade::kLowerMs;

    Settle(pipe, AdsMode::Tracked, false, true, now, MakePose(0.0f, 50.0f, 0.0f));
    const AdsPipeline::Pose hip = pipe.Frame(AdsMode::Tracked, false, true, now,
                                             MakePose(0.0f, 50.0f, 0.0f));
    CheckNear(hip.yaw, 50.0f, 1e-4f, "the hip pose is absolute again");
    now += AdsFade::kRaiseMs;

    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 50.0f, 0.0f));
    now += AdsFade::kLowerMs;
    const AdsPipeline::Pose second =
        Settle(pipe, AdsMode::Tracked, true, true, now, MakePose(0.0f, 53.0f, 0.0f));
    CheckNear(second.yaw, 3.0f, 1e-3f, "the second aim measures from its own entry frame");
}

// Reset is what a menu, a cinematic, the master toggle or a tracker dropout does
// to the aim. Without it the aim resumes against a pose from before.
void ResetDropsTheEntryPose() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 20.0f, 0.0f));
    now += AdsFade::kLowerMs;

    pipe.Reset();

    pipe.Frame(AdsMode::Tracked, true, true, now, MakePose(0.0f, 45.0f, 0.0f));
    now += AdsFade::kLowerMs;
    const AdsPipeline::Pose out =
        Settle(pipe, AdsMode::Tracked, true, true, now, MakePose(0.0f, 47.0f, 0.0f));
    CheckNear(out.yaw, 2.0f, 1e-3f, "after a reset the aim re-enters from the live pose");
}

// A tap of the aim button is the most common input there is. The reversal has to
// start from where the transition IS, or the frame after the release removes a
// fully applied pose in one step.
void ATapDoesNotStepThePose() {
    AdsPipeline pipe;
    unsigned long long now = 1000;
    const AdsPipeline::Pose in = MakePose(0.0f, 30.0f, 0.0f);

    pipe.Frame(AdsMode::Paused, false, true, now, in);
    now += 8;
    const AdsPipeline::Pose pressed = pipe.Frame(AdsMode::Paused, true, true, now, in);
    now += 8;
    const AdsPipeline::Pose released = pipe.Frame(AdsMode::Paused, false, true, now, in);

    Check(std::fabs(released.yaw - pressed.yaw) < 0.5f * std::fabs(in.yaw),
          "a tap does not step the pose by the whole travelled distance");
}

}  // namespace

int main() {
    HipFireIsUntouched();
    PausedFadesToNothingButKeepsRoll();
    TrackedIsRelativeToTheEntryFrame();
    RollIsNeverMadeRelative();
    YawCrossesTheSeamTheShortWay();
    EntryCaptureIsGatedOnALiveRotation();
    LoweringTheWeaponDropsTheEntryPose();
    ResetDropsTheEntryPose();
    ATapDoesNotStepThePose();

    if (g_failures != 0) {
        std::printf("ads_pose_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("ads_pose_test: all cases passed\n");
    return EXIT_SUCCESS;
}
