#include "pch.h"
#include "hotkey_handler.h"

#include "core/ads.h"
#include "core/config.h"
#include "core/debug_log.h"
#include "core/mod.h"

#include <cameraunlock/input/chord_hotkeys.h>

namespace NMSHT {

namespace {
constexpr int kVkY = 'Y';
constexpr int kVkG = 'G';
constexpr int kVkU = 'U';
} // namespace

void HotkeyHandler::Start(const Config& config) {
    using cameraunlock::input::ChordGuarded;
    using cameraunlock::input::NavGuarded;

    const auto toggle    = []() { Mod::Instance().Toggle(); };
    const auto cycleMode = []() { Mod::Instance().CycleTrackingMode(); };
    const auto cycleAds  = []() { Ads::Instance().CycleMode(); };

    m_poller.SetToggleKey(config.toggleKey, NavGuarded(toggle));
    m_poller.AddHotkey(config.cycleModeKey, NavGuarded(cycleMode));
    m_poller.AddHotkey(config.adsModeKey,   NavGuarded(cycleAds));

    m_poller.AddHotkey(kVkY, ChordGuarded(toggle));
    m_poller.AddHotkey(kVkG, ChordGuarded(cycleMode));
    m_poller.AddHotkey(kVkU, ChordGuarded(cycleAds));

    if (!m_poller.Start(16)) {
        HT_LOG("ERROR: the hotkey thread did not start - the toggle (0x%02X), "
               "mode (0x%02X) and ADS (0x%02X) keys and the Ctrl+Shift chords "
               "will all do nothing this session.",
               static_cast<unsigned>(config.toggleKey),
               static_cast<unsigned>(config.cycleModeKey),
               static_cast<unsigned>(config.adsModeKey));
    }
}

} // namespace NMSHT
