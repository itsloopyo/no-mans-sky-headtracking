#pragma once

#include <cmath>

namespace NMSHT {

// The engine's camera transform: five rows of four floats.
//   rows 0..2  orientation basis (right / up / BACKWARD), row-vector layout
//   row  3     position, fractional part of NMS's floating-origin split
//   row  4     position, quantised cell part
constexpr int kTransformRows   = 5;
constexpr int kTransformFloats = kTransformRows * 4;

constexpr float kPi       = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;
// Divided in double before narrowing: 180 / (float)pi is a full ulp off, which
// would move every angle the diagnostics report.
constexpr float kRadToDeg = static_cast<float>(180.0 / 3.14159265358979323846);

// The nine floats of the orientation basis inside those rows, in order.
constexpr int kBasisIndices[9] = { 0, 1, 2, 4, 5, 6, 8, 9, 10 };

// Head rotation in the engine's ROW-VECTOR convention, so that
//   v_world = v_local * (H * M)
// applies H in the camera's own frame before M takes it to world space.
//
// A row-vector rotation matrix is the transpose of the column-vector one, so
// each block below is written transposed relative to the textbook form.
// Composition order matches the shared YPR doctrine: yaw about local up,
// then pitch about local right, then roll about local forward.
inline void BuildHeadRot3x3(float yawDeg, float pitchDeg, float rollDeg, float R[9]) {
    const float cy = std::cos(yawDeg * kDegToRad),   sy = std::sin(yawDeg * kDegToRad);
    const float cp = std::cos(pitchDeg * kDegToRad), sp = std::sin(pitchDeg * kDegToRad);
    const float cr = std::cos(rollDeg * kDegToRad),  sr = std::sin(rollDeg * kDegToRad);

    // Column-vector factors (X = right, Y = up, Z = forward):
    //   Ry = [ cy 0 sy ; 0 1 0 ; -sy 0 cy ]
    //   Rx = [ 1 0 0 ; 0 cp -sp ; 0 sp cp ]
    //   Rz = [ cr -sr 0 ; sr cr 0 ; 0 0 1 ]
    // The composed column-vector rotation is C = Ry * Rx * Rz; the row-vector
    // form this function returns is R = C^T = Rz^T * Rx^T * Ry^T.
    const float c00 = cy * cr + sy * sp * sr;
    const float c01 = -cy * sr + sy * sp * cr;
    const float c02 = sy * cp;
    const float c10 = cp * sr;
    const float c11 = cp * cr;
    const float c12 = -sp;
    const float c20 = -sy * cr + cy * sp * sr;
    const float c21 = sy * sr + cy * sp * cr;
    const float c22 = cy * cp;

    R[0] = c00; R[1] = c10; R[2] = c20;
    R[3] = c01; R[4] = c11; R[5] = c21;
    R[6] = c02; R[7] = c12; R[8] = c22;
}

// m3x3 = h3x3 * m3x3, reading and writing the basis stored in the 4-wide rows
// of the engine transform. Pre-multiplying is what makes the head rotation
// camera-local under the row-vector convention.
inline void PreMultiplyRotation(float* m, const float h[9]) {
    const float b[9] = { m[0], m[1], m[2],
                         m[4], m[5], m[6],
                         m[8], m[9], m[10] };
    float r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = h[i * 3 + 0] * b[0 * 3 + j]
                         + h[i * 3 + 1] * b[1 * 3 + j]
                         + h[i * 3 + 2] * b[2 * 3 + j];
    m[0] = r[0]; m[1] = r[1]; m[2]  = r[2];
    m[4] = r[3]; m[5] = r[4]; m[6]  = r[5];
    m[8] = r[6]; m[9] = r[7]; m[10] = r[8];
}

inline void CopyTransform(float* dst, const float* src) {
    for (int i = 0; i < kTransformFloats; ++i) dst[i] = src[i];
}

// The basis and the fractional position, leaving the fourth column of each row
// alone. Row 4, the quantised cell index of NMS's floating-origin split, is
// never touched: a head offset is orders of magnitude smaller than a cell.
inline void WriteBasisAndPosition(float* dst, const float* rows) {
    dst[0]  = rows[0];  dst[1]  = rows[1];  dst[2]  = rows[2];
    dst[4]  = rows[4];  dst[5]  = rows[5];  dst[6]  = rows[6];
    dst[8]  = rows[8];  dst[9]  = rows[9];  dst[10] = rows[10];
    dst[12] = rows[12]; dst[13] = rows[13]; dst[14] = rows[14];
}

// Bit-for-bit comparison of the nine orientation floats.
inline bool BasisEqual(const float* a, const float* b) {
    for (int i : kBasisIndices) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// Bit-for-bit comparison of exactly the twelve floats WriteBasisAndPosition
// writes. Used to tell whether the engine has actually committed a camera into
// a transform, or whether what is sitting there is still the mod's own last
// write - which is what happens if the engine commits somewhere else.
inline bool WrittenRowsEqual(const float* a, const float* b) {
    for (int i : kBasisIndices) {
        if (a[i] != b[i]) return false;
    }
    return a[12] == b[12] && a[13] == b[13] && a[14] == b[14];
}

// The clean camera with the head pose composed onto it, into `rows`.
//
// Every sign here is a boundary conversion between the tracker's convention and
// this engine's, each verified in game (.lab/NOTES.md): yaw is negated, pitch
// and roll pass through, x is mirrored and z is not. The z pass-through only
// holds because the basis rows are right / up / BACKWARD, which is what the
// forward-lean test established. None of these is ever a user-facing setting.
inline void ComposeTrackedRows(const float* clean,
                               float yaw, float pitch, float roll,
                               bool havePosition, float x, float y, float z,
                               float* rows, const float* gravityUp = nullptr) {
    CopyTransform(rows, clean);

    float h[9];
    BuildHeadRot3x3(gravityUp ? 0.0f : -yaw, pitch, roll, h);
    PreMultiplyRotation(rows, h);

    if (gravityUp) {
        const float c = std::cos(-yaw * kDegToRad);
        const float s = std::sin(-yaw * kDegToRad);
        for (int i = 0; i < 3; ++i) {
            const int row = i * 4;
            const float rx = rows[row], ry = rows[row + 1], rz = rows[row + 2];
            const float ux = gravityUp[0], uy = gravityUp[1], uz = gravityUp[2];
            const float d = (ux * rx + uy * ry + uz * rz) * (1.0f - c);
            rows[row] = c * rx + s * (uy * rz - uz * ry) + d * ux;
            rows[row + 1] = c * ry + s * (uz * rx - ux * rz) + d * uy;
            rows[row + 2] = c * rz + s * (ux * ry - uy * rx) + d * uz;
        }
    }

    if (!havePosition) return;

    // Resolved against the CLEAN basis so the lean follows the body rather than
    // the head-rotated view. Both flips happen HERE, after the processor's
    // clamp, never through an invert flag - those apply before the clamp and
    // would move the generous 0.40 m forward-lean budget onto the backward lean.
    const float ex = -x;
    const float ez =  z;
    rows[12] += clean[0] * ex + clean[4] * y + clean[8]  * ez;
    rows[13] += clean[1] * ex + clean[5] * y + clean[9]  * ez;
    rows[14] += clean[2] * ex + clean[6] * y + clean[10] * ez;
}

}  // namespace NMSHT
