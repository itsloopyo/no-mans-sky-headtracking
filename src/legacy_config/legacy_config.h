#pragma once

// The pre-canonical HeadTracking.ini reader, frozen. It reads a file exactly as
// Config::LoadFromIni at commit 02b6f3e did, through the same IniReader and
// value_guards calls in the same order, and must never change: the canonical
// config's legacy import runs it on every file an older build wrote.
//
// It differs from that reader in three ways. It fills this frozen Config rather
// than the runtime one. It writes nothing: no first-run file, and its
// diagnostics go to the sink it is handed, which may be null. And the five
// [Debug] keys camera_hook.cpp read on its own at 02b6f3e, each with
// GetPrivateProfileIntA or GetPrivateProfileStringA on the same ANSI path, are
// read here too, into the last five fields, with the same calls and the same
// parsing.

#include <cameraunlock/config/value_guards.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace NMSHT::legacy {

// The runtime Config's fields and defaults at 02b6f3e, then the reads
// camera_hook.cpp made outside it.
struct Config {
    unsigned short udpPort = 4242;

    float yawSensitivity = 1.0f;
    float pitchSensitivity = 1.0f;
    float rollSensitivity = 1.0f;
    bool invertYaw = false;
    bool invertPitch = false;
    bool invertRoll = false;

    float localSmoothing = 0.0f;
    float remoteSmoothing = 0.15f;

    bool positionEnabled = true;
    float posSensitivityX = 1.0f;
    float posSensitivityY = 1.0f;
    float posSensitivityZ = 1.0f;
    float posLimitX = 0.3f;
    float posLimitY = 0.2f;
    float posLimitYDown = 0.2f;
    float posLimitZ = 0.4f;
    float posLimitZBack = 0.1f;
    bool posInvertX = false;
    bool posInvertY = false;
    bool posInvertZ = false;

    bool reticleFollowsAim = true;

    int toggleKey = 0x23;
    int cycleModeKey = 0x21;

    bool autoEnable = true;
    bool logToFile = true;

    bool diagnostics = false;
    bool readWatch = false;
    bool aimCallerSweep = false;
    bool cullCallerSweep = false;
    bool callerCensus = false;
    bool liveCallerOverrides = false;
    bool writeWatch = false;
    bool weaponProbe = false;
    bool cleanGlobalCycle = false;
    bool crosshairProbe = false;
    bool reticleSweep = false;

    // [Debug] AimTransformCallers, AimCopyCallers and TrackedTransformCallers,
    // as camera_hook.cpp's LoadOverride parsed them: nullopt for an empty value,
    // which left the build profile's pinned callers in force, otherwise the
    // listed callers, at most 160, possibly none.
    std::optional<std::vector<std::uint32_t>> aimTransformCallers;
    std::optional<std::vector<std::uint32_t>> aimCopyCallers;
    std::optional<std::vector<std::uint32_t>> trackedTransformCallers;
    // [Debug] ReadWatchOffset, as GetPrivateProfileIntA returned it.
    std::uint32_t readWatchOffset = 0;
    // [Debug] SceneSampleRva: nullopt for an empty value, which left the build
    // profile's RVA in force.
    std::optional<std::uint32_t> sceneSampleRva;
};

enum class ReadStatus {
    Read,
    // No file at the path. The build wrote its first-run file there and read
    // that back, which gives the defaults.
    Absent,
};

// Reads iniPath (an ANSI path, as GetPrivateProfileStringA takes it) into
// config, key by key in the published order. Keys the file lacks keep what
// config held.
ReadStatus Read(const std::string& iniPath, Config& config, cameraunlock::config::LogSink log);

struct Key {
    std::string section;
    std::string key;
};

// Every key Read reads, in the order it reads them. The two retired smoothing
// keys it only warns about are not among them: they set nothing.
std::vector<Key> ReadKeys();

}  // namespace NMSHT::legacy
