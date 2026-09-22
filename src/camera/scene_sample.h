#pragma once

#include <cstdint>

namespace NMSHT {

// Traps the instruction right after the one where the main thread reads the
// camera's render rows to set up the frame, and records which commit's rows it
// read. The reticle is then placed from that commit, not the newest one.
//
// On Game Pass the camera is committed by whichever thread the job system hands
// the camera update to, so a commit can land between the scene reading the
// camera and the HUD placing the reticle, or not land in a frame at all. The
// newest commit is then one step ahead of or behind the picture, and the
// reticle jumps by one commit's worth of mouse or head movement and snaps back.
//
// An execute hardware breakpoint, like the commit hook, because the site is an
// instruction inside a function rather than an entry MinHook can attach to.
void InstallSceneSampleHook(std::uint32_t sampleRva);

// Frames that have reached the sample site.
std::uint64_t SceneSampleCount();

}  // namespace NMSHT
