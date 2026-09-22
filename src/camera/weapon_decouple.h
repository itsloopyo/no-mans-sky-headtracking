#pragma once

#include <cstdint>

namespace NMSHT {

// Weapon placement uses the primary-camera accessor, bypassing the active-camera
// accessor. Supply clean rows there so animation history never contains tracking.
void InstallWeaponDecouple(std::uint32_t weaponCameraRva, bool aimIsDecoupled);

}
