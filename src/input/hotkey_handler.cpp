#include "pch.h"
#include "hotkey_handler.h"

#include "camera/camera_hook.h"
#include "core/config.h"
#include "core/debug_log.h"
#include "core/mod.h"

#include <cameraunlock/input/key_binding_registration.h>

namespace NMSHT {

void HotkeyHandler::Start(const Config& config) {
    cameraunlock::input::RegisterKeyBindings(m_poller, KeyBindings(config.toggleKey),
                                             []() { Mod::Instance().Toggle(); });
    cameraunlock::input::RegisterKeyBindings(m_poller, KeyBindings(config.cycleTrackingModeKey),
                                             []() { Mod::Instance().CycleTrackingMode(); });
    if (config.aimCallerSweep || config.cullCallerSweep) {
        cameraunlock::input::RegisterKeyBindings(m_poller, KeyBindings(config.sweepFreezeKey),
                                                 []() { RequestSweepFreeze(); });
    }

    if (!m_poller.Start(16)) {
        HT_LOG("ERROR: the hotkey thread did not start - ToggleKey (%s) and "
               "CycleTrackingModeKey (%s) will do nothing this session.",
               config.toggleKey.c_str(), config.cycleTrackingModeKey.c_str());
    }
}

} // namespace NMSHT
