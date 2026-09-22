// Reticle suite: where the clean aim lands in the frame the head-tracked
// camera drew.
//
// The camera hook composes the head pose onto the clean rows and writes the
// result into the engine's live transform, while every gameplay consumer is
// handed the clean rows through the accessor. So the shot leaves along the
// clean forward and the frame is drawn from the applied basis, and these cases
// pin the one arithmetic that reconciles them - including its signs, which are
// what decide whether the crosshair chases the shot or runs away from it.

#include "camera/aim_projection.h"
#include "camera/camera_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using NMSHT::AimScreenOffset;
using NMSHT::BuildHeadRot3x3;
using NMSHT::ComposeTrackedRows;
using NMSHT::CopyTransform;
using NMSHT::kTransformFloats;
using NMSHT::ProjectCleanAimIntoTrackedView;

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
    std::printf("FAIL: %s (expected %.5f, got %.5f)\n", what, expected, actual);
}

constexpr float kTol = 1e-4f;

// The vertical field of view the engine renders with at the default setting:
// Options > Camera reads 75, and the projection takes a quarter of that as the
// vertical HALF angle, so the vertical field is 37.5 degrees.
constexpr float kVFov = 37.5f;
constexpr float kAspect = 16.0f / 9.0f;

// A camera looking down world -Z with a 30 degree yaw already on it, so nothing
// here can pass by accident on an axis-aligned basis. Rows are
// right / up / BACKWARD in the engine's row-vector layout.
void MakeCamera(float* rows) {
    const float c = std::cos(30.0f * NMSHT::kDegToRad);
    const float s = std::sin(30.0f * NMSHT::kDegToRad);
    const float src[kTransformFloats] = {
           c, 0.0f,   -s, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
           s, 0.0f,    c, 0.0f,
        4.0f, 5.0f, 6.0f, 1.25f,
        -320512.0f, 116736.0f, 743424.0f, 1024.0f,
    };
    CopyTransform(rows, src);
}

void Tracked(const float* clean, float yaw, float pitch, float roll, float* rows) {
    ComposeTrackedRows(clean, yaw, pitch, roll, false, 0.0f, 0.0f, 0.0f, rows);
}

void ACentredHeadLeavesTheReticleAtScreenCentre() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 0.0f, 0.0f, 0.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "a centred head projects");
    CheckNear(r.ndcX, 0.0f, kTol, "a centred head leaves the reticle at centre in x");
    CheckNear(r.ndcY, 0.0f, kTol, "a centred head leaves the reticle at centre in y");
}

// The whole point of the correction: the view swings right, the aim did not, so
// the crosshair has to travel LEFT to stay on the shot.
void TurningTheHeadRightMovesTheReticleLeft() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 12.0f, 0.0f, 0.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "a yawed head projects");
    Check(r.ndcX < 0.0f, "a head turned right puts the reticle left of centre");
    CheckNear(r.ndcY, 0.0f, kTol, "pure yaw does not move the reticle vertically");
}

void LookingUpMovesTheReticleDown() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 0.0f, 12.0f, 0.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "a pitched head projects");
    Check(r.ndcY < 0.0f, "a head looking up puts the reticle below centre");
    CheckNear(r.ndcX, 0.0f, kTol, "pure pitch does not move the reticle horizontally");
}

// Roll turns the view about the axis the centred aim already lies on, so an aim
// that was at the centre stays there. This passes whether or not the projection
// rotates the offset by roll - it is the combined case below that separates
// those two, and only one of them matches what the camera does.
void RollAloneLeavesTheReticleAtCentre() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 0.0f, 0.0f, 25.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "a rolled head projects");
    CheckNear(r.ndcX, 0.0f, kTol, "pure roll leaves the reticle centred in x");
    CheckNear(r.ndcY, 0.0f, kTol, "pure roll leaves the reticle centred in y");
}

// Where roll sits in the composition decides what it does to the reticle, and
// this is the case that says which. BuildHeadRot3x3 rotates yaw about local up,
// then pitch about the new right, then roll about the FINAL view axis - so roll
// is outermost, the whole picture turns about screen centre, and an aim point
// that pitch had already pushed off centre has to turn with it.
//
// Measured in angle space, not in normalised coordinates: the horizontal axis
// carries the aspect, so the offset is an ellipse there rather than a circle.
void RollTurnsAPitchedReticleAboutScreenCentre() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float pitchOnly[kTransformFloats];
    Tracked(clean, 0.0f, 15.0f, 0.0f, pitchOnly);
    float pitchAndRoll[kTransformFloats];
    Tracked(clean, 0.0f, 15.0f, 30.0f, pitchAndRoll);

    const AimScreenOffset a =
        ProjectCleanAimIntoTrackedView(clean, pitchOnly, kVFov, kAspect);
    const AimScreenOffset b =
        ProjectCleanAimIntoTrackedView(clean, pitchAndRoll, kVFov, kAspect);
    Check(a.valid && b.valid, "pitch and pitch+roll both project");

    const float ax = a.ndcX * kAspect, ay = a.ndcY;
    const float bx = b.ndcX * kAspect, by = b.ndcY;
    const float aLen = std::sqrt(ax * ax + ay * ay);
    const float bLen = std::sqrt(bx * bx + by * by);
    CheckNear(bLen, aLen, kTol, "roll on top of pitch does not change how far "
                                "off centre the reticle sits");

    const float cosTurn = (ax * bx + ay * by) / (aLen * bLen);
    CheckNear(std::acos(cosTurn) * NMSHT::kRadToDeg, 30.0f, 1e-2f,
              "roll on top of pitch turns the reticle by exactly the roll angle");
}

// The x and y half-angles differ by the aspect, so the same angular offset is a
// smaller fraction of the frame horizontally than vertically. Yaw and pitch of
// equal size therefore have to come back in that ratio, or the reticle is
// scaled wrong on one axis and agrees with the shot only along the other.
void TheAspectScalesTheHorizontalAxis() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float yawed[kTransformFloats];
    Tracked(clean, 10.0f, 0.0f, 0.0f, yawed);
    float pitched[kTransformFloats];
    Tracked(clean, 0.0f, 10.0f, 0.0f, pitched);

    const AimScreenOffset x =
        ProjectCleanAimIntoTrackedView(clean, yawed, kVFov, kAspect);
    const AimScreenOffset y =
        ProjectCleanAimIntoTrackedView(clean, pitched, kVFov, kAspect);
    Check(x.valid && y.valid, "equal yaw and pitch both project");
    CheckNear(std::fabs(x.ndcX) * kAspect, std::fabs(y.ndcY), kTol,
              "equal yaw and pitch differ on screen by exactly the aspect");
}

// A 10 degree yaw at a 37.5 degree vertical field: the offset is
// tan(10) / (tan(18.75) * aspect) of half the frame width. Locking the
// arithmetic, not just its sign, is what catches a half-angle used where a full
// angle belongs - which is a fixed scale error that looks like a working
// reticle until someone measures it.
void TheMagnitudeIsTheTangentRatio() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 10.0f, 0.0f, 0.0f, applied);

    const float expected = std::tan(10.0f * NMSHT::kDegToRad) /
                           (std::tan(0.5f * kVFov * NMSHT::kDegToRad) * kAspect);
    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "the magnitude case projects");
    CheckNear(std::fabs(r.ndcX), expected, kTol,
              "a 10 degree yaw lands at the tangent ratio of half the frame");
}

// Measured in the running game, not derived here: two captures of the same
// scene at head yaw 0 and head yaw +30 put a world-anchored HUD marker at
// normalised -0.14896 and +0.75521 of half the frame width (.lab/NOTES.md).
// Solving those two for the frame's own half-angle gives tan(hfov/2) = 0.6117,
// so the clean aim - which sits at the centre of the yaw-0 frame - belongs
// 0.9439 of a half-frame to the LEFT once the view has swung 30 degrees.
//
// This is the case that would catch an over-correction. Everything else here
// checks the projection against itself; this checks it against the game.
void ThirtyDegreesMatchesTheViewSwingMeasuredInGame() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 30.0f, 0.0f, 0.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(r.valid, "the measured case projects");
    Check(r.ndcX < 0.0f, "a 30 degree head turn to the right puts the aim left");
    // 3% covers reading the marker off a pair of screenshots; a factor-of-two
    // error, or a field of view taken for the full angle instead of the half,
    // is nowhere near it.
    CheckNear(std::fabs(r.ndcX), 0.9439f, 0.03f,
              "a 30 degree head turn moves the reticle as far as the world "
              "actually moved in game");
}

// Past the edge of the frame the projection still has to answer something
// finite, and at a right angle it has to refuse rather than divide by a
// vanishing forward component.
void AnAimBehindTheViewIsRefused() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 120.0f, 0.0f, 0.0f, applied);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, applied, kVFov, kAspect);
    Check(!r.valid, "an aim behind the view is refused");
}

void MissingCameraOrFieldOfViewIsRefused() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float applied[kTransformFloats];
    Tracked(clean, 5.0f, 0.0f, 0.0f, applied);

    Check(!ProjectCleanAimIntoTrackedView(nullptr, applied, kVFov, kAspect).valid,
          "no clean camera is refused");
    Check(!ProjectCleanAimIntoTrackedView(clean, nullptr, kVFov, kAspect).valid,
          "no applied camera is refused");
    Check(!ProjectCleanAimIntoTrackedView(clean, applied, 0.0f, kAspect).valid,
          "an unreadable field of view is refused");
    Check(!ProjectCleanAimIntoTrackedView(clean, applied, kVFov, 0.0f).valid,
          "an unreadable aspect is refused");
}

// The eye moves with a lean and the aim ray does not, so a lean must not move
// the reticle: this projection marks the aim at infinity, where the parallax
// term is zero. The day an impact distance is available that stops being true,
// and this case is what will say so.
void ALeanDoesNotMoveTheReticle() {
    float clean[kTransformFloats];
    MakeCamera(clean);
    float leaned[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 0.0f, 0.0f, true, 0.30f, 0.0f, 0.0f, leaned);

    const AimScreenOffset r =
        ProjectCleanAimIntoTrackedView(clean, leaned, kVFov, kAspect);
    Check(r.valid, "a leaned camera projects");
    CheckNear(r.ndcX, 0.0f, kTol, "a lean leaves the reticle centred in x");
    CheckNear(r.ndcY, 0.0f, kTol, "a lean leaves the reticle centred in y");
}

}  // namespace

int main() {
    ACentredHeadLeavesTheReticleAtScreenCentre();
    TurningTheHeadRightMovesTheReticleLeft();
    LookingUpMovesTheReticleDown();
    RollAloneLeavesTheReticleAtCentre();
    RollTurnsAPitchedReticleAboutScreenCentre();
    TheAspectScalesTheHorizontalAxis();
    TheMagnitudeIsTheTangentRatio();
    ThirtyDegreesMatchesTheViewSwingMeasuredInGame();
    AnAimBehindTheViewIsRefused();
    MissingCameraOrFieldOfViewIsRefused();
    ALeanDoesNotMoveTheReticle();

    if (g_failures != 0) {
        std::printf("aim_projection_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("aim_projection_test: all cases passed\n");
    return EXIT_SUCCESS;
}
