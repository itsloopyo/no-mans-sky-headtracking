#include "pch.h"
#include "config.h"

#include <cameraunlock/config/ini_reader.h>

#include "debug_log.h"
#include "legacy_config/legacy_config.h"

namespace NMSHT {

bool Config::LoadFromIni(const std::string& path) {
    legacy::Config read;
    if (legacy::Read(path, read, &cameraunlock::logging::Line) == legacy::ReadStatus::Absent) return false;

    udpPort = read.udpPort;
    yawSensitivity = read.yawSensitivity;
    pitchSensitivity = read.pitchSensitivity;
    rollSensitivity = read.rollSensitivity;
    invertYaw = read.invertYaw;
    invertPitch = read.invertPitch;
    invertRoll = read.invertRoll;
    localSmoothing = read.localSmoothing;
    remoteSmoothing = read.remoteSmoothing;
    positionEnabled = read.positionEnabled;
    posSensitivityX = read.posSensitivityX;
    posSensitivityY = read.posSensitivityY;
    posSensitivityZ = read.posSensitivityZ;
    posLimitX = read.posLimitX;
    posLimitY = read.posLimitY;
    posLimitYDown = read.posLimitYDown;
    posLimitZ = read.posLimitZ;
    posLimitZBack = read.posLimitZBack;
    posInvertX = read.posInvertX;
    posInvertY = read.posInvertY;
    posInvertZ = read.posInvertZ;
    reticleFollowsAim = read.reticleFollowsAim;
    toggleKey = read.toggleKey;
    cycleModeKey = read.cycleModeKey;
    autoEnable = read.autoEnable;
    logToFile = read.logToFile;
    diagnostics = read.diagnostics;
    readWatch = read.readWatch;
    aimCallerSweep = read.aimCallerSweep;
    cullCallerSweep = read.cullCallerSweep;
    callerCensus = read.callerCensus;
    liveCallerOverrides = read.liveCallerOverrides;
    writeWatch = read.writeWatch;
    weaponProbe = read.weaponProbe;
    cleanGlobalCycle = read.cleanGlobalCycle;
    crosshairProbe = read.crosshairProbe;
    reticleSweep = read.reticleSweep;
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

    w.WriteSection("Hotkeys");
    w.WriteHex("ToggleKey",    toggleKey);
    w.WriteHex("CycleModeKey", cycleModeKey);
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
