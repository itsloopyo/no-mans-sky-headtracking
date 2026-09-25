// nms_config_oracle <list>: reads each path the list file names, one per line,
// as v0.1.0 did at startup, and prints one record per path (record.h). A path
// with no file behaves as it did in the published build: Mod::Initialize wrote
// the first-run file there and read it back.
//
// nms_config_oracle --first-run <path> writes v0.1.0's first-run file to <path>
// and exits; it is how data/v0.1.0/first-run.ini was made.
//
// config.cpp, config.h, debug_log.h, pch.h and core/constants.h beside this file
// are byte copies of v0.1.0's src/core/config.cpp, src/core/config.h,
// src/core/debug_log.h, src/pch.h and src/core/constants.h; core-c480d8a holds
// the core ADS header v0.1.0 compiled, which core has since removed. What is
// written here is v0.1.0's Mod::Initialize, HotkeyHandler::Start and the five
// [Debug] reads in src/camera/camera_hook.cpp, transcribed.

#include "config.h"
#include "../record.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using nms_differential::Bindings;
using nms_differential::Bits;
using nms_differential::Callers;
using nms_differential::Flag;
using nms_differential::Hex;
using nms_differential::kCtrlShift;
using nms_differential::Record;
using nms_differential::SceneSample;

// v0.1.0 src/camera/camera_hook.cpp, LoadOverride: GetPrivateProfileStringA
// into 4096 bytes, empty leaves the pinned callers, otherwise up to 160 hex
// items split at ',' and spaces, each read with strtoul.
std::optional<std::vector<std::uint32_t>> LoadOverride(const char* key, const std::string& iniPath) {
    char raw[4096] = {};
    GetPrivateProfileStringA("Debug", key, "", raw, sizeof(raw), iniPath.c_str());
    if (raw[0] == '\0') return std::nullopt;
    std::vector<std::uint32_t> rva;
    for (const char* p = raw; *p != '\0';) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == '\0') break;
        rva.push_back(static_cast<std::uint32_t>(std::strtoul(p, nullptr, 16)));
        if (rva.size() >= 160) break;
        while (*p != '\0' && *p != ',') ++p;
    }
    return rva;
}

Record Read(const std::string& path) {
    // Mod::Initialize: write the first-run file where there is none, then read.
    const bool present = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    NMSHT::Config c;
    if (!present && !c.WriteDefault(path)) {
        std::fprintf(stderr, "cannot write the first-run file %s\n", path.c_str());
        std::exit(2);
    }
    if (!c.LoadFromIni(path)) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(2);
    }

    Record record;
    record["status"] = present ? "usable" : "absent";
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
    record["field.adsMode"] = cameraunlock::ads::AdsModeValue(c.adsMode);
    record["field.toggleKey"] = Hex(static_cast<std::uint32_t>(c.toggleKey));
    record["field.cycleModeKey"] = Hex(static_cast<std::uint32_t>(c.cycleModeKey));
    record["field.adsModeKey"] = Hex(static_cast<std::uint32_t>(c.adsModeKey));
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

    // camera_hook.cpp's own reads of the same path.
    record["field.aimTransformCallers"] = Callers(LoadOverride("AimTransformCallers", path));
    record["field.aimCopyCallers"] = Callers(LoadOverride("AimCopyCallers", path));
    record["field.trackedTransformCallers"] = Callers(LoadOverride("TrackedTransformCallers", path));
    record["field.readWatchOffset"] = Hex(GetPrivateProfileIntA("Debug", "ReadWatchOffset", 0, path.c_str()));
    {
        char raw[32] = {};
        GetPrivateProfileStringA("Debug", "SceneSampleRva", "", raw, sizeof(raw), path.c_str());
        record["field.sceneSampleRva"] = SceneSample(
            raw[0] != 0 ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::strtoul(raw, nullptr, 16)))
                        : std::nullopt);
    }

    // Mod::Initialize: tracking on as AutoEnable says, rotation and position
    // or rotation only as [Position] Enabled says, and the ADS mode handed to
    // Ads::Initialize.
    record["startup.enabled"] = Flag(c.autoEnable);
    record["startup.mode"] = c.positionEnabled ? "RotationAndPosition" : "RotationOnly";
    record["startup.adsMode"] = cameraunlock::ads::AdsModeValue(c.adsMode);

    // HotkeyHandler::Start: each action's code, which did not fire while Ctrl
    // and Shift were both held (NavGuarded), and its fixed Ctrl+Shift letter.
    record["hotkey.Toggle"] = Bindings({{0, c.toggleKey}, {kCtrlShift, 'Y'}});
    record["hotkey.CycleTrackingMode"] = Bindings({{0, c.cycleModeKey}, {kCtrlShift, 'G'}});
    record["hotkey.CycleAdsMode"] = Bindings({{0, c.adsModeKey}, {kCtrlShift, 'U'}});
    // CameraHook's caller sweeps polled Ctrl+Shift+J, fixed in code.
    record["hotkey.SweepFreeze"] = Bindings({{kCtrlShift, 'J'}});
    return record;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--first-run") {
        return NMSHT::Config{}.WriteDefault(argv[2]) ? 0 : 1;
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: nms_config_oracle <file listing one path per line>\n"
                             "       nms_config_oracle --first-run <path>\n");
        return 2;
    }
    std::ifstream list(argv[1]);
    if (!list) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::string path;
    while (std::getline(list, path)) {
        if (path.empty()) continue;
        std::cout << nms_differential::Serialize(Read(path));
    }
    return 0;
}
