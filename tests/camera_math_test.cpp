// Transform suite: the row-vector camera maths the commit hook writes into the
// engine's live transform.
//
// Every number here was measured in game (see .lab/NOTES.md - yaw is negated at
// the boundary, the basis rows are right/up/BACKWARD, a forward lean is negative
// z). These cases lock the composition, the signs and the row layout so a
// restructure cannot quietly turn the view the other way.

#include "camera/camera_math.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using NMSHT::BasisEqual;
using NMSHT::BuildHeadRot3x3;
using NMSHT::ComposeTrackedRows;
using NMSHT::CopyTransform;
using NMSHT::kTransformFloats;
using NMSHT::PreMultiplyRotation;
using NMSHT::WriteBasisAndPosition;

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

constexpr float kTol = 1e-5f;

// An arbitrary but valid camera: a 30 degree yaw about world up, sitting at a
// position with a cell part, so anything that trampled row 4 would show up.
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

void ZeroRotationIsIdentity() {
    float h[9];
    BuildHeadRot3x3(0.0f, 0.0f, 0.0f, h);
    const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    for (int i = 0; i < 9; ++i) {
        CheckNear(h[i], identity[i], kTol, "zero head pose is the identity");
    }
}

void HeadRotationIsOrthonormal() {
    float h[9];
    BuildHeadRot3x3(23.0f, -14.0f, 9.0f, h);
    for (int i = 0; i < 3; ++i) {
        float len = 0.0f;
        for (int k = 0; k < 3; ++k) len += h[i * 3 + k] * h[i * 3 + k];
        CheckNear(len, 1.0f, kTol, "head rotation rows are unit length");
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = i + 1; j < 3; ++j) {
            float dot = 0.0f;
            for (int k = 0; k < 3; ++k) dot += h[i * 3 + k] * h[j * 3 + k];
            CheckNear(dot, 0.0f, kTol, "head rotation rows are orthogonal");
        }
    }
    const float det = h[0] * (h[4] * h[8] - h[5] * h[7])
                    - h[1] * (h[3] * h[8] - h[5] * h[6])
                    + h[2] * (h[3] * h[7] - h[4] * h[6]);
    CheckNear(det, 1.0f, kTol, "head rotation is a rotation, not a reflection");
}

// The row-vector form is the transpose of the column-vector composition
// C = Ry * Rx * Rz. Locking a single axis at a known angle is what catches a
// sign flip, which is the failure that reaches the player as "the view turns
// the wrong way".
void SingleAxisSigns() {
    float h[9];

    BuildHeadRot3x3(90.0f, 0.0f, 0.0f, h);
    const float yaw90[9] = { 0, 0, -1,
                             0, 1,  0,
                             1, 0,  0 };
    for (int i = 0; i < 9; ++i) CheckNear(h[i], yaw90[i], kTol, "yaw 90 matrix");

    BuildHeadRot3x3(0.0f, 90.0f, 0.0f, h);
    const float pitch90[9] = { 1,  0, 0,
                               0,  0, 1,
                               0, -1, 0 };
    for (int i = 0; i < 9; ++i) CheckNear(h[i], pitch90[i], kTol, "pitch 90 matrix");

    BuildHeadRot3x3(0.0f, 0.0f, 90.0f, h);
    const float roll90[9] = {  0, 1, 0,
                              -1, 0, 0,
                               0, 0, 1 };
    for (int i = 0; i < 9; ++i) CheckNear(h[i], roll90[i], kTol, "roll 90 matrix");
}

// Composition order is yaw, then pitch, then roll - the shared YPR doctrine.
// Built here from the three single-axis matrices so the test states the order
// independently of the closed form the implementation uses.
void CompositionOrderIsYawPitchRoll() {
    float y[9], p[9], r[9], combined[9];
    BuildHeadRot3x3(17.0f, 0.0f, 0.0f, y);
    BuildHeadRot3x3(0.0f, -11.0f, 0.0f, p);
    BuildHeadRot3x3(0.0f, 0.0f, 25.0f, r);
    BuildHeadRot3x3(17.0f, -11.0f, 25.0f, combined);

    // Row-vector transposition reverses the product: (Ry Rx Rz)^T = Rz^T Rx^T Ry^T.
    float rp[9];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += r[i * 3 + k] * p[k * 3 + j];
            rp[i * 3 + j] = sum;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += rp[i * 3 + k] * y[k * 3 + j];
            CheckNear(combined[i * 3 + j], sum, kTol,
                      "combined pose is roll * pitch * yaw in row-vector form");
        }
    }
}

void PreMultiplyByIdentityLeavesTheBasisAlone() {
    float rows[kTransformFloats], original[kTransformFloats];
    MakeCamera(rows);
    CopyTransform(original, rows);

    float h[9];
    BuildHeadRot3x3(0.0f, 0.0f, 0.0f, h);
    PreMultiplyRotation(rows, h);
    for (int i = 0; i < kTransformFloats; ++i) {
        CheckNear(rows[i], original[i], kTol, "identity pose leaves the transform alone");
    }
}

// The head rotation is applied in the camera's own frame: H * M, never M * H.
// The two differ the moment the camera is not axis-aligned, which is every
// frame in world.
void PreMultiplyIsCameraLocal() {
    float rows[kTransformFloats], camera[kTransformFloats];
    MakeCamera(rows);
    MakeCamera(camera);

    float h[9];
    BuildHeadRot3x3(-35.0f, 12.0f, 4.0f, h);
    PreMultiplyRotation(rows, h);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) sum += h[i * 3 + k] * camera[k * 4 + j];
            CheckNear(rows[i * 4 + j], sum, kTol, "rotated basis is h * basis");
        }
    }
    // Position and the fourth column ride through untouched.
    for (int i : { 3, 7, 11, 12, 13, 14, 15, 16, 17, 18, 19 }) {
        CheckNear(rows[i], camera[i], kTol, "pre-multiply touches only the basis");
    }
}

void WriteBasisAndPositionLeavesTheCellPartAlone() {
    float dst[kTransformFloats], src[kTransformFloats];
    MakeCamera(src);
    for (int i = 0; i < kTransformFloats; ++i) dst[i] = -1.0f;

    WriteBasisAndPosition(dst, src);

    for (int i : { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 }) {
        CheckNear(dst[i], src[i], kTol, "basis and fractional position are written");
    }
    for (int i : { 3, 7, 11, 15, 16, 17, 18, 19 }) {
        CheckNear(dst[i], -1.0f, kTol, "row 4 and the fourth column are left alone");
    }
}

void BasisEqualComparesOnlyTheOrientation() {
    float a[kTransformFloats], b[kTransformFloats];
    MakeCamera(a);
    MakeCamera(b);
    Check(BasisEqual(a, b), "identical transforms have equal bases");

    b[12] = 999.0f;
    b[3] = 999.0f;
    Check(BasisEqual(a, b), "position is not part of the basis comparison");

    b[5] = 0.5f;
    Check(!BasisEqual(a, b), "a changed orientation float is seen");
}

// ---------------------------------------------------------------------------
// The engine boundary. These signs are the ones AGENTS.md names as the
// fleet's recurring first-in-game report, and each was measured in this game
// (.lab/NOTES.md): +yaw swings the view RIGHT, +x moves the eye LEFT, -z is the
// forward lean, and all three only work out because the basis rows are
// right / up / BACKWARD. Nothing above this line would notice if a boundary
// negation were deleted, which is how they used to be untested.

// The camera's forward direction. Row 2 points BACKWARD, so forward is -row2.
void ForwardOf(const float* rows, float* out) {
    out[0] = -rows[8];
    out[1] = -rows[9];
    out[2] = -rows[10];
}

// A camera looking down world -Z, with world +X to its right and +Y up, placed
// away from the origin so a position term cannot hide in a zero.
void MakeLevelCamera(float* rows) {
    const float src[kTransformFloats] = {
        1.0f, 0.0f, 0.0f, 0.0f,   // right   = +X
        0.0f, 1.0f, 0.0f, 0.0f,   // up      = +Y
        0.0f, 0.0f, 1.0f, 0.0f,   // BACKWARD = +Z, so forward is -Z
        4.0f, 5.0f, 6.0f, 1.25f,
        -320512.0f, 116736.0f, 743424.0f, 1024.0f,
    };
    CopyTransform(rows, src);
}

void APositiveYawSwingsTheViewRight() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 30.0f, 0.0f, 0.0f, false, 0, 0, 0, rows);

    float fwd[3];
    ForwardOf(rows, fwd);
    // Forward started at (0,0,-1). Turning right about world up takes it toward
    // +X. If the boundary negation on yaw were dropped it would go to -X.
    CheckNear(fwd[0], std::sin(30.0f * NMSHT::kDegToRad), 1e-4f,
              "a positive yaw swings the view to the camera's RIGHT");
    CheckNear(fwd[1], 0.0f, 1e-4f, "a pure yaw does not tilt the view");
    CheckNear(fwd[2], -std::cos(30.0f * NMSHT::kDegToRad), 1e-4f,
              "a positive yaw keeps the view mostly forward");
}

void APositivePitchLooksUp() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 25.0f, 0.0f, false, 0, 0, 0, rows);

    float fwd[3];
    ForwardOf(rows, fwd);
    CheckNear(fwd[1], std::sin(25.0f * NMSHT::kDegToRad), 1e-4f,
              "a positive pitch looks UP, so pitch passes through unnegated");
}

void APositiveRollTiltsTheHorizonClockwise() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 0.0f, 25.0f, false, 0, 0, 0, rows);

    // Asserted as what the player sees, not as a matrix element, because that is
    // how it was verified: a +25 roll tilted the horizon clockwise, right end
    // down. Take a world point out to the camera's right at eye level and
    // project it with the row-vector basis - screen x is its dot with row 0,
    // screen y its dot with row 1. Right end DOWN means positive x, negative y.
    const float right[3] = { 1.0f, 0.0f, 0.0f };
    const float screenX = right[0] * rows[0] + right[1] * rows[1] + right[2] * rows[2];
    const float screenY = right[0] * rows[4] + right[1] * rows[5] + right[2] * rows[6];
    CheckNear(screenX, std::cos(25.0f * NMSHT::kDegToRad), 1e-4f,
              "a point to the camera's right stays on the right under roll");
    CheckNear(screenY, -std::sin(25.0f * NMSHT::kDegToRad), 1e-4f,
              "a positive roll puts the horizon's right end DOWN, as measured in game");

    float fwd[3];
    ForwardOf(rows, fwd);
    CheckNear(fwd[2], -1.0f, 1e-4f, "a pure roll does not change where the view points");
}

void APositiveXMovesTheEyeLeft() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 0.0f, 0.0f, true, 0.30f, 0.0f, 0.0f, rows);

    // The camera's right is +X, so moving LEFT is -X. x is mirrored at the
    // boundary; without that negation the eye would go the other way and
    // leaning would look like it works while going backwards.
    CheckNear(rows[12], clean[12] - 0.30f, 1e-5f,
              "a positive tracker x moves the eye to the camera's LEFT");
    CheckNear(rows[13], clean[13], 1e-5f, "a pure x lean does not move the eye up");
    CheckNear(rows[14], clean[14], 1e-5f, "a pure x lean does not move the eye forward");
}

void APositiveYMovesTheEyeUp() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 0.0f, 0.0f, true, 0.0f, 0.20f, 0.0f, rows);

    CheckNear(rows[13], clean[13] + 0.20f, 1e-5f,
              "a positive tracker y moves the eye UP, so y passes through");
}

void ANegativeZMovesTheEyeForward() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 0.0f, 0.0f, 0.0f, true, 0.0f, 0.0f, -0.40f, rows);

    // Forward is -Z for this camera, so a forward lean must DECREASE z. The
    // processor's forward lean is negative z and it is added along row 2
    // unnegated - which only lands forward because row 2 points backward. This
    // is the case that established the basis convention in the first place.
    float fwd[3];
    ForwardOf(clean, fwd);
    CheckNear(rows[14], clean[14] - 0.40f, 1e-5f,
              "a negative tracker z leans the eye FORWARD along -row2");
    CheckNear(fwd[2], -1.0f, 1e-5f, "and forward really is -row2 for that camera");
}

void TheLeanIsResolvedAgainstTheCleanBasisNotTheRotatedOne() {
    float clean[kTransformFloats];
    MakeLevelCamera(clean);

    // A big yaw, so a lean resolved against the rotated basis would land
    // somewhere obviously different from one resolved against the clean basis.
    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 90.0f, 0.0f, 0.0f, true, 0.30f, 0.0f, 0.0f, rows);

    // Still straight along the CLEAN camera's right axis, negated. If the lean
    // followed the head instead of the body it would have moved along z.
    CheckNear(rows[12], clean[12] - 0.30f, 1e-5f,
              "the lean follows the body, not the head-rotated view");
    CheckNear(rows[14], clean[14], 1e-5f,
              "and a yaw does not leak the lean onto another axis");
}

void ComposingWithoutPositionLeavesTheEyeWhereItWas() {
    float clean[kTransformFloats];
    MakeCamera(clean);

    float rows[kTransformFloats];
    ComposeTrackedRows(clean, 17.0f, -8.0f, 4.0f, false, 9.0f, 9.0f, 9.0f, rows);

    for (int i : { 12, 13, 14, 15 }) {
        CheckNear(rows[i], clean[i], kTol,
                  "rotation-only tracking never moves the eye");
    }
    for (int i : { 16, 17, 18, 19 }) {
        CheckNear(rows[i], clean[i], kTol, "and never touches the cell part");
    }
}

void YawModesPreservePositionAndUseDifferentAxes() {
    const float up[3] = {0, 1, 0};
    float clean[kTransformFloats], local[kTransformFloats], world[kTransformFloats];
    MakeCamera(clean);
    ComposeTrackedRows(clean, 25, -12, 7, true, 0.1f, 0.2f, -0.3f, local);
    ComposeTrackedRows(clean, 25, -12, 7, true, 0.1f, 0.2f, -0.3f, world, up);
    for (int i = 0; i < kTransformFloats; ++i)
        CheckNear(world[i], local[i], kTol, "yaw modes agree with a level clean camera");

    float tilt[9];
    BuildHeadRot3x3(0, 50, 20, tilt);
    PreMultiplyRotation(clean, tilt);
    ComposeTrackedRows(clean, 35, 0, 0, true, 0.1f, 0.2f, -0.3f, local);
    ComposeTrackedRows(clean, 35, 0, 0, true, 0.1f, 0.2f, -0.3f, world, up);
    Check(!BasisEqual(local, world), "yaw modes differ when the clean camera is tilted");
    for (int i : {1, 5, 9})
        CheckNear(world[i], clean[i], kTol, "world yaw preserves each basis vector's world Y");
    for (int i = 12; i < kTransformFloats; ++i)
        CheckNear(world[i], local[i], kTol, "yaw mode leaves lean and floating-origin cells unchanged");

    float restored[kTransformFloats];
    ComposeTrackedRows(clean, 35, 0, 0, true, 0.1f, 0.2f, -0.3f, restored);
    for (int i = 0; i < kTransformFloats; ++i)
        CheckNear(restored[i], local[i], kTol, "switching back restores camera-local composition");
}

void GravityYawPreservesElevationAtThePoles() {
    const float up[3] = {0.36f, 0.48f, 0.8f};
    for (float pitch : {-90.0f, -89.9f, 0.0f, 89.9f, 90.0f}) {
        float clean[kTransformFloats], rows[kTransformFloats], tilt[9];
        MakeCamera(clean);
        BuildHeadRot3x3(0, pitch, 23, tilt);
        PreMultiplyRotation(clean, tilt);
        ComposeTrackedRows(clean, 63, 0, 0, false, 0, 0, 0, rows, up);
        for (int row : {0, 4, 8}) {
            float before = 0, after = 0, norm = 0;
            for (int j = 0; j < 3; ++j) {
                before += clean[row + j] * up[j];
                after += rows[row + j] * up[j];
                norm += rows[row + j] * rows[row + j];
            }
            CheckNear(after, before, kTol, "gravity yaw preserves elevation about arbitrary up");
            CheckNear(norm, 1, kTol, "gravity yaw stays normalized at vertical camera pitch");
        }
        for (int i = 12; i < kTransformFloats; ++i)
            CheckNear(rows[i], clean[i], kTol, "gravity yaw leaves the eye and cell unchanged");
    }
}

}  // namespace

int main() {
    ZeroRotationIsIdentity();
    YawModesPreservePositionAndUseDifferentAxes();
    GravityYawPreservesElevationAtThePoles();
    HeadRotationIsOrthonormal();
    SingleAxisSigns();
    CompositionOrderIsYawPitchRoll();
    PreMultiplyByIdentityLeavesTheBasisAlone();
    PreMultiplyIsCameraLocal();
    WriteBasisAndPositionLeavesTheCellPartAlone();
    BasisEqualComparesOnlyTheOrientation();
    APositiveYawSwingsTheViewRight();
    APositivePitchLooksUp();
    APositiveRollTiltsTheHorizonClockwise();
    APositiveXMovesTheEyeLeft();
    APositiveYMovesTheEyeUp();
    ANegativeZMovesTheEyeForward();
    TheLeanIsResolvedAgainstTheCleanBasisNotTheRotatedOne();
    ComposingWithoutPositionLeavesTheEyeWhereItWas();

    if (g_failures != 0) {
        std::printf("camera_math_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("camera_math_test: all cases passed\n");
    return EXIT_SUCCESS;
}
