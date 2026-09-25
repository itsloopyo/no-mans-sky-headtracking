#include "legacy_config.h"

#include <cameraunlock/config/ini_reader.h>
#include <cameraunlock/protocol/port_utils.h>

#include <windows.h>

#include <cctype>
#include <cstdlib>

namespace NMSHT::legacy {

namespace {

namespace guards = cameraunlock::config;

float ReadFloat(const cameraunlock::IniReader& ini, const char* section, const char* key, float fallback,
                float lo, float hi, guards::LogSink log) {
    return guards::ReadFloatChecked(ini, section, key, fallback, lo, hi, log);
}

constexpr float kMaxSens = guards::kMaxSensitivity;
constexpr float kMaxLimit = guards::kMaxPositionLimit;

bool ReadBool(const cameraunlock::IniReader& ini, const char* section, const char* key, bool fallback,
              guards::LogSink log) {
    const std::string raw = guards::ReadRawValue(ini, section, key);
    if (raw.empty()) return fallback;

    std::string lowered = raw;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") return true;
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") return false;

    if (log != nullptr) {
        log("config: [%s] %s=%s is not true or false, so the default %s is used instead.", section, key,
            raw.c_str(), fallback ? "true" : "false");
    }
    return fallback;
}

int ReadHotkey(const cameraunlock::IniReader& ini, const char* key, int fallback, guards::LogSink log) {
    const std::string raw = guards::ReadRawValue(ini, "Hotkeys", key);
    if (raw.empty()) return fallback;

    const char* text = raw.c_str();
    if (raw.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;

    char* end = nullptr;
    const long vk = std::strtol(text, &end, 16);
    if (end != nullptr && *end == '\0' && end != text && vk >= 0 && vk <= 0xFF &&
        guards::IsBindableVirtualKey(static_cast<int>(vk))) {
        return static_cast<int>(vk);
    }

    if (log != nullptr) {
        log("config: [Hotkeys] %s=%s is not a virtual key code that can be polled (write it "
            "as hex, GetAsyncKeyState defines 0x01-0xFE, and Ctrl/Shift/Alt are reserved for "
            "the chord bindings), using 0x%X",
            key, raw.c_str(), fallback);
    }
    return fallback;
}

// camera_hook.cpp's LoadOverride at 02b6f3e, into a list rather than its
// double-buffered CallerSet.
constexpr int kMaxOverrideCallers = 160;

std::optional<std::vector<std::uint32_t>> ReadCallerOverride(const char* key, const std::string& iniPath) {
    char raw[4096] = {};
    GetPrivateProfileStringA("Debug", key, "", raw, sizeof(raw), iniPath.c_str());
    if (raw[0] == '\0') return std::nullopt;
    std::vector<std::uint32_t> callers;
    for (const char* p = raw; *p != '\0';) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p == '\0') break;
        callers.push_back(static_cast<std::uint32_t>(std::strtoul(p, nullptr, 16)));
        if (static_cast<int>(callers.size()) >= kMaxOverrideCallers) break;
        while (*p != '\0' && *p != ',') ++p;
    }
    return callers;
}

}  // namespace

ReadStatus Read(const std::string& iniPath, Config& c, guards::LogSink log) {
    // The five [Debug] reads camera_hook.cpp made, on the same path whether or
    // not the file exists: GetPrivateProfile* gives an absent file the default.
    const auto readOutside = [&c, &iniPath]() {
        c.aimTransformCallers = ReadCallerOverride("AimTransformCallers", iniPath);
        c.aimCopyCallers = ReadCallerOverride("AimCopyCallers", iniPath);
        c.trackedTransformCallers = ReadCallerOverride("TrackedTransformCallers", iniPath);
        c.readWatchOffset = GetPrivateProfileIntA("Debug", "ReadWatchOffset", 0, iniPath.c_str());
        char raw[32] = {};
        GetPrivateProfileStringA("Debug", "SceneSampleRva", "", raw, sizeof(raw), iniPath.c_str());
        c.sceneSampleRva = raw[0] != 0 ? std::optional<std::uint32_t>(
                                             static_cast<std::uint32_t>(std::strtoul(raw, nullptr, 16)))
                                       : std::nullopt;
    };

    cameraunlock::IniReader r;
    if (!r.Open(iniPath)) {
        readOutside();
        return ReadStatus::Absent;
    }

    const int rawPort = r.ReadInt("Network", "UDPPort", c.udpPort);
    bool portValid = false;
    const uint16_t port = cameraunlock::NormalizeUdpPort(rawPort, c.udpPort, portValid);
    if (!portValid && log != nullptr) {
        log("config: [Network] UDPPort=%d is outside 1024-65535, using %u", rawPort, port);
    }
    c.udpPort = port;

    c.yawSensitivity = ReadFloat(r, "Sensitivity", "YawMultiplier", c.yawSensitivity, -kMaxSens, kMaxSens, log);
    c.pitchSensitivity =
        ReadFloat(r, "Sensitivity", "PitchMultiplier", c.pitchSensitivity, -kMaxSens, kMaxSens, log);
    c.rollSensitivity = ReadFloat(r, "Sensitivity", "RollMultiplier", c.rollSensitivity, -kMaxSens, kMaxSens, log);
    c.invertYaw = ReadBool(r, "Sensitivity", "InvertYaw", c.invertYaw, log);
    c.invertPitch = ReadBool(r, "Sensitivity", "InvertPitch", c.invertPitch, log);
    c.invertRoll = ReadBool(r, "Sensitivity", "InvertRoll", c.invertRoll, log);

    c.localSmoothing = ReadFloat(r, "Smoothing", "LocalSmoothing", c.localSmoothing, 0.0f, 1.0f, log);
    c.remoteSmoothing = ReadFloat(r, "Smoothing", "RemoteSmoothing", c.remoteSmoothing, 0.0f, 1.0f, log);
    guards::WarnRetiredSmoothingKey(r, "Smoothing", "Factor", log);
    guards::WarnRetiredSmoothingKey(r, "Position", "Smoothing", log);

    c.positionEnabled = ReadBool(r, "Position", "Enabled", c.positionEnabled, log);
    c.posSensitivityX = ReadFloat(r, "Position", "SensitivityX", c.posSensitivityX, -kMaxSens, kMaxSens, log);
    c.posSensitivityY = ReadFloat(r, "Position", "SensitivityY", c.posSensitivityY, -kMaxSens, kMaxSens, log);
    c.posSensitivityZ = ReadFloat(r, "Position", "SensitivityZ", c.posSensitivityZ, -kMaxSens, kMaxSens, log);
    c.posLimitX = ReadFloat(r, "Position", "LimitX", c.posLimitX, 0.0f, kMaxLimit, log);
    c.posLimitY = ReadFloat(r, "Position", "LimitY", c.posLimitY, 0.0f, kMaxLimit, log);
    // Falls back to whatever LimitY resolved to.
    c.posLimitYDown = ReadFloat(r, "Position", "LimitYDown", c.posLimitY, 0.0f, kMaxLimit, log);
    c.posLimitZ = ReadFloat(r, "Position", "LimitZ", c.posLimitZ, 0.0f, kMaxLimit, log);
    c.posLimitZBack = ReadFloat(r, "Position", "LimitZBack", c.posLimitZBack, 0.0f, kMaxLimit, log);
    c.posInvertX = ReadBool(r, "Position", "InvertX", c.posInvertX, log);
    c.posInvertY = ReadBool(r, "Position", "InvertY", c.posInvertY, log);
    c.posInvertZ = ReadBool(r, "Position", "InvertZ", c.posInvertZ, log);

    c.reticleFollowsAim = ReadBool(r, "Reticle", "FollowAim", c.reticleFollowsAim, log);

    c.toggleKey = ReadHotkey(r, "ToggleKey", c.toggleKey, log);
    c.cycleModeKey = ReadHotkey(r, "CycleModeKey", c.cycleModeKey, log);

    c.autoEnable = ReadBool(r, "General", "AutoEnable", c.autoEnable, log);
    c.logToFile = ReadBool(r, "General", "LogToFile", c.logToFile, log);

    c.diagnostics = ReadBool(r, "Debug", "Diagnostics", c.diagnostics, log);
    c.readWatch = ReadBool(r, "Debug", "ReadWatch", c.readWatch, log);
    c.aimCallerSweep = ReadBool(r, "Debug", "AimCallerSweep", c.aimCallerSweep, log);
    c.cullCallerSweep = ReadBool(r, "Debug", "CullCallerSweep", c.cullCallerSweep, log);
    c.callerCensus = ReadBool(r, "Debug", "CallerCensus", c.callerCensus, log);
    c.liveCallerOverrides = ReadBool(r, "Debug", "LiveCallerOverrides", c.liveCallerOverrides, log);
    c.writeWatch = ReadBool(r, "Debug", "WriteWatch", c.writeWatch, log);
    c.weaponProbe = ReadBool(r, "Debug", "WeaponProbe", c.weaponProbe, log);
    c.cleanGlobalCycle = ReadBool(r, "Debug", "CleanGlobalCycle", c.cleanGlobalCycle, log);
    c.crosshairProbe = ReadBool(r, "Debug", "CrosshairProbe", c.crosshairProbe, log);
    c.reticleSweep = ReadBool(r, "Debug", "ReticleSweep", c.reticleSweep, log);

    readOutside();
    return ReadStatus::Read;
}

std::vector<Key> ReadKeys() {
    return {
        {"Network", "UDPPort"},
        {"Sensitivity", "YawMultiplier"},
        {"Sensitivity", "PitchMultiplier"},
        {"Sensitivity", "RollMultiplier"},
        {"Sensitivity", "InvertYaw"},
        {"Sensitivity", "InvertPitch"},
        {"Sensitivity", "InvertRoll"},
        {"Smoothing", "LocalSmoothing"},
        {"Smoothing", "RemoteSmoothing"},
        {"Position", "Enabled"},
        {"Position", "SensitivityX"},
        {"Position", "SensitivityY"},
        {"Position", "SensitivityZ"},
        {"Position", "LimitX"},
        {"Position", "LimitY"},
        {"Position", "LimitYDown"},
        {"Position", "LimitZ"},
        {"Position", "LimitZBack"},
        {"Position", "InvertX"},
        {"Position", "InvertY"},
        {"Position", "InvertZ"},
        {"Reticle", "FollowAim"},
        {"Hotkeys", "ToggleKey"},
        {"Hotkeys", "CycleModeKey"},
        {"General", "AutoEnable"},
        {"General", "LogToFile"},
        {"Debug", "Diagnostics"},
        {"Debug", "ReadWatch"},
        {"Debug", "AimCallerSweep"},
        {"Debug", "CullCallerSweep"},
        {"Debug", "CallerCensus"},
        {"Debug", "LiveCallerOverrides"},
        {"Debug", "WriteWatch"},
        {"Debug", "WeaponProbe"},
        {"Debug", "CleanGlobalCycle"},
        {"Debug", "CrosshairProbe"},
        {"Debug", "ReticleSweep"},
        {"Debug", "AimTransformCallers"},
        {"Debug", "AimCopyCallers"},
        {"Debug", "TrackedTransformCallers"},
        {"Debug", "ReadWatchOffset"},
        {"Debug", "SceneSampleRva"},
    };
}

}  // namespace NMSHT::legacy
