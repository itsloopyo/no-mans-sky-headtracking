// The differential test for the canonical config conversion.
//
// nms_config_differential <path to nms_config_oracle.exe>
//
// Every input is read three ways:
//   oracle    - v0.1.0's reader and startup code, the newest published build
//               (nms_config_oracle, built from oracle/);
//   import    - the frozen reader in src/legacy_config/ and the startup code
//               that ran on it;
//   migration - ConfigOwner on a folder holding a copy as HeadTracking.ini:
//               the import, the map into Config, the render into a new
//               CameraUnlock.ini, then the canonical reader and table, and the
//               game's startup code on the result. Every owner reads a scratch
//               Defaults.ini, never the developer's own.
//
// Comparison 1, oracle against import, is what a player sees change that the
// conversion did not cause: commits since v0.1.0 that changed how the file is
// read. Every difference it finds must be one of kReaderChanges below.
//
// Comparison 2, import against migration, is the proof for the migration: no
// difference apart from the approved drops the import records (core's
// data/config-format.json). No default moves in this conversion, so the no-file
// input has no exception of its own.

#include "record.h"

#include "core/config.h"
#include "legacy_config/legacy_config.h"

#include <cameraunlock/config/canonical_ini.h>
#include <cameraunlock/config/config_owner.h>
#include <cameraunlock/config/legacy_import.h>
#include <cameraunlock/config/testing/ini_mutations.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;

using nms_differential::Bindings;
using nms_differential::Bits;
using nms_differential::Callers;
using nms_differential::Flag;
using nms_differential::Hex;
using nms_differential::kCtrlShift;
using nms_differential::Record;
using nms_differential::SceneSample;

namespace {

int g_failures = 0;

void Fail(const std::string& message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    ++g_failures;
}

void Check(bool condition, const std::string& message) {
    if (!condition) Fail(message);
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

struct Input {
    std::string name;
    // No bytes: there is no file.
    std::optional<std::string> bytes;
};

const fs::path kData = fs::path(NMS_SOURCE_DIR) / "tests" / "config_differential" / "data";

// The published inputs: v0.1.0's shipped HeadTracking.ini, the only committed
// version of it up to v0.1.0, and v0.1.0's first-run file. v0.1.0 carried no
// launcher seed. unreleased/shipped.ini is the file after v0.1.0 that the
// launcher seed and the dev deploy have written since, which a tester may hold.
const char* const kDataFiles[] = {
    "v0.1.0/shipped.ini",
    "v0.1.0/first-run.ini",
    "unreleased/shipped.ini",
};

std::string Replace(std::string text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) throw std::logic_error("'" + from + "' is not in the base file");
    return text.replace(at, from.size(), to);
}

// A caller list past each limit LoadOverride had: 161 callers where it kept
// 160, and a value past its 4096-byte buffer, where the third caller is cut
// off and the second reads as far as the cut.
std::vector<std::string> CallerListLimits() {
    std::string many;
    for (int i = 1; i <= 161; ++i) many += (i == 1 ? "" : ", ") + Hex(static_cast<std::uint32_t>(0x1000 + i));
    return {many, "0x1," + std::string(4200, '0') + ",0x2"};
}

// Each key the frozen reader reads, for the corpus: another valid value, and one
// value outside each range the reader refuses or clamps. The hotkey codes are
// read as hex, and their Ctrl+Shift letters were fixed in code, so no key of
// the file folds a chord into them.
std::vector<cfg::testing::MutationKey> MutationKeys() {
    const std::vector<std::string> sensitivity = {"-101", "101"};
    const std::vector<std::string> smoothing = {"-0.5", "1.5"};
    const std::vector<std::string> limit = {"-0.1", "10.5"};
    return {
        {"Network", "UDPPort", "5252", {"1023", "65536"}, false, {}},
        {"Sensitivity", "YawMultiplier", "0.5", sensitivity, false, {}},
        {"Sensitivity", "PitchMultiplier", "0.5", sensitivity, false, {}},
        {"Sensitivity", "RollMultiplier", "0.5", sensitivity, false, {}},
        {"Sensitivity", "InvertYaw", "true", {}, false, {}},
        {"Sensitivity", "InvertPitch", "true", {}, false, {}},
        {"Sensitivity", "InvertRoll", "true", {}, false, {}},
        {"Smoothing", "LocalSmoothing", "0.3", smoothing, false, {}},
        {"Smoothing", "RemoteSmoothing", "0.5", smoothing, false, {}},
        {"Position", "Enabled", "false", {}, false, {}},
        {"Position", "SensitivityX", "0.5", sensitivity, false, {}},
        {"Position", "SensitivityY", "0.5", sensitivity, false, {}},
        {"Position", "SensitivityZ", "0.5", sensitivity, false, {}},
        {"Position", "LimitX", "0.25", limit, false, {}},
        {"Position", "LimitY", "0.15", limit, false, {}},
        {"Position", "LimitYDown", "0.1", limit, false, {}},
        {"Position", "LimitZ", "0.3", limit, false, {}},
        {"Position", "LimitZBack", "0.05", limit, false, {}},
        {"Position", "InvertX", "true", {}, false, {}},
        {"Position", "InvertY", "true", {}, false, {}},
        {"Position", "InvertZ", "true", {}, false, {}},
        {"Reticle", "FollowAim", "false", {}, false, {}},
        // 0xFF is past what GetAsyncKeyState defines; Shift and Ctrl are
        // refused as the chord guard's modifiers.
        {"Hotkeys", "ToggleKey", "0x78", {"0xFF", "0x10"}, true, {}},
        {"Hotkeys", "CycleModeKey", "0x70", {"0xFF", "0x11"}, true, {}},
        {"General", "AutoEnable", "false", {}, false, {}},
        {"General", "LogToFile", "false", {}, false, {}},
        {"Debug", "Diagnostics", "true", {}, false, {}},
        {"Debug", "ReadWatch", "true", {}, false, {}},
        {"Debug", "AimCallerSweep", "true", {}, false, {}},
        {"Debug", "CullCallerSweep", "true", {}, false, {}},
        {"Debug", "CallerCensus", "true", {}, false, {}},
        {"Debug", "LiveCallerOverrides", "true", {}, false, {}},
        {"Debug", "WriteWatch", "true", {}, false, {}},
        {"Debug", "WeaponProbe", "true", {}, false, {}},
        {"Debug", "CleanGlobalCycle", "true", {}, false, {}},
        {"Debug", "CrosshairProbe", "true", {}, false, {}},
        {"Debug", "ReticleSweep", "true", {}, false, {}},
        {"Debug", "AimTransformCallers", "0x1A2B3C, 0x4D5E6F", CallerListLimits(), false, {}},
        {"Debug", "AimCopyCallers", "0x10, 0x20", CallerListLimits(), false, {}},
        {"Debug", "TrackedTransformCallers", "0x30", CallerListLimits(), false, {}},
        {"Debug", "ReadWatchOffset", "24", {}, false, {}},
        {"Debug", "SceneSampleRva", "0x1234", {}, false, {}},
    };
}

std::vector<cfg::LegacyKey> ImportKeys() {
    std::vector<cfg::LegacyKey> keys;
    for (const auto& key : NMSHT::legacy::ReadKeys()) keys.push_back({key.section, key.key});
    return keys;
}

std::vector<Input> Inputs() {
    std::vector<Input> inputs;
    for (const char* file : kDataFiles) inputs.push_back({file, ReadBytes(kData / file)});
    inputs.push_back({"no file", std::nullopt});
    inputs.push_back({"empty file", std::string()});

    const std::string base = ReadBytes(kData / "v0.1.0" / "shipped.ini");
    // The ADS cycle's WritePrivateProfileStringA("ADS", "Mode", ...) rewrote
    // the one line in place.
    inputs.push_back({"v0.1.0 shipped, [ADS] Mode=marker", Replace(base, "Mode=paused", "Mode=marker")});
    inputs.push_back({"v0.1.0 shipped, [ADS] Mode=tracked", Replace(base, "Mode=paused", "Mode=tracked")});
    // A caller list LoadOverride read as a list of none, which the corpus
    // values never produce.
    inputs.push_back({"v0.1.0 shipped, [Debug] AimCopyCallers=,",
                      Replace(base, "[Debug]\n", "[Debug]\nAimCopyCallers=,\nLiveCallerOverrides=true\n")});

    for (auto& mutation : cfg::testing::GenerateIniMutations(base, ImportKeys(), MutationKeys())) {
        inputs.push_back({"corpus: " + mutation.name, std::move(mutation.bytes)});
    }
    return inputs;
}

// The import as the game ran it: the frozen reader, then the startup code at
// 02b6f3e (Mod::Initialize, HotkeyHandler::Start and the caller sweeps' fixed
// Ctrl+Shift+J), which is v0.1.0's without the ADS mode.
Record ImportRecord(const fs::path& path) {
    NMSHT::legacy::Config c;
    const auto status = NMSHT::legacy::Read(path.string(), c, nullptr);
    Record record;
    record["status"] = status == NMSHT::legacy::ReadStatus::Absent ? "absent" : "usable";
    record["field.udpPort"] = std::to_string(c.udpPort);
    record["field.yawSensitivity"] = Bits(c.yawSensitivity);
    record["field.pitchSensitivity"] = Bits(c.pitchSensitivity);
    record["field.rollSensitivity"] = Bits(c.rollSensitivity);
    record["field.invertYaw"] = Flag(c.invertYaw);
    record["field.invertPitch"] = Flag(c.invertPitch);
    record["field.invertRoll"] = Flag(c.invertRoll);
    record["field.localSmoothing"] = Bits(c.localSmoothing);
    record["field.remoteSmoothing"] = Bits(c.remoteSmoothing);
    record["field.positionEnabled"] = Flag(c.positionEnabled);
    record["field.posSensitivityX"] = Bits(c.posSensitivityX);
    record["field.posSensitivityY"] = Bits(c.posSensitivityY);
    record["field.posSensitivityZ"] = Bits(c.posSensitivityZ);
    record["field.posLimitX"] = Bits(c.posLimitX);
    record["field.posLimitY"] = Bits(c.posLimitY);
    record["field.posLimitYDown"] = Bits(c.posLimitYDown);
    record["field.posLimitZ"] = Bits(c.posLimitZ);
    record["field.posLimitZBack"] = Bits(c.posLimitZBack);
    record["field.posInvertX"] = Flag(c.posInvertX);
    record["field.posInvertY"] = Flag(c.posInvertY);
    record["field.posInvertZ"] = Flag(c.posInvertZ);
    record["field.reticleFollowsAim"] = Flag(c.reticleFollowsAim);
    record["field.toggleKey"] = Hex(static_cast<std::uint32_t>(c.toggleKey));
    record["field.cycleModeKey"] = Hex(static_cast<std::uint32_t>(c.cycleModeKey));
    record["field.autoEnable"] = Flag(c.autoEnable);
    record["field.logToFile"] = Flag(c.logToFile);
    record["field.diagnostics"] = Flag(c.diagnostics);
    record["field.readWatch"] = Flag(c.readWatch);
    record["field.aimCallerSweep"] = Flag(c.aimCallerSweep);
    record["field.cullCallerSweep"] = Flag(c.cullCallerSweep);
    record["field.callerCensus"] = Flag(c.callerCensus);
    record["field.liveCallerOverrides"] = Flag(c.liveCallerOverrides);
    record["field.writeWatch"] = Flag(c.writeWatch);
    record["field.weaponProbe"] = Flag(c.weaponProbe);
    record["field.cleanGlobalCycle"] = Flag(c.cleanGlobalCycle);
    record["field.crosshairProbe"] = Flag(c.crosshairProbe);
    record["field.reticleSweep"] = Flag(c.reticleSweep);
    record["field.aimTransformCallers"] = Callers(c.aimTransformCallers);
    record["field.aimCopyCallers"] = Callers(c.aimCopyCallers);
    record["field.trackedTransformCallers"] = Callers(c.trackedTransformCallers);
    record["field.readWatchOffset"] = Hex(c.readWatchOffset);
    record["field.sceneSampleRva"] = SceneSample(c.sceneSampleRva);
    record["startup.enabled"] = Flag(c.autoEnable);
    record["startup.mode"] = c.positionEnabled ? "RotationAndPosition" : "RotationOnly";
    record["hotkey.Toggle"] = Bindings({{0, c.toggleKey}, {kCtrlShift, 'Y'}});
    record["hotkey.CycleTrackingMode"] = Bindings({{0, c.cycleModeKey}, {kCtrlShift, 'G'}});
    record["hotkey.SweepFreeze"] = Bindings({{kCtrlShift, 'J'}});
    return record;
}

std::vector<Record> RunOracle(const fs::path& oracle, const fs::path& list, const fs::path& output) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(output.c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create " + output.string());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = out;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    std::wstring command = L"\"" + oracle.wstring() + L"\" \"" + list.wstring() + L"\"";
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(out);
    if (!started) throw std::runtime_error("cannot start " + oracle.string());
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (code != 0) throw std::runtime_error("the oracle exited with " + std::to_string(code));

    std::vector<Record> records;
    std::istringstream lines(ReadBytes(output));
    std::string line;
    Record record;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "end") {
            records.push_back(std::move(record));
            record.clear();
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) throw std::runtime_error("the oracle wrote '" + line + "'");
        record[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return records;
}

// What changed in how the file is read between v0.1.0 and the frozen reader,
// each with the commit that changed it.
struct ReaderChange {
    const char* id;
    const char* description;
};

const ReaderChange kReaderChanges[] = {
    {"ads-mode",
     "e5fb487 (feat: keep head tracking on through aim down sights) retired the ADS mode cycle: [ADS] Mode and "
     "[Hotkeys] AdsModeKey are no longer read, and Insert / Ctrl+Shift+U no longer cycle the mode"},
};

bool IsAdsName(const std::string& name) {
    return name == "field.adsMode" || name == "field.adsModeKey" || name == "startup.adsMode" ||
           name == "hotkey.CycleAdsMode";
}

// Comparison 1 for one input: the ids of the reader changes that explain every
// difference, or nullopt when a difference has no explanation.
std::optional<std::set<std::string>> ExplainReaderChanges(const Record& oracle, const Record& import) {
    std::set<std::string> found;
    std::set<std::string> names;
    for (const auto& entry : oracle) names.insert(entry.first);
    for (const auto& entry : import) names.insert(entry.first);
    for (const std::string& name : names) {
        const auto o = oracle.find(name);
        const auto i = import.find(name);
        if (o != oracle.end() && i != import.end() && o->second == i->second) continue;
        if (i == import.end() && IsAdsName(name)) {
            found.insert("ads-mode");
            continue;
        }
        return std::nullopt;
    }
    return found;
}

std::string Describe(const Record& record) {
    std::string text;
    for (const auto& entry : record) text += "\n    " + entry.first + " = " + entry.second;
    return text;
}

const char* ModeName(cameraunlock::TrackingMode mode) {
    switch (mode) {
        case cameraunlock::TrackingMode::RotationAndPosition:
            return "RotationAndPosition";
        case cameraunlock::TrackingMode::RotationOnly:
            return "RotationOnly";
        case cameraunlock::TrackingMode::PositionOnly:
            return "PositionOnly";
    }
    throw std::logic_error("not a tracking mode");
}

std::string ListBindings(const std::string& list) {
    std::vector<std::pair<unsigned, int>> items;
    for (const auto& binding : NMSHT::KeyBindings(list)) {
        items.push_back({static_cast<unsigned>(binding.modifiers), binding.vk});
    }
    return Bindings(items);
}

std::optional<std::vector<std::uint32_t>> RuntimeCallers(const std::vector<std::uint32_t>& callers) {
    if (callers.empty()) return std::nullopt;
    return callers;
}

// A Config through the game's startup code (Mod::Initialize, HotkeyHandler::Start
// and CameraHook::Install), in the names ImportRecord uses. The pose goes to the
// tracking pipeline with no sensitivity and no inversion, and the crosshair
// always follows the aim, so those fields read as what the pipeline applies.
Record ConfigRecord(const NMSHT::Config& c) {
    Record record;
    record["field.udpPort"] = std::to_string(c.udpPort);
    for (const char* name : {"field.yawSensitivity", "field.pitchSensitivity", "field.rollSensitivity",
                             "field.posSensitivityX", "field.posSensitivityY", "field.posSensitivityZ"}) {
        record[name] = Bits(1.0f);
    }
    for (const char* name : {"field.invertYaw", "field.invertPitch", "field.invertRoll", "field.posInvertX",
                             "field.posInvertY", "field.posInvertZ"}) {
        record[name] = Flag(false);
    }
    record["field.localSmoothing"] = Bits(c.localSmoothing);
    record["field.remoteSmoothing"] = Bits(c.remoteSmoothing);
    record["field.posLimitX"] = Bits(c.posLimitX);
    record["field.posLimitY"] = Bits(c.posLimitY);
    record["field.posLimitYDown"] = Bits(c.posLimitYDown);
    record["field.posLimitZ"] = Bits(c.posLimitZ);
    record["field.posLimitZBack"] = Bits(c.posLimitZBack);
    record["field.reticleFollowsAim"] = Flag(true);
    record["field.logToFile"] = Flag(c.writeLog);
    record["field.diagnostics"] = Flag(c.diagnostics);
    record["field.readWatch"] = Flag(c.readWatch);
    record["field.aimCallerSweep"] = Flag(c.aimCallerSweep);
    record["field.cullCallerSweep"] = Flag(c.cullCallerSweep);
    record["field.callerCensus"] = Flag(c.callerCensus);
    record["field.liveCallerOverrides"] = Flag(c.liveCallerOverrides);
    record["field.writeWatch"] = Flag(c.writeWatch);
    record["field.weaponProbe"] = Flag(c.weaponProbe);
    record["field.cleanGlobalCycle"] = Flag(c.cleanGlobalCycle);
    record["field.crosshairProbe"] = Flag(c.crosshairProbe);
    record["field.reticleSweep"] = Flag(c.reticleSweep);
    record["field.aimTransformCallers"] = Callers(RuntimeCallers(c.aimTransformCallers));
    record["field.aimCopyCallers"] = Callers(RuntimeCallers(c.aimCopyCallers));
    record["field.trackedTransformCallers"] = Callers(RuntimeCallers(c.trackedTransformCallers));
    record["field.readWatchOffset"] = Hex(c.readWatchOffset);
    if (c.sceneSampleRva.size() > 1) throw std::logic_error("a migrated SceneSampleRva lists more than one address");
    record["field.sceneSampleRva"] =
        SceneSample(c.sceneSampleRva.empty() ? std::nullopt : std::optional<std::uint32_t>(c.sceneSampleRva.front()));
    record["startup.enabled"] = Flag(c.enableOnStartup);
    record["startup.mode"] = ModeName(NMSHT::StartupTrackingMode(c));
    record["hotkey.Toggle"] = ListBindings(c.toggleKey);
    record["hotkey.CycleTrackingMode"] = ListBindings(c.cycleTrackingModeKey);
    record["hotkey.SweepFreeze"] = ListBindings(c.sweepFreezeKey);
    return record;
}

bool Dropped(const cfg::ImportResult& import, cfg::DropRule rule, const std::string& section, const std::string& key) {
    for (const auto& dropped : import.dropped) {
        if (dropped.rule == rule && dropped.section == section && dropped.key == key) return true;
    }
    return false;
}

// The pose-shaping settings the import reads, by record name.
struct PoseShapingKey {
    const char* field;
    const char* section;
    const char* key;
};

const PoseShapingKey kPoseShaping[] = {
    {"field.yawSensitivity", "Sensitivity", "YawMultiplier"},
    {"field.pitchSensitivity", "Sensitivity", "PitchMultiplier"},
    {"field.rollSensitivity", "Sensitivity", "RollMultiplier"},
    {"field.invertYaw", "Sensitivity", "InvertYaw"},
    {"field.invertPitch", "Sensitivity", "InvertPitch"},
    {"field.invertRoll", "Sensitivity", "InvertRoll"},
    {"field.posSensitivityX", "Position", "SensitivityX"},
    {"field.posSensitivityY", "Position", "SensitivityY"},
    {"field.posSensitivityZ", "Position", "SensitivityZ"},
    {"field.posInvertX", "Position", "InvertX"},
    {"field.posInvertY", "Position", "InvertY"},
    {"field.posInvertZ", "Position", "InvertZ"},
};

// The import listed the key in pose_shaping as a value the player changed, and
// dropped it.
bool PoseShapingDropped(const cfg::ImportResult& import, const PoseShapingKey& key) {
    for (const auto& value : import.pose_shaping) {
        if (value.section == key.section && value.key == key.key) {
            return !value.folded && Dropped(import, cfg::DropRule::PoseShaping, key.section, key.key);
        }
    }
    return false;
}

// Comparison 2 for one input: empty when every difference between the import
// and the migration is one core's data/config-format.json approves, and the
// import recorded it; otherwise what is left. `explained` gains the id of each
// allowance a difference used.
std::vector<std::string> UnexplainedMigrationDifferences(const Record& import, const cfg::ImportResult& result,
                                                         const Record& migration, std::set<std::string>& explained) {
    std::vector<std::string> left;
    if (import.at("status") != migration.at("status")) {
        left.push_back("status " + import.at("status") + " -> " + migration.at("status"));
        return left;
    }
    std::set<std::string> names;
    for (const auto& entry : import) names.insert(entry.first);
    for (const auto& entry : migration) names.insert(entry.first);
    for (const std::string& name : names) {
        const auto i = import.find(name);
        const auto m = migration.find(name);
        if (i != import.end() && m != migration.end() && i->second == m->second) continue;
        // The raw fields that decide the startup state and the bound keys are
        // compared as that state (startup.*) and those bindings (hotkey.*).
        if (name == "field.positionEnabled" || name == "field.autoEnable" || name == "field.toggleKey" ||
            name == "field.cycleModeKey") {
            continue;
        }
        // Approved change `pose_shaping`: a value the player moved off the
        // shipped identity.
        const auto shaping = std::find_if(std::begin(kPoseShaping), std::end(kPoseShaping),
                                          [&name](const PoseShapingKey& key) { return name == key.field; });
        if (shaping != std::end(kPoseShaping) && PoseShapingDropped(result, *shaping)) {
            explained.insert("pose-shaping");
            continue;
        }
        // Approved change `reticle`: the crosshair follows the aim whatever
        // [Reticle] FollowAim said.
        if (name == "field.reticleFollowsAim" && i != import.end() && i->second == "0" &&
            Dropped(result, cfg::DropRule::Reticle, "Reticle", "FollowAim")) {
            explained.insert("reticle");
            continue;
        }
        // A caller list of none is carried as the one caller 0x0, which no
        // call returns to (offset 0 of NMS.exe is its header), so the override
        // still serves no caller.
        if ((name == "field.aimTransformCallers" || name == "field.aimCopyCallers" ||
             name == "field.trackedTransformCallers") &&
            i != import.end() && m != migration.end() && i->second == "none" && m->second == Hex(0)) {
            explained.insert("caller-none");
            continue;
        }
        left.push_back(name + ": " + (i == import.end() ? "(none)" : i->second) + " -> " +
                       (m == migration.end() ? "(none)" : m->second));
    }
    return left;
}

cfg::LegacyInput InputFor(const fs::path& path) { return cfg::LegacyInput{path.wstring(), path.string(), false}; }

std::string RenderedDefaults() {
    cfg::RenderHeader header;
    header.display_name = NMSHT::kGameDisplayName;
    return cfg::RenderCanonicalFresh(NMSHT::ConfigTable(), header);
}

cfg::ConfigOwnerOptions<NMSHT::Config> Options(const fs::path& folder, const fs::path& defaults) {
    return NMSHT::ConfigOwnerOptions(folder, cfg::DefaultsFile::At(defaults.wstring()));
}

// A file's bytes, last write time and attributes, which no load may change.
struct FileState {
    std::string bytes;
    unsigned long long written = 0;
    DWORD attributes = 0;
    bool operator==(const FileState& other) const {
        return bytes == other.bytes && written == other.written && attributes == other.attributes;
    }
};

std::optional<FileState> StateOf(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return std::nullopt;
        throw std::runtime_error("cannot read the attributes of " + path.string());
    }
    FileState state;
    state.bytes = ReadBytes(path);
    state.written = (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                    data.ftLastWriteTime.dwLowDateTime;
    state.attributes = data.dwFileAttributes;
    return state;
}

std::set<std::string> Names(const fs::path& dir) {
    std::set<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) names.insert(entry.path().filename().string());
    return names;
}

bool LogSays(const std::vector<std::string>& log, const std::string& text) {
    for (const std::string& line : log) {
        if (line.find(text) != std::string::npos) return true;
    }
    return false;
}

// Runs the owner on `folder`, which holds the input as HeadTracking.ini (or
// nothing), with Defaults.ini at `defaults`, checks what the design asks of the
// migration beyond comparison 2, and returns the migration's record. A file it
// creates by migrating goes into `migratedFiles`.
Record Migrate(const Input& input, const fs::path& folder, const fs::path& defaults,
               std::set<std::string>& migratedFiles) {
    const fs::path legacy = folder / "HeadTracking.ini";
    const fs::path path = folder / "CameraUnlock.ini";
    const std::string label = input.name + " (" + folder.filename().string() + "): ";
    const std::optional<FileState> legacyBefore = StateOf(legacy);
    const std::set<std::string> both{"CameraUnlock.ini", "HeadTracking.ini"};

    const cfg::ConfigLoadResult<NMSHT::Config> loaded = cfg::ConfigOwner<NMSHT::Config>(Options(folder, defaults)).Load();
    Check(StateOf(legacy) == legacyBefore, label + "the load leaves the legacy file's bytes, write time and attributes");
    Record record;
    switch (loaded.status) {
        case cfg::ConfigLoadStatus::Created:
            record = ConfigRecord(loaded.config);
            record["status"] = "absent";
            Check(ReadBytes(path) == RenderedDefaults(), label + "a first launch writes the committed file's bytes");
            Check(Names(folder) == std::set<std::string>{"CameraUnlock.ini"},
                  label + "a first launch creates CameraUnlock.ini and nothing else");
            break;
        case cfg::ConfigLoadStatus::Migrated: {
            record = ConfigRecord(loaded.config);
            record["status"] = "usable";
            Check(Names(folder) == both, label + "the folder holds the legacy file and CameraUnlock.ini and nothing else");
            const std::string migrated = ReadBytes(path);
            Check(cfg::HasCanonicalStamp(migrated), label + "the migrated file carries the stamp");
            const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(migrated);
            NMSHT::Config reread = NMSHT::ConfigTable().defaults();
            Check(doc.IsReadable() && doc.diagnostics.empty() &&
                      cfg::ApplyCanonical(doc, NMSHT::ConfigTable(), reread).diagnostics.empty(),
                  label + "the migrated file reads without a diagnostic");
            migratedFiles.insert(migrated);
            break;
        }
        default:
            Fail(label + "the migration loaded " + cfg::ConfigLoadStatusName(loaded.status) +
                 (loaded.reason.empty() ? std::string() : ": " + loaded.reason));
            return Record{{"status", cfg::ConfigLoadStatusName(loaded.status)}};
    }

    // The next start reads CameraUnlock.ini, imports nothing and writes nothing.
    const std::optional<FileState> created = StateOf(path);
    const cfg::ConfigLoadResult<NMSHT::Config> relaunched =
        cfg::ConfigOwner<NMSHT::Config>(Options(folder, defaults)).Load();
    Check(relaunched.status == cfg::ConfigLoadStatus::Canonical, label + "the next start reads CameraUnlock.ini");
    Record again = ConfigRecord(relaunched.config);
    again["status"] = record.at("status");
    Check(again == record, label + "the next start gives the same settings");
    Check(StateOf(path) == created && StateOf(legacy) == legacyBefore, label + "the next start changes neither file");
    Check(!legacyBefore || LogSays(relaunched.log, "is left as it was and is not read"),
          label + "the next start logs that the legacy file is not read");
    return record;
}

// A Defaults.ini holding a value other than the built-in one on every global
// row the table binds. The tracking mode pair names position only, since both
// false is not a mode.
const char* const kSkewedDefaults =
    "[CameraUnlock]\r\nConfigFormat=1\r\n\r\n"
    "[Network]\r\nUdpPort=5252\r\n\r\n"
    "[General]\r\nEnableOnStartup=false\r\nRotationEnabled=false\r\n\r\n"
    "[Smoothing]\r\nLocalSmoothing=0.5\r\nRemoteSmoothing=0.5\r\n\r\n"
    "[Position]\r\nPositionEnabled=true\r\nPositionLimitX=0.5\r\nPositionLimitY=0.5\r\n"
    "PositionLimitYDown=0.5\r\nPositionLimitZ=0.5\r\nPositionLimitZBack=0.5\r\n\r\n"
    "[Hotkeys]\r\nToggleKey=F8\r\nCycleTrackingModeKey=F9\r\n";

// Beside this executable, where the canonical config lint (lint-migrated.mjs)
// reads the migrated files after this test.
fs::path MigratedFolder() {
    std::vector<wchar_t> exe(MAX_PATH);
    const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (length == 0 || length >= exe.size()) throw std::runtime_error("cannot find this executable's path");
    return fs::path(std::wstring(exe.data(), length)).parent_path() / "migrated";
}

std::vector<std::pair<std::string, std::string>> Listing(const fs::path& dir) {
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& entry : fs::directory_iterator(dir)) {
        files.push_back({entry.path().filename().string(), ReadBytes(entry.path())});
    }
    std::sort(files.begin(), files.end());
    return files;
}

fs::path MakeTempRoot() {
    wchar_t temp[MAX_PATH + 1];
    if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
    const fs::path root = fs::path(temp) / ("nms-config-differential-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: nms_config_differential <nms_config_oracle.exe>\n");
        return 2;
    }
    const fs::path oracle = argv[1];
    const fs::path root = MakeTempRoot();
    const std::vector<Input> inputs = Inputs();

    std::string list;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const fs::path dir = root / std::to_string(i);
        fs::create_directories(dir / "oracle");
        fs::create_directories(dir / "import");
        if (inputs[i].bytes) {
            WriteBytes(dir / "oracle" / "HeadTracking.ini", *inputs[i].bytes);
            WriteBytes(dir / "import" / "HeadTracking.ini", *inputs[i].bytes);
        }
        list += (dir / "oracle" / "HeadTracking.ini").string() + "\n";
    }
    WriteBytes(root / "oracle-inputs.txt", list);
    const std::vector<Record> published = RunOracle(oracle, root / "oracle-inputs.txt", root / "oracle-records.txt");
    Check(published.size() == inputs.size(), "the oracle read " + std::to_string(published.size()) + " of " +
                                                 std::to_string(inputs.size()) + " inputs");

    std::map<std::string, std::vector<std::string>> changesSeen;
    std::size_t migrated = 0;
    std::map<std::string, std::vector<std::string>> allowancesSeen;
    std::set<std::string> migratedFiles;
    for (std::size_t i = 0; i < inputs.size() && i < published.size(); ++i) {
        const Input& input = inputs[i];
        const fs::path dir = root / std::to_string(i);
        const Record import = ImportRecord(dir / "import" / "HeadTracking.ini");
        const auto explained = ExplainReaderChanges(published[i], import);
        if (!explained) {
            Fail("comparison 1, " + input.name + ": the frozen reader differs from v0.1.0 in a way no commit explains" +
                 "\n  oracle:" + Describe(published[i]) + "\n  import:" + Describe(import));
            continue;
        }
        for (const std::string& id : *explained) changesSeen[id].push_back(input.name);

        // The frozen reader on a read-only copy leaves the folder as it was.
        const fs::path readOnly = dir / "read-only";
        fs::create_directories(readOnly);
        if (input.bytes) {
            WriteBytes(readOnly / "HeadTracking.ini", *input.bytes);
            SetFileAttributesW((readOnly / "HeadTracking.ini").c_str(), FILE_ATTRIBUTE_READONLY);
        }
        const auto before = Listing(readOnly);
        NMSHT::Config mapped;
        const cfg::ImportResult result =
            NMSHT::ConfigLegacyImport().run(InputFor(readOnly / "HeadTracking.ini"), mapped);
        Check(Listing(readOnly) == before, input.name + ": the import writes nothing");
        if (input.bytes) SetFileAttributesW((readOnly / "HeadTracking.ini").c_str(), FILE_ATTRIBUTE_NORMAL);

        // Comparison 2 and the migration's own checks. Each input migrates three
        // times: with Defaults.ini at the built-in values, from a read-only
        // legacy file, and with a Defaults.ini that differs on every global row.
        // All three must give the same settings, since the migration writes a
        // value wherever the imported one is not what default gives.
        fs::create_directories(dir / "migrate");
        if (input.bytes) WriteBytes(dir / "migrate" / "HeadTracking.ini", *input.bytes);
        const Record migration = Migrate(input, dir / "migrate", dir / "defaults" / "Defaults.ini", migratedFiles);
        if (migration.at("status") == "usable") ++migrated;
        std::set<std::string> allowances;
        for (const std::string& difference : UnexplainedMigrationDifferences(import, result, migration, allowances)) {
            Fail("comparison 2, " + input.name + ": " + difference);
        }
        for (const std::string& id : allowances) allowancesSeen[id].push_back(input.name);

        const fs::path readOnlyMigrate = dir / "migrate-read-only";
        fs::create_directories(readOnlyMigrate);
        if (input.bytes) {
            WriteBytes(readOnlyMigrate / "HeadTracking.ini", *input.bytes);
            SetFileAttributesW((readOnlyMigrate / "HeadTracking.ini").c_str(), FILE_ATTRIBUTE_READONLY);
        }
        Check(Migrate(input, readOnlyMigrate, dir / "defaults" / "Defaults.ini", migratedFiles) == migration,
              input.name + ": a read-only legacy file migrates as a writable one does");
        if (input.bytes) SetFileAttributesW((readOnlyMigrate / "HeadTracking.ini").c_str(), FILE_ATTRIBUTE_NORMAL);

        // With no legacy file the settings are Defaults.ini's own, so only an
        // input with a file is held to the import there.
        if (input.bytes) {
            const fs::path skewed = dir / "migrate-skewed";
            WriteBytes(skewed / "HeadTracking.ini", *input.bytes);
            WriteBytes(dir / "skewed" / "Defaults.ini", kSkewedDefaults);
            Check(Migrate(input, skewed, dir / "skewed" / "Defaults.ini", migratedFiles) == migration,
                  input.name + ": the migration gives the same settings over a Defaults.ini that differs everywhere");
        }
    }

    std::printf("Comparison 1 (v0.1.0 against the frozen reader) over %zu inputs:\n", inputs.size());
    for (const ReaderChange& change : kReaderChanges) {
        const auto seen = changesSeen.find(change.id);
        const std::size_t count = seen == changesSeen.end() ? 0 : seen->second.size();
        std::printf("  %s\n    %zu inputs, e.g. %s\n", change.description, count,
                    count == 0 ? "none" : seen->second.front().c_str());
        Check(count > 0, std::string("no input shows the recorded change '") + change.id + "'");
    }

    std::printf("Comparison 2 (the frozen reader against the migration): %zu inputs migrated\n", migrated);
    const std::pair<const char*, const char*> kAllowances[] = {
        {"pose-shaping", "approved change pose_shaping: a sensitivity or inversion off the shipped identity, dropped"},
        {"reticle", "approved change reticle: [Reticle] FollowAim=false, dropped; the crosshair follows the aim"},
        {"caller-none", "a caller list of none, carried as 0x0, which no call returns to"},
    };
    for (const auto& [id, description] : kAllowances) {
        const auto seen = allowancesSeen.find(id);
        const std::size_t count = seen == allowancesSeen.end() ? 0 : seen->second.size();
        std::printf("  %s\n    %zu inputs, e.g. %s\n", description, count,
                    count == 0 ? "none" : seen->second.front().c_str());
        Check(count > 0, std::string("no input exercises the allowance '") + id + "'");
    }

    // The shipped files carry no pose shaping away from the identity, so the
    // conversion folds nothing into the mod's axis code.
    for (const char* file : kDataFiles) {
        NMSHT::Config mapped;
        const fs::path path = root / "shipped" / file;
        WriteBytes(path, ReadBytes(kData / file));
        const cfg::ImportResult result = NMSHT::ConfigLegacyImport().run(InputFor(path), mapped);
        Check(result.pose_shaping.size() == std::size(kPoseShaping),
              std::string(file) + ": the import lists every pose-shaping setting it reads");
        for (const auto& value : result.pose_shaping) {
            Check(value.folded, std::string(file) + ": [" + value.section + "] " + value.key + "=" + value.value +
                                    " is the shipped " + value.shipped);
        }
    }

    // A player who installed a published build and changed nothing gets the
    // committed file, with Defaults.ini at the built-in values: every untouched
    // global row migrates as default.
    const std::string committed = ReadBytes(fs::path(NMS_SOURCE_DIR) / "HeadTracking.ini");
    Check(committed == RenderedDefaults(), "HeadTracking.ini, the committed CameraUnlock.ini, is the table's fresh render");
    for (const char* file : kDataFiles) {
        const fs::path dir = root / "upgrade" / fs::path(file).parent_path() / fs::path(file).stem();
        WriteBytes(dir / "HeadTracking.ini", ReadBytes(kData / file));
        cfg::ConfigOwner<NMSHT::Config> owner(Options(dir, root / "upgrade-global" / "Defaults.ini"));
        Check(owner.Load().status == cfg::ConfigLoadStatus::Migrated, std::string(file) + " migrates");
        Check(ReadBytes(dir / "CameraUnlock.ini") == committed, std::string(file) + " migrates to the committed file");
        Check(ReadBytes(dir / "HeadTracking.ini") == ReadBytes(kData / file), std::string(file) + " is left as it was");
    }

    // Core's canonical config lint runs over these next (lint-migrated.mjs).
    const fs::path lintFolder = MigratedFolder();
    fs::remove_all(lintFolder);
    fs::create_directories(lintFolder);
    std::size_t n = 0;
    for (const std::string& file : migratedFiles) WriteBytes(lintFolder / (std::to_string(n++) + ".ini"), file);
    std::printf("%zu distinct migrated files written to %s\n", migratedFiles.size(), lintFolder.string().c_str());

    fs::remove_all(root);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("Config differential tests passed");
    return 0;
}
