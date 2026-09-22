#pragma once

namespace NMSHT {

// Starts the background installer for the Vulkan frame hooks. Called straight
// from DllMain rather than from the mod's init thread: the game resolves its
// per-device Vulkan entry points once, during device creation, and a hook
// installed after that point sees nothing at all.
void StartFramePhaseInstaller();

// Reports the installer's outcome once the log file is open. The installer
// itself runs before there is anywhere to write to.
void LogFramePhaseStatus();

} // namespace NMSHT
