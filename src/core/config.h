#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <cameraunlock/config/config_concepts.g.h>
#include <cameraunlock/config/config_owner.h>
#include <cameraunlock/config/config_table.h>
#include <cameraunlock/config/legacy_import.h>
#include <cameraunlock/data/position_settings.h>
#include <cameraunlock/input/key_bindings.h>
#include <cameraunlock/math/smoothing_utils.h>
#include <cameraunlock/tracking/tracking_mode.h>

namespace NMSHT {

// Every setting HeadTracking.ini holds. Its canonical format, the file's rows
// and their comments are ConfigTable(); ConfigOwner is the only reader and
// writer of the file.
struct Config {
    std::uint16_t udpPort = 4242;
    bool enableOnStartup = true;
    // The tracking mode at startup, with positionEnabled.
    bool rotationEnabled = true;

    // Which of the two applies is decided per connection from the packet
    // source address; both cover rotation and position alike.
    float localSmoothing  = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
    float remoteSmoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);

    bool  positionEnabled = true;
    float posLimitX     = cameraunlock::PositionSettings{}.limit_x;
    float posLimitY     = cameraunlock::PositionSettings{}.limit_y;
    float posLimitYDown = cameraunlock::PositionSettings{}.limit_y_down;
    float posLimitZ     = cameraunlock::PositionSettings{}.limit_z;
    float posLimitZBack = cameraunlock::PositionSettings{}.limit_z_back;

    std::string toggleKey = cameraunlock::config::schema::ConceptTraits<
        cameraunlock::config::schema::Concept::ToggleKey>::kCanonicalDefault;
    std::string cycleTrackingModeKey = cameraunlock::config::schema::ConceptTraits<
        cameraunlock::config::schema::Concept::CycleTrackingModeKey>::kCanonicalDefault;

    bool writeLog = true;

    // Writes engine-discovery detail (vtable layouts, call sites, transform
    // dumps) to the log. Off by default: it is the evidence a bug report needs,
    // not something a player benefits from.
    bool diagnostics = false;

    // Puts a CPU data breakpoint on the live camera transform and logs which
    // instructions read it, in short bursts. The only way to identify the
    // renderer: it reads that memory directly, not through the accessor.
    bool readWatch  = false;
    // Serves the CLEAN camera to one candidate caller at a time, cycling every
    // few seconds, so the consumer behind a wrong-looking aim can be found by
    // watching the game rather than by guessing addresses. Diagnostic only.
    bool aimCallerSweep = false;
    // Same sweep, the other way round: serves the TRACKED camera to one
    // candidate at a time while the rest of the build stays clean. For finding
    // which consumer decides visibility, when culling is following the wrong
    // camera. Diagnostic only.
    bool cullCallerSweep = false;
    // Freezes either sweep on the candidate it is serving, and names it. A
    // key, not "quit when it looks right": quitting takes seconds and the sweep
    // has moved on by then, where a press lands on the candidate under the
    // player's eye.
    std::string sweepFreezeKey = "Ctrl+Shift+J";
    // Records every distinct return address that calls the camera accessor,
    // with a hit count, and dumps the table to the log. The caller lists a
    // profile pins have to come from somewhere, and on a build nobody has
    // censused they were being guessed from another store's addresses, which
    // are not call sites here at all. Diagnostic only.
    bool callerCensus = false;
    // Applies the three caller lists below in place of the build profile's,
    // and applies them again whenever the file changes, so a candidate set can
    // be tried without a rebuild and a save load. Separate from the census
    // because counting every accessor call is expensive and the override is not.
    bool liveCallerOverrides = false;
    bool writeWatch = false;

    // Locates the nodes the engine hangs off the camera, and cycles the engine's
    // camera globals one at a time, in the hunt for whatever places the
    // viewmodel.
    bool weaponProbe      = false;
    bool cleanGlobalCycle = false;

    // Hunts for the HUD crosshair's screen position in memory and holds each
    // candidate away from centre in turn. Diagnostic: the window in which the
    // crosshair moves names the address.
    bool crosshairProbe = false;

    // Walks the crosshair around a fixed square instead of following the aim,
    // so that "the reticle does not move" can be told apart from "the reticle
    // moves to the wrong place". Diagnostic.
    bool reticleSweep = false;

    // Caller RVAs for liveCallerOverrides. Empty keeps the build profile's.
    std::vector<std::uint32_t> aimTransformCallers;
    std::vector<std::uint32_t> aimCopyCallers;
    std::vector<std::uint32_t> trackedTransformCallers;
    // Byte offset into the camera transform that readWatch covers.
    std::uint32_t readWatchOffset = 0;
    // The scene sample hook's RVA in place of the build profile's: empty keeps
    // the profile's, and one entry replaces it, 0x0 turning the hook off.
    std::vector<std::uint32_t> sceneSampleRva;
};

// The game's name as cameraunlock-core's data/games.json spells it.
inline constexpr const char* kGameDisplayName = "No Man's Sky";

// At most this many callers of each override list are applied.
inline constexpr std::size_t kMaxOverrideCallers = 160;

cameraunlock::config::ConfigTable<Config> ConfigTable();

// The import of a HeadTracking.ini an older build wrote: the frozen reader in
// src/legacy_config/, and the map from what it read into Config.
cameraunlock::config::LegacyImport<Config> ConfigLegacyImport();

// The owner's options for the file at `path`, a full path.
cameraunlock::config::ConfigOwnerOptions<Config> ConfigOwnerOptions(std::wstring path);

// The tracking mode the RotationEnabled / PositionEnabled pair names. The
// table never gives a config both false.
cameraunlock::TrackingMode StartupTrackingMode(const Config& config);

// A hotkey list from Config as bindings. Throws std::invalid_argument for a list
// ParseKeyBindings refuses, which the table's hotkey codec never lets through.
std::vector<cameraunlock::input::KeyBinding> KeyBindings(const std::string& list);

} // namespace NMSHT
