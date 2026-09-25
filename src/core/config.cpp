#include "pch.h"
#include "config.h"

#include "debug_log.h"
#include "legacy_config/legacy_config.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace NMSHT {

namespace cfg = cameraunlock::config;
using cfg::schema::Concept;
using cameraunlock::input::KeyBinding;
using cameraunlock::input::KeyModifiers;

cfg::ConfigTable<Config> ConfigTable() {
    cfg::ConfigTable<Config> table{Config{}};
    table.Concept<Concept::UdpPort>(&Config::udpPort)
        .Concept<Concept::EnableOnStartup>(&Config::enableOnStartup)
        .Concept<Concept::RotationEnabled>(&Config::rotationEnabled)
        .Writable()
        .Concept<Concept::LocalSmoothing>(&Config::localSmoothing)
        .Concept<Concept::RemoteSmoothing>(&Config::remoteSmoothing)
        .Concept<Concept::PositionEnabled>(&Config::positionEnabled)
        .Writable()
        .Concept<Concept::PositionLimitX>(&Config::posLimitX)
        .Concept<Concept::PositionLimitY>(&Config::posLimitY)
        .Concept<Concept::PositionLimitYDown>(&Config::posLimitYDown)
        .Concept<Concept::PositionLimitZ>(&Config::posLimitZ)
        .Concept<Concept::PositionLimitZBack>(&Config::posLimitZBack)
        .Concept<Concept::ToggleKey>(&Config::toggleKey)
        .Concept<Concept::CycleTrackingModeKey>(&Config::cycleTrackingModeKey)
        .Local("Logging", "WriteLog", &Config::writeLog, cfg::BoolCodec(),
               "true: write HeadTracking.log beside NMS.exe. It starts fresh every launch.")
        .Local("Debug", "Diagnostics", &Config::diagnostics, cfg::BoolCodec(),
               "Everything in this section is for tracking down a fault; leave it as it is unless you\n"
               "were asked to change it.\n"
               "true: write engine-discovery detail to HeadTracking.log. Turn it on for a bug report.")
        .Local("Debug", "ReadWatch", &Config::readWatch, cfg::BoolCodec(),
               "true: log which instructions read the live camera transform, using a CPU data\n"
               "breakpoint. The game crawls while each burst is armed. ReadWatch and WriteWatch share\n"
               "one debug register, so only the first of the two to start is armed.")
        .Local("Debug", "WriteWatch", &Config::writeWatch, cfg::BoolCodec(),
               "true: log which instructions write the live camera transform, the same way.")
        .Local("Debug", "AimCallerSweep", &Config::aimCallerSweep, cfg::BoolCodec(),
               "true: serve the clean camera to one candidate caller at a time, cycling every few\n"
               "seconds, and log each one, to find which consumer aims something that aims wrongly.")
        .Local("Debug", "CullCallerSweep", &Config::cullCallerSweep, cfg::BoolCodec(),
               "true: the same sweep the other way round, serving the tracked camera to one candidate\n"
               "at a time, to find what decides visibility.")
        .Local("Debug", "CallerCensus", &Config::callerCensus, cfg::BoolCodec(),
               "true: count every caller of the camera accessor and write the counts to the log\n"
               "every five seconds.")
        .Local("Debug", "LiveCallerOverrides", &Config::liveCallerOverrides, cfg::BoolCodec(),
               "true: use AimTransformCallers, AimCopyCallers and TrackedTransformCallers in place of\n"
               "the build's own callers, and read them again whenever this file changes.")
        .Local("Debug", "WeaponProbe", &Config::weaponProbe, cfg::BoolCodec(),
               "true: locate the first-person weapon transform and write one report to the log.")
        .Local("Debug", "CleanGlobalCycle", &Config::cleanGlobalCycle, cfg::BoolCodec(),
               "true: hold each engine camera global clean in turn, to find which one places the\n"
               "first-person weapon.")
        .Local("Debug", "CrosshairProbe", &Config::crosshairProbe, cfg::BoolCodec(),
               "true: hunt for the crosshair's screen position in memory.")
        .Local("Debug", "ReticleSweep", &Config::reticleSweep, cfg::BoolCodec(),
               "true: walk the crosshair around a fixed square instead of following the aim, to tell\n"
               "a crosshair that does not move apart from one that moves to the wrong place.")
        .Local("Debug", "AimTransformCallers", &Config::aimTransformCallers, cfg::Hex32ListCodec(),
               "Caller addresses, relative to NMS.exe, for LiveCallerOverrides: those served the clean\n"
               "camera through the accessor. Empty keeps the build's own.")
        .Engine()
        .Local("Debug", "AimCopyCallers", &Config::aimCopyCallers, cfg::Hex32ListCodec(),
               "Those served the clean camera through the copy. Empty keeps the build's own.")
        .Engine()
        .Local("Debug", "TrackedTransformCallers", &Config::trackedTransformCallers, cfg::Hex32ListCodec(),
               "Those served the tracked camera through the accessor. Empty keeps the build's own.")
        .Engine()
        .Local("Debug", "ReadWatchOffset", &Config::readWatchOffset, cfg::Hex32Codec(),
               "Byte offset into the camera transform that ReadWatch covers.")
        .Engine()
        .Local("Debug", "SceneSampleRva", &Config::sceneSampleRva, cfg::Hex32ListCodec(),
               "Address, relative to NMS.exe, of the once-a-frame scene sample in place of the build's\n"
               "own. Empty keeps the build's own; 0x0 turns the sample off.")
        .Engine();
    return table;
}

namespace {

// A legacy action: its code in the file, which fired while Ctrl and Shift were
// not both held, and the Ctrl+Shift letter the build fixed in code. The frozen
// reader keeps only a code GetAsyncKeyState can poll that is not a modifier, so
// every code here has a binding.
std::string WithChord(int code, char letter) {
    return cameraunlock::input::FormatKeyBindings(
        {KeyBinding{KeyModifiers::kNone, code}, KeyBinding{KeyModifiers::kCtrl | KeyModifiers::kShift, letter}});
}

// A legacy caller list. The published build kept the build's own callers for
// an empty value, which is an empty list here. A value that listed no caller
// (such as ",") served the override to no caller at all; a list holding only
// 0x0 does the same, since no call returns to offset 0 of NMS.exe, its header.
std::vector<std::uint32_t> Callers(const std::optional<std::vector<std::uint32_t>>& legacy) {
    if (!legacy) return {};
    if (legacy->empty()) return {0};
    return *legacy;
}

cfg::ImportResult RunImport(const cfg::LegacyInput& input, Config& out) {
    legacy::Config read;
    const legacy::ReadStatus status = legacy::Read(input.ansi_path, read, &cameraunlock::logging::Line);

    std::vector<cfg::DroppedValue> dropped;
    std::vector<cfg::PoseShapingValue> poseShaping;
    // The shipped file set every sensitivity to 1 and every inversion to false,
    // so the mod applies the pose as the tracker sends it and folds nothing.
    const auto shaping = [&](auto value, auto shipped, const char* section, const char* key) {
        cfg::LegacyPoseShaping(value, shipped, section, key, poseShaping, dropped);
    };
    shaping(read.yawSensitivity, 1.0f, "Sensitivity", "YawMultiplier");
    shaping(read.pitchSensitivity, 1.0f, "Sensitivity", "PitchMultiplier");
    shaping(read.rollSensitivity, 1.0f, "Sensitivity", "RollMultiplier");
    shaping(read.invertYaw, false, "Sensitivity", "InvertYaw");
    shaping(read.invertPitch, false, "Sensitivity", "InvertPitch");
    shaping(read.invertRoll, false, "Sensitivity", "InvertRoll");
    shaping(read.posSensitivityX, 1.0f, "Position", "SensitivityX");
    shaping(read.posSensitivityY, 1.0f, "Position", "SensitivityY");
    shaping(read.posSensitivityZ, 1.0f, "Position", "SensitivityZ");
    shaping(read.posInvertX, false, "Position", "InvertX");
    shaping(read.posInvertY, false, "Position", "InvertY");
    shaping(read.posInvertZ, false, "Position", "InvertZ");

    out.udpPort = read.udpPort;
    out.enableOnStartup = read.autoEnable;
    // [Position] Enabled chose the startup mode and nothing else: the cycle
    // key reached every mode either way.
    const auto channels = cameraunlock::EncodeTrackingMode(read.positionEnabled
                                                               ? cameraunlock::TrackingMode::RotationAndPosition
                                                               : cameraunlock::TrackingMode::RotationOnly);
    out.rotationEnabled = channels.rotation_enabled;
    out.positionEnabled = channels.position_enabled;
    out.localSmoothing = read.localSmoothing;
    out.remoteSmoothing = read.remoteSmoothing;
    out.posLimitX = read.posLimitX;
    out.posLimitY = read.posLimitY;
    out.posLimitYDown = read.posLimitYDown;
    out.posLimitZ = read.posLimitZ;
    out.posLimitZBack = read.posLimitZBack;
    // The crosshair always follows the aim now; only a file that turned that
    // off loses something.
    if (!read.reticleFollowsAim) dropped.push_back({cfg::DropRule::Reticle, "Reticle", "FollowAim", "false"});
    out.toggleKey = WithChord(read.toggleKey, 'Y');
    out.cycleTrackingModeKey = WithChord(read.cycleModeKey, 'G');
    out.writeLog = read.logToFile;
    out.diagnostics = read.diagnostics;
    out.readWatch = read.readWatch;
    out.aimCallerSweep = read.aimCallerSweep;
    out.cullCallerSweep = read.cullCallerSweep;
    out.callerCensus = read.callerCensus;
    out.liveCallerOverrides = read.liveCallerOverrides;
    out.writeWatch = read.writeWatch;
    out.weaponProbe = read.weaponProbe;
    out.cleanGlobalCycle = read.cleanGlobalCycle;
    out.crosshairProbe = read.crosshairProbe;
    out.reticleSweep = read.reticleSweep;
    out.aimTransformCallers = Callers(read.aimTransformCallers);
    out.aimCopyCallers = Callers(read.aimCopyCallers);
    out.trackedTransformCallers = Callers(read.trackedTransformCallers);
    out.readWatchOffset = read.readWatchOffset;
    out.sceneSampleRva = read.sceneSampleRva ? std::vector<std::uint32_t>{*read.sceneSampleRva}
                                             : std::vector<std::uint32_t>{};

    if (status == legacy::ReadStatus::Absent) {
        return cfg::ImportResult::Absent(std::move(dropped), std::move(poseShaping));
    }
    return cfg::ImportResult::Imported(std::move(dropped), std::move(poseShaping));
}

}  // namespace

cfg::LegacyImport<Config> ConfigLegacyImport() {
    cfg::LegacyImport<Config> import;
    import.run = &RunImport;
    for (const legacy::Key& key : legacy::ReadKeys()) import.keys.push_back({key.section, key.key});
    return import;
}

cfg::ConfigOwnerOptions<Config> ConfigOwnerOptions(std::wstring path) {
    cfg::ConfigOwnerOptions<Config> options;
    options.path = std::move(path);
    options.table = ConfigTable();
    options.import = ConfigLegacyImport();
    options.header.display_name = kGameDisplayName;
    return options;
}

cameraunlock::TrackingMode StartupTrackingMode(const Config& config) {
    const auto mode = cameraunlock::DecodeTrackingMode(config.rotationEnabled, config.positionEnabled);
    if (!mode) throw std::logic_error("RotationEnabled and PositionEnabled are both false, which the table never gives");
    return *mode;
}

std::vector<KeyBinding> KeyBindings(const std::string& list) {
    cameraunlock::input::KeyBindingsParseResult parsed = cameraunlock::input::ParseKeyBindings(list);
    if (!parsed.ok()) throw std::invalid_argument("hotkey list '" + list + "': " + parsed.error);
    return std::move(parsed.bindings);
}

} // namespace NMSHT
