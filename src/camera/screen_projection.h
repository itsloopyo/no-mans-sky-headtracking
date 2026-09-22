#pragma once

#include <cstdint>

namespace NMSHT {

// Hooks the engine's world-to-screen projection, cTkGraphicsManager's
//
//   Project(mgr, rows, fov, points, out, count, flags, screenSize)
//
// where `rows` is a camera transform in the engine's five-row layout, `points`
// is `count` world positions of two 16-byte rows each (the fractional position
// then the cell index of the floating-origin split) and `out` receives one
// 16-byte screen position per point.
//
// `flags` picks the output space, which is the detail that hid the crosshair
// from the first search: non-zero returns 0..1 normalised coordinates and the
// caller scales them itself, zero returns pixels scaled by `screenSize`. The
// camera-manager helper at 0x0065CEE0 passes flags=1 with a hard-coded
// {1920,1080}, so its answers are normalised and screen centre is (0.5, 0.5),
// not (960, 540).
//
// Measured in world: 97% of the engine's projections are already handed the
// HEAD-TRACKED camera, because the HUD reads the live transform straight from
// memory and this mod leaves it rotated. Not one is handed the clean camera.
// So world-anchored markers are already projected through the frame the player
// is looking at, and swapping the camera here would correct nothing.
//
// The reticle does NOT come through here at all, which is what this hook
// established and why it is now diagnostics only. The engine's own aim point
// (camera position plus forward times 2000 m) is already built from the CLEAN
// camera and already lands off-centre once the head turns, and across minutes of
// play at 12 to 35 degrees of yaw not one single-point projection answered
// screen centre - while the crosshair stayed in the middle of the frame
// throughout. It is a screen-space sprite; see crosshair_probe.h.
//
// Installed only when [Debug] Diagnostics is on: this function runs about
// 150,000 times a minute and a player gains nothing from the counters.
void InstallScreenProjection(std::uint32_t worldToScreenRva);

// Projections seen, and which camera each was handed, compared bit for bit.
// `applied` counts consumers that read the engine's live transform straight
// from memory; `clean` counts those that went through the accessor; `other` is
// a third camera - the frontend and loading UI rig, which sits at the origin.
std::uint64_t ProjectionCallCount();
std::uint64_t ProjectionCleanCount();
std::uint64_t ProjectionAppliedCount();
std::uint64_t ProjectionOtherCount();

}  // namespace NMSHT
