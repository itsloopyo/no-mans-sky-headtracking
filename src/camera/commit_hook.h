#pragma once

#include "build_profile.h"

#include <cstdint>

namespace NMSHT {

// Injects the head rotation immediately after the engine commits the camera
// for the frame.
//
// The swapchain is the wrong clock for this game. Measured on 2026-08-31:
// rotating between vkAcquireNextImageKHR and vkQueuePresentKHR moves nothing on
// screen, while the same rotation left in place moves the whole view, so the
// renderer samples the camera in the present -> acquire span, before our window
// ever opened. A write watch then found exactly ONE instruction that writes the
// live transform, once a frame, and injecting right behind it is the engine's
// own boundary rather than a guess at one.
//
// The site is an address inside a chunked function with no direct callers, so
// there is nothing MinHook can attach to. It is trapped with an execution
// hardware breakpoint instead, which is why it fires through a vectored handler
// rather than a detour.
// `thirdPersonCommitRva` is the second site, zero where the build needs none:
// the Steam image inlines the write into each camera behaviour, so the first
// site covers first person only and this one covers the third-person cameras.
void InstallCommitHook(std::uint32_t commitRva, std::uint32_t thirdPersonCommitRva,
                       BuildProfile::CommitCameraReg cameraReg);

// Frames that have reached the commit site.
std::uint64_t CommitHitCount();

} // namespace NMSHT
