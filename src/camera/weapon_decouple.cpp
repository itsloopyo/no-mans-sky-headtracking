#include "pch.h"
#include "weapon_decouple.h"

#include "camera_hook.h"
#include "core/debug_log.h"

#include <cameraunlock/hooks/hook_manager.h>

namespace NMSHT {
namespace {

using WeaponCameraFn = float* (*)(void*);
WeaponCameraFn g_original = nullptr;

// Holding the weapon on the clean camera is only right where the game shoots
// down the clean camera too. Where it does not, this drags the weapon off the
// rendered view to match a direction nothing uses, and the weapon pass - drawn
// at its own narrower field of view - magnifies that offset into the gun
// visibly swinging faster than the world.
float* WeaponCameraDetour(void* manager) {
    return WeaponCameraForTransform(g_original(manager));
}

}

void InstallWeaponDecouple(std::uint32_t weaponCameraRva, bool aimIsDecoupled) {
    if (!aimIsDecoupled) {
        HT_LOG("Weapon decoupling inactive: nothing decouples aim on this build, so "
               "the shot follows the view and the weapon stays on it too.");
        return;
    }
    if (weaponCameraRva == 0) {
        HT_LOG("Weapon decoupling inactive: no weapon camera accessor in this build profile.");
        return;
    }
    void* const target = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + weaponCameraRva);
    auto& hooks = cameraunlock::hooks::HookManager::Instance();
    auto status = hooks.CreateHook(target, reinterpret_cast<void*>(&WeaponCameraDetour),
                                   reinterpret_cast<void**>(&g_original));
    if (status == cameraunlock::hooks::HookStatus::Ok) status = hooks.EnableHook(target);
    if (status != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: weapon camera hook failed (%s).",
               cameraunlock::hooks::HookStatusToString(status));
        hooks.RemoveHook(target);
        return;
    }
    HT_LOG("Weapon camera accessor hooked at RVA 0x%08X.", weaponCameraRva);
}

}
