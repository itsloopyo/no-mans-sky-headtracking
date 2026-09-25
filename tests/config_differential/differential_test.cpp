// The differential test for the canonical config conversion.
//
// nms_config_differential <path to nms_config_oracle.exe>
//
// Every input is read two ways:
//   oracle - v0.1.0's reader and startup code, the newest published build
//            (nms_config_oracle, built from oracle/);
//   import - the frozen reader in src/legacy_config/ and the startup code
//            that ran on it.
//
// Comparison 1, oracle against import, is what a player sees change that the
// conversion did not cause: commits since v0.1.0 that changed how the file is
// read. Every difference it finds must be one of kReaderChanges below.

#include "record.h"

#include "legacy_config/legacy_config.h"

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
// 02b6f3e (Mod::Initialize and HotkeyHandler::Start), which is v0.1.0's without
// the ADS mode.
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
        NMSHT::legacy::Config ignored;
        NMSHT::legacy::Read((readOnly / "HeadTracking.ini").string(), ignored, nullptr);
        Check(Listing(readOnly) == before, input.name + ": the frozen reader writes nothing");
        if (input.bytes) SetFileAttributesW((readOnly / "HeadTracking.ini").c_str(), FILE_ATTRIBUTE_NORMAL);
    }

    std::printf("Comparison 1 (v0.1.0 against the frozen reader) over %zu inputs:\n", inputs.size());
    for (const ReaderChange& change : kReaderChanges) {
        const auto seen = changesSeen.find(change.id);
        const std::size_t count = seen == changesSeen.end() ? 0 : seen->second.size();
        std::printf("  %s\n    %zu inputs, e.g. %s\n", change.description, count,
                    count == 0 ? "none" : seen->second.front().c_str());
        Check(count > 0, std::string("no input shows the recorded change '") + change.id + "'");
    }

    fs::remove_all(root);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("Config differential tests passed");
    return 0;
}
