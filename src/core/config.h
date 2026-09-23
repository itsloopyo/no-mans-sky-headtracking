#pragma once

#include <string>

#include <cameraunlock/data/position_settings.h>
#include <cameraunlock/math/smoothing_utils.h>

namespace NMSHT {

struct Config {
    // Network
    unsigned short udpPort = 4242;

    // Rotation sensitivity / invert
    float yawSensitivity   = 1.0f;
    float pitchSensitivity = 1.0f;
    float rollSensitivity  = 1.0f;
    bool  invertYaw   = false;
    bool  invertPitch = false;
    bool  invertRoll  = false;

    // Smoothing. Which of the two applies is decided per connection from the
    // packet source address; both cover rotation and position alike.
    float localSmoothing  = static_cast<float>(cameraunlock::math::kDefaultLocalSmoothing);
    float remoteSmoothing = static_cast<float>(cameraunlock::math::kDefaultRemoteSmoothing);

    // Position
    bool  positionEnabled = true;
    float posSensitivityX = 1.0f;
    float posSensitivityY = 1.0f;
    float posSensitivityZ = 1.0f;
    float posLimitX     = cameraunlock::PositionSettings{}.limit_x;
    float posLimitY     = cameraunlock::PositionSettings{}.limit_y;
    float posLimitYDown = cameraunlock::PositionSettings{}.limit_y_down;
    float posLimitZ     = cameraunlock::PositionSettings{}.limit_z;
    float posLimitZBack = cameraunlock::PositionSettings{}.limit_z_back;
    bool  posInvertX = false;
    bool  posInvertY = false;
    bool  posInvertZ = false;

    // Moves NMS's own crosshair onto the point the shot will cross, instead of
    // leaving it pinned to the middle of the frame where it stops marking the
    // shot the moment the head turns. Off restores the stock crosshair.
    bool reticleFollowsAim = true;

    // Hotkeys (virtual key codes)
    int toggleKey     = 0x23; // End
    int cycleModeKey  = 0x21; // PageUp

    // General
    bool autoEnable = true;
    bool logToFile  = true;

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
    // Records every distinct return address that calls the camera accessor,
    // with a hit count, and dumps the table to the log. The caller lists a
    // profile pins have to come from somewhere, and on a build nobody has
    // censused they were being guessed from another store's addresses, which
    // are not call sites here at all. Diagnostic only.
    bool callerCensus = false;
    // Re-reads AimTransformCallers / AimCopyCallers / TrackedTransformCallers
    // from the ini every few seconds, so a candidate set can be tried without
    // a rebuild and a save load. Separate from the census because counting
    // every accessor call is expensive and the override is not.
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

    // Loads from the given INI path. Missing keys keep their defaults.
    // Returns false if the file cannot be opened.
    bool LoadFromIni (const std::string& path);
    bool WriteDefault(const std::string& path) const;
};

} // namespace NMSHT
