#pragma once

#include "camera_math.h"

namespace NMSHT {

// Where the shot goes, in the frame the player is looking at.
//
// The game aims down the CLEAN camera - the accessor hands every gameplay
// consumer a mirror of the untracked rows, so weapon fire, raycasts and
// projectiles all leave along the camera the mouse is pointing. The frame is
// drawn from the APPLIED camera, the rows the commit hook wrote. Those two
// differ by the head pose, so the crosshair NMS pins to the middle of the
// screen stops marking the shot the moment the head turns.
//
// This resolves the clean aim direction in the applied basis and perspective
// divides it, which puts it on the screen coordinate the shot will cross.
//
// Basis to basis, deliberately: the vectors handed in are the ones the camera
// hook actually wrote, so the projection cannot encode a different rotation
// composition from the camera. Re-deriving it from yaw/pitch/roll agrees on
// single-axis poses and drifts on combined ones. Concretely, this camera rolls
// about the FINAL view axis - BuildHeadRot3x3 turns yaw about local up, then
// pitch about the new right, then roll about the forward those two produced -
// so the whole picture turns about screen centre and the reticle has to turn
// with it. Nothing here has to know that; it falls out of consuming the rows.
//
// ROTATION ONLY. A positional lean moves the render eye off the shot eye, and
// the correction for that is `lean / distance` - which needs the distance to
// whatever the round is about to hit, and nothing in this mod can ask NMS for
// it. So the aim DIRECTION is projected, which is the same thing as marking the
// aim at infinity: correct to within a few degrees at conversational range,
// under one across a room, shrinking with distance, and with no sign in it that
// can invert. Bring the parallax term back only when both of these hold:
//   1. a live impact distance is available for the frame being drawn, from the
//      game's own aim result or a cast this mod makes, and
//   2. two shots fired at opposite leans put their rounds through one hole,
//      which is what proves the aim really is decoupled first.
// Never substitute a fixed convergence range for (1). The error it leaves is
// zero at exactly that range and changes side either side of it.
struct AimScreenOffset {
    bool  valid;
    float ndcX;  // -1 at the left edge of the frame, +1 at the right
    float ndcY;  // -1 at the bottom, +1 at the top
};

// `clean` and `applied` are five rows of four floats in the engine's layout;
// `vFovDegrees` is the vertical field of view the frame is being drawn with and
// `aspect` its width divided by its height.
inline AimScreenOffset ProjectCleanAimIntoTrackedView(const float* clean,
                                                      const float* applied,
                                                      float vFovDegrees,
                                                      float aspect) {
    AimScreenOffset out{ false, 0.0f, 0.0f };
    if (clean == nullptr || applied == nullptr) return out;
    if (!(vFovDegrees > 0.0f) || !(aspect > 0.0f)) return out;

    // Row 2 is the BACKWARD vector, not the forward one - the same fact that
    // makes the yaw negation and the unnegated lean z correct in camera_math.h.
    const float aim[3] = { -clean[8], -clean[9], -clean[10] };

    const float ax =  aim[0] * applied[0] + aim[1] * applied[1] + aim[2] * applied[2];
    const float ay =  aim[0] * applied[4] + aim[1] * applied[5] + aim[2] * applied[6];
    const float az = -(aim[0] * applied[8] + aim[1] * applied[9] + aim[2] * applied[10]);

    // Not "is it in front of the camera": as the forward component goes to zero
    // the projection goes to infinity, and a reticle at 1e30 is a NaN on its way
    // into the HUD. 0.1 is about 84 degrees off the view axis, well past the
    // frame edge at any field of view NMS offers.
    constexpr float kMinForward = 0.1f;
    if (az < kMinForward) return out;

    const float tanV = std::tan(0.5f * vFovDegrees * kDegToRad);
    if (!(tanV > 0.0f)) return out;

    out.ndcX = (ax / az) / (tanV * aspect);
    out.ndcY = (ay / az) / tanV;
    out.valid = true;
    return out;
}

}  // namespace NMSHT
