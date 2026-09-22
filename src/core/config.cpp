#include "pch.h"
#include "config.h"

#include <cameraunlock/config/ini_reader.h>
#include <cameraunlock/config/value_guards.h>
#include <cameraunlock/protocol/port_utils.h>

#include <cctype>
#include <cstdlib>

#include "debug_log.h"

namespace NMSHT {

namespace {

namespace guards = cameraunlock::config;

// Every number in the file goes through guards::ReadFloatChecked rather than
// IniReader::ReadFloat. ReadFloat is a strtod PREFIX parse over text that still
// carries its inline comment, so "LocalSmoothing=0,15" - a European decimal
// comma, and the reader pins the C locale, so this is the expected user error -
// yields 0.0, sits inside the valid range and passes every check with nothing in
// the log. ReadFloatChecked strips the comment, requires the whole token to
// parse, refuses NaN and Inf before they reach exp() or the camera basis, and
// says what it had to correct.
//
// The bounds are the guards' own: they bound what would arrive at the camera as
// garbage, not what is a sensible setting, so every value a player might
// plausibly type is kept as typed. Validation only, never a floor - a configured
// 0.0 smoothing stays 0.0.
float ReadFloat(const cameraunlock::IniReader& ini, const char* section,
                const char* key, float fallback, float lo, float hi) {
    return guards::ReadFloatChecked(ini, section, key, fallback, lo, hi,
                                    &cameraunlock::logging::Line);
}

// A negative multiplier is a legitimate way to invert an axis without touching
// the Invert flags, so only the magnitude is bounded.
constexpr float kMaxSens = guards::kMaxSensitivity;

// Travel limits are metres, and the floor is 0 rather than -kMaxPositionLimit: a
// negative limit inverts the clamp in PositionProcessor - Clamp(v, -limit,
// limit) returns the lower bound for every input once the bounds cross - which
// pins the lean at a fixed offset instead of freeing it.
constexpr float kMaxLimit = guards::kMaxPositionLimit;

// IniReader::ReadBool matches the WHOLE value against a fixed list and hands
// back the default on anything else, silently. HeadTracking.ini is seeded next
// to the game exe for the player to edit, so "Enabled=true ; lean" - a comment
// where the file already puts one for other keys - would revert the setting with
// nothing in the log the README tells them to read.
bool ReadBool(const cameraunlock::IniReader& ini, const char* section,
              const char* key, bool fallback) {
    const std::string raw = guards::ReadRawValue(ini, section, key);
    if (raw.empty()) return fallback;

    std::string lowered = raw;
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") return true;
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") return false;

    HT_LOG("config: [%s] %s=%s is not true or false, so the default %s is used instead.",
           section, key, raw.c_str(), fallback ? "true" : "false");
    return fallback;
}

// A virtual key GetAsyncKeyState can never report is a hotkey that silently does
// nothing: ToggleKey=0x230 registers and is polled forever without ever firing,
// and the user cannot tell that from a broken mod. The token is parsed whole,
// for the same reason every number above is - IniReader::ReadHex is a strtol
// PREFIX parse, so "ToggleKey=End" reads as 0x0E and binds a code
// GetAsyncKeyState never reports.
int ReadHotkey(const cameraunlock::IniReader& ini, const char* key, int fallback) {
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

    HT_LOG("config: [Hotkeys] %s=%s is not a virtual key code that can be polled (write it "
           "as hex, GetAsyncKeyState defines 0x01-0xFE, and Ctrl/Shift/Alt are reserved for "
           "the chord bindings), using 0x%X",
           key, raw.c_str(), fallback);
    return fallback;
}

// An unknown value is the DEFAULT, not whichever branch happens to be last -
// that covers a typo in a hand-edited file and is also the migration path for a
// mode renamed since an older release wrote the setting. ParseAdsMode already
// does the falling back; this exists to say so in the log, because a player who
// typed `off` and got stock ADS deserves to know the file was not read the way
// they meant it.
cameraunlock::ads::AdsMode ReadAdsMode(const cameraunlock::IniReader& reader,
                                              cameraunlock::ads::AdsMode fallback) {
    const std::string raw = guards::ReadRawValue(reader, "ADS", "Mode");
    if (raw.empty()) return fallback;

    const cameraunlock::ads::AdsMode parsed = cameraunlock::ads::ParseAdsMode(raw.c_str());
    if (_stricmp(raw.c_str(), cameraunlock::ads::AdsModeValue(parsed)) != 0) {
        HT_LOG("config: [ADS] Mode=%s is not one of paused/marker/tracked - using %s.",
               raw.c_str(), cameraunlock::ads::AdsModeValue(parsed));
    }
    return parsed;
}

}  // namespace

bool Config::LoadFromIni(const std::string& path) {
    cameraunlock::IniReader r;
    if (!r.Open(path)) return false;

    // ReadInt is the one reader that does not honour its default on a
    // present-but-unparseable value - it hands back 0 - and a receiver bound to
    // port 0 takes an ephemeral port no tracker is sending to. A value past
    // 65535 is worse than useless cast straight to uint16_t: 70000 truncates to
    // 4464 and the mod listens on a port the user never named.
    const int rawPort = r.ReadInt("Network", "UDPPort", udpPort);
    bool portValid = false;
    const uint16_t port = cameraunlock::NormalizeUdpPort(rawPort, udpPort, portValid);
    if (!portValid) {
        HT_LOG("config: [Network] UDPPort=%d is outside 1024-65535, using %u", rawPort, port);
    }
    udpPort = port;

    // Sensitivity
    yawSensitivity   = ReadFloat(r, "Sensitivity", "YawMultiplier",   yawSensitivity,   -kMaxSens, kMaxSens);
    pitchSensitivity = ReadFloat(r, "Sensitivity", "PitchMultiplier", pitchSensitivity, -kMaxSens, kMaxSens);
    rollSensitivity  = ReadFloat(r, "Sensitivity", "RollMultiplier",  rollSensitivity,  -kMaxSens, kMaxSens);
    invertYaw   = ReadBool(r, "Sensitivity", "InvertYaw",   invertYaw);
    invertPitch = ReadBool(r, "Sensitivity", "InvertPitch", invertPitch);
    invertRoll  = ReadBool(r, "Sensitivity", "InvertRoll",  invertRoll);

    // Smoothing. Each key falls back to its own default (local 0.0, remote 0.15),
    // never to a shared one: a bad RemoteSmoothing dropping to the local default
    // would leave a phone's network jitter entirely unsmoothed.
    localSmoothing  = ReadFloat(r, "Smoothing", "LocalSmoothing",  localSmoothing,  0.0f, 1.0f);
    remoteSmoothing = ReadFloat(r, "Smoothing", "RemoteSmoothing", remoteSmoothing, 0.0f, 1.0f);
    guards::WarnRetiredSmoothingKey(r, "Smoothing", "Factor", &cameraunlock::logging::Line);
    guards::WarnRetiredSmoothingKey(r, "Position", "Smoothing", &cameraunlock::logging::Line);

    // Position
    positionEnabled = ReadBool (r, "Position", "Enabled",      positionEnabled);
    posSensitivityX = ReadFloat(r, "Position", "SensitivityX", posSensitivityX, -kMaxSens, kMaxSens);
    posSensitivityY = ReadFloat(r, "Position", "SensitivityY", posSensitivityY, -kMaxSens, kMaxSens);
    posSensitivityZ = ReadFloat(r, "Position", "SensitivityZ", posSensitivityZ, -kMaxSens, kMaxSens);
    posLimitX     = ReadFloat(r, "Position", "LimitX",     posLimitX,     0.0f, kMaxLimit);
    posLimitY     = ReadFloat(r, "Position", "LimitY",     posLimitY,     0.0f, kMaxLimit);
    // Defaults to whatever LimitY resolved to, so a config that sets only
    // LimitY stays symmetric instead of silently keeping the 0.20 default
    // downward while the upward budget moves.
    posLimitYDown = ReadFloat(r, "Position", "LimitYDown", posLimitY,     0.0f, kMaxLimit);
    posLimitZ     = ReadFloat(r, "Position", "LimitZ",     posLimitZ,     0.0f, kMaxLimit);
    posLimitZBack = ReadFloat(r, "Position", "LimitZBack", posLimitZBack, 0.0f, kMaxLimit);
    // No position smoothing key: position uses the same LocalSmoothing /
    // RemoteSmoothing pair as rotation.
    posInvertX = ReadBool(r, "Position", "InvertX", posInvertX);
    posInvertY = ReadBool(r, "Position", "InvertY", posInvertY);
    posInvertZ = ReadBool(r, "Position", "InvertZ", posInvertZ);

    // Reticle
    reticleFollowsAim = ReadBool(r, "Reticle", "FollowAim", reticleFollowsAim);

    // ADS
    adsMode = ReadAdsMode(r, adsMode);

    // Hotkeys
    toggleKey    = ReadHotkey(r, "ToggleKey",    toggleKey);
    cycleModeKey = ReadHotkey(r, "CycleModeKey", cycleModeKey);
    adsModeKey   = ReadHotkey(r, "AdsModeKey",   adsModeKey);

    // General
    autoEnable = ReadBool(r, "General", "AutoEnable", autoEnable);
    logToFile  = ReadBool(r, "General", "LogToFile",  logToFile);

    diagnostics      = ReadBool(r, "Debug", "Diagnostics",      diagnostics);
    readWatch        = ReadBool(r, "Debug", "ReadWatch",        readWatch);
    aimCallerSweep   = ReadBool(r, "Debug", "AimCallerSweep",   aimCallerSweep);
    cullCallerSweep  = ReadBool(r, "Debug", "CullCallerSweep",  cullCallerSweep);
    callerCensus     = ReadBool(r, "Debug", "CallerCensus",     callerCensus);
    liveCallerOverrides = ReadBool(r, "Debug", "LiveCallerOverrides", liveCallerOverrides);
    writeWatch       = ReadBool(r, "Debug", "WriteWatch",       writeWatch);
    weaponProbe      = ReadBool(r, "Debug", "WeaponProbe",      weaponProbe);
    cleanGlobalCycle = ReadBool(r, "Debug", "CleanGlobalCycle", cleanGlobalCycle);
    crosshairProbe   = ReadBool(r, "Debug", "CrosshairProbe",   crosshairProbe);
    reticleSweep     = ReadBool(r, "Debug", "ReticleSweep",     reticleSweep);

    return true;
}

bool Config::WriteDefault(const std::string& path) const {
    cameraunlock::IniWriter w;
    if (!w.Open(path)) return false;

    w.WriteSection("Network");
    w.WriteInt("UDPPort", udpPort);
    w.WriteBlankLine();

    w.WriteSection("Sensitivity");
    w.WriteDouble("YawMultiplier",   yawSensitivity);
    w.WriteDouble("PitchMultiplier", pitchSensitivity);
    w.WriteDouble("RollMultiplier",  rollSensitivity);
    w.WriteBool("InvertYaw",   invertYaw);
    w.WriteBool("InvertPitch", invertPitch);
    w.WriteBool("InvertRoll",  invertRoll);
    w.WriteBlankLine();

    w.WriteSection("Smoothing");
    w.WriteComment(" Picked per connection from the packet source address; covers rotation and position.");
    w.WriteComment(" Tracker running on this machine (loopback). 0 = none, 1 = heavy.");
    w.WriteDouble("LocalSmoothing", localSmoothing);
    w.WriteComment(" Tracker on a remote device on the network. 0 = none, 1 = heavy.");
    w.WriteDouble("RemoteSmoothing", remoteSmoothing);
    w.WriteBlankLine();

    w.WriteSection("Position");
    w.WriteBool("Enabled", positionEnabled);
    w.WriteDouble("SensitivityX", posSensitivityX);
    w.WriteDouble("SensitivityY", posSensitivityY);
    w.WriteDouble("SensitivityZ", posSensitivityZ);
    w.WriteDouble("LimitX",     posLimitX);
    w.WriteDouble("LimitY",     posLimitY);
    w.WriteDouble("LimitYDown", posLimitYDown);
    w.WriteDouble("LimitZ",     posLimitZ);
    w.WriteDouble("LimitZBack", posLimitZBack);
    w.WriteBool("InvertX", posInvertX);
    w.WriteBool("InvertY", posInvertY);
    w.WriteBool("InvertZ", posInvertZ);
    w.WriteBlankLine();

    w.WriteSection("Reticle");
    w.WriteComment(" Head tracking moves the view off the gun, so the crosshair NMS draws in");
    w.WriteComment(" the middle of the frame stops marking where the shot goes. On, the");
    w.WriteComment(" crosshair is moved onto the shot instead. Off restores the stock one.");
    w.WriteBool("FollowAim", reticleFollowsAim);
    w.WriteBlankLine();

    w.WriteSection("ADS");
    w.WriteComment(" What head tracking does while the sights are up. Cycled in game with");
    w.WriteComment(" Insert or Ctrl+Shift+U, which writes the new value back here.");
    w.WriteComment("   paused  - tracking stands down for as long as the sights are up.");
    w.WriteComment("   marker  - tracking stays live and an aim marker is drawn.");
    w.WriteComment("   tracked - tracking stays live, nothing drawn.");
    w.WriteComment(" This build does not yet know when your sights are up, and draws no");
    w.WriteComment(" marker, so all three modes currently behave the same. The missing");
    w.WriteComment(" weapon-zoom address is named in the log.");
    w.WriteString("Mode", cameraunlock::ads::AdsModeValue(adsMode));
    w.WriteBlankLine();

    w.WriteSection("Hotkeys");
    w.WriteHex("ToggleKey",    toggleKey);
    w.WriteHex("CycleModeKey", cycleModeKey);
    w.WriteHex("AdsModeKey",   adsModeKey);
    w.WriteBlankLine();

    w.WriteSection("General");
    w.WriteBool("AutoEnable", autoEnable);
    w.WriteBool("LogToFile",  logToFile);
    w.WriteBlankLine();

    w.WriteSection("Debug");
    w.WriteComment(" Engine-discovery detail in the log. Turn on for a bug report.");
    w.WriteBool("Diagnostics", diagnostics);
    w.WriteComment(" Log which instructions read the live camera transform, using a CPU");
    w.WriteComment(" data breakpoint. Makes the game crawl while each burst is armed.");
    w.WriteComment(" ReadWatch and WriteWatch share one debug register; only one runs.");
    w.WriteBool("ReadWatch", readWatch);
    w.WriteComment(" Serves the clean camera to one candidate caller at a time,");
    w.WriteComment(" cycling every few seconds, and logs each one. For finding");
    w.WriteComment(" which consumer aims something that is aiming wrongly.");
    w.WriteBool("AimCallerSweep", aimCallerSweep);
    w.WriteComment(" The same sweep inverted: serves the TRACKED camera to one");
    w.WriteComment(" candidate at a time, for finding what decides visibility.");
    w.WriteBool("CullCallerSweep", cullCallerSweep);
    w.WriteBool("CallerCensus", callerCensus);
    w.WriteBool("LiveCallerOverrides", liveCallerOverrides);
    w.WriteBool("WriteWatch", writeWatch);
    w.WriteBool("WeaponProbe", weaponProbe);
    w.WriteBool("CleanGlobalCycle", cleanGlobalCycle);
    w.WriteBool("CrosshairProbe", crosshairProbe);
    w.WriteComment(" Walks the crosshair around a fixed square instead of following the aim.");
    w.WriteBool("ReticleSweep", reticleSweep);

    return true;
}

} // namespace NMSHT
