#pragma once

namespace NMSHT {

// Locates the HUD crosshair's screen position in memory.
//
// NMS does not project the on-foot crosshair. Measured with the world-to-screen
// hook: the engine's own aim point (camera position plus forward times 2000 m)
// is already built from the CLEAN camera and already projects off-centre when
// the head is turned, and no projection at all answers screen centre while the
// head is off-axis - yet the crosshair stays in the middle of the frame at every
// head angle. So it is a screen-space sprite, and moving it onto the shot means
// finding the value that positions it.
//
// The search has three stages and each one is a filter, because the first alone
// returns thousands of matches:
//   1. every float pair in writable memory that equals the centre of the screen,
//   2. of those, the ones the game REWRITES every frame - the crosshair is
//      spring-animated (CrosshairSpringTime, CrosshairAimTime), so its position
//      is live state, while the thousands of static matches are constants,
//   3. of those, the one whose writing instruction, found with a hardware write
//      watch, sits in the HUD code.
void StartCrosshairProbe();

}  // namespace NMSHT
