#include "pch.h"
#include "hotkey_handler.h"

#include "core/config.h"
#include "core/debug_log.h"
#include "core/mod.h"

#include <cameraunlock/input/chord_hotkeys.h>

namespace NMSHT {

namespace {
constexpr int kVkY = 'Y';
constexpr int kVkG = 'G';
} // namespace

void HotkeyHandler::Start(const Config& config) {
    using cameraunlock::input::ChordGuarded;
    using cameraunlock::input::NavGuarded;

    const auto toggle    = []() { Mod::Instance().Toggle(); };
    const auto cycleMode = []() { Mod::Instance().CycleTrackingMode(); };

    m_poller.SetToggleKey(config.toggleKey, NavGuarded(toggle));
    m_poller.AddHotkey(config.cycleModeKey, NavGuarded(cycleMode));

    m_poller.AddHotkey(kVkY, ChordGuarded(toggle));
    m_poller.AddHotkey(kVkG, ChordGuarded(cycleMode));

    if (!m_poller.Start(16)) {
        HT_LOG("ERROR: the hotkey thread did not start - the toggle (0x%02X) and "
               "mode (0x%02X) keys and the Ctrl+Shift chords will all do nothing "
               "this session.",
               static_cast<unsigned>(config.toggleKey),
               static_cast<unsigned>(config.cycleModeKey));
    }
}

} // namespace NMSHT
