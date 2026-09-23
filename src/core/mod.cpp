#include "pch.h"
#include "mod.h"
#include "debug_log.h"

#include "camera/camera_hook.h"

namespace NMSHT {

Mod& Mod::Instance() {
    static Mod s_instance;
    return s_instance;
}

static std::string DirectoryOf(HMODULE hModule) {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(hModule, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::string s(path, n);
    auto slash = s.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string{} : s.substr(0, slash);
}

bool Mod::Initialize(HMODULE hModule) {
    m_gameDir = DirectoryOf(hModule);
    if (m_gameDir.empty()) return false;

    const std::string iniPath = m_gameDir + "\\" + kConfigFileName;

    // Load INI (write defaults if missing).
    DWORD attrs = GetFileAttributesA(iniPath.c_str());
    const bool iniAvailable =
        attrs != INVALID_FILE_ATTRIBUTES || m_config.WriteDefault(iniPath);
    // Opened before the config load so the loader's own diagnostics (retired
    // keys, unreadable INI) reach the file instead of being dropped.
    OpenLogFile();
    const bool configLoaded = m_config.LoadFromIni(iniPath);
    if (!m_config.logToFile) CloseLogFile();

    HT_LOG("=== %s v%s ===", kModName, kModVersion);
    HT_LOG("Initialize: dir=%s", m_gameDir.c_str());
    if (!iniAvailable) {
        HT_LOG("WARN: could not write %s; settings will not persist.", iniPath.c_str());
    }
    if (!configLoaded) {
        HT_LOG("WARN: could not read %s - using built-in defaults.", iniPath.c_str());
    }

    // Rotation pipeline.
    cameraunlock::SensitivitySettings sens;
    sens.yaw   = m_config.yawSensitivity;
    sens.pitch = m_config.pitchSensitivity;
    sens.roll  = m_config.rollSensitivity;
    sens.invert_yaw   = m_config.invertYaw;
    sens.invert_pitch = m_config.invertPitch;
    sens.invert_roll  = m_config.invertRoll;
    m_session.GetProcessor().SetSensitivity(sens);

    // Position pipeline.
    cameraunlock::PositionSettings pos;
    pos.sensitivity_x = m_config.posSensitivityX;
    pos.sensitivity_y = m_config.posSensitivityY;
    pos.sensitivity_z = m_config.posSensitivityZ;
    pos.invert_x = m_config.posInvertX;
    pos.invert_y = m_config.posInvertY;
    pos.invert_z = m_config.posInvertZ;
    pos.limit_x = m_config.posLimitX;
    pos.limit_y = m_config.posLimitY;
    pos.limit_y_down = m_config.posLimitYDown;
    pos.limit_z = m_config.posLimitZ;
    pos.limit_z_back = m_config.posLimitZBack;

    // Both smoothing values go into both processors; which one applies is
    // decided per frame from the packet source address inside Update().
    m_session.SetLocalSmoothing(m_config.localSmoothing);
    m_session.SetRemoteSmoothing(m_config.remoteSmoothing);
    // Through the session, not the processor: it recomposes the owned smoothing
    // pair onto the struct, so the two calls compose in either order.
    m_session.SetPositionSettings(pos);
    HT_LOG("Smoothing: local=%.2f remote=%.2f",
           m_config.localSmoothing, m_config.remoteSmoothing);
    m_session.SetMode(m_config.positionEnabled
                          ? cameraunlock::TrackingMode::RotationAndPosition
                          : cameraunlock::TrackingMode::RotationOnly);

    // UDP receiver.
    m_receiver.SetLog([](const std::string& msg) {
        HT_LOG("UDP: %s", msg.c_str());
    });
    if (!m_receiver.Start(m_config.udpPort)) {
        // No cause named here. The receiver has already logged the one the OS
        // gave through the sink above, and "port busy" was wrong for every
        // bind that fails for another reason - a port inside a Hyper-V/WSL
        // reserved range refuses with WSAEACCES and no app is holding it.
        HT_LOG("UDP port %u not bound yet; the receiver is retrying and the mod "
               "stays loaded.", m_config.udpPort);
    } else {
        HT_LOG("UDP receiver started on port %u.", m_config.udpPort);
    }

    m_hotkeys.Start(m_config);

    // Before Install, not after. The commit hook can fire the instant it is
    // armed, and a frame that lands while m_enabled is still false latches the
    // one-shot "tracking is switched OFF" line - which is never corrected, and
    // which a previous "still no head tracking" report was traced to.
    m_enabled.store(m_config.autoEnable, std::memory_order_release);

    CameraHook::Instance().Install();

    HT_LOG("Initialized. Enabled=%s.", m_enabled.load() ? "true" : "false");
    return true;
}

void Mod::SetEnabled(bool enabled) {
    m_enabled.store(enabled, std::memory_order_release);
    HT_LOG("Enabled=%s", enabled ? "true" : "false");
}

void Mod::Toggle() { SetEnabled(!IsEnabled()); }

void Mod::CycleTrackingMode() {
    switch (m_session.CycleMode()) {
    case cameraunlock::TrackingMode::RotationAndPosition:
        HT_LOG("Tracking mode: rotation and position");
        break;
    case cameraunlock::TrackingMode::RotationOnly:
        HT_LOG("Tracking mode: rotation only (position disabled)");
        break;
    case cameraunlock::TrackingMode::PositionOnly:
        HT_LOG("Tracking mode: position only (rotation disabled)");
        break;
    }
}

void Mod::NoteConnectionState() {
    const bool receiving = m_receiver.IsReceiving();
    if (m_receiving.exchange(receiving, std::memory_order_relaxed) == receiving) return;

    if (receiving) {
        const bool remote = m_receiver.IsRemoteConnection();
        HT_LOG("Tracker connected: %s source, smoothing %.2f.",
               remote ? "remote" : "local",
               remote ? m_config.remoteSmoothing : m_config.localSmoothing);
    } else {
        // Not a freeze and not a reset: the receiver keeps reporting the last
        // pose it parsed, so the view holds where the head was rather than
        // snapping anywhere.
        HT_LOG("Tracker stopped sending (no packet for %d ms); the view holds "
               "the last pose it sent.",
               cameraunlock::UdpReceiver::kConnectionTimeoutMs);
    }
}

bool Mod::GetProcessedRotation(float& yaw, float& pitch, float& roll) {
    NoteConnectionState();
    if (!IsEnabled()) return false;

    const float dt = m_frameClock.Tick();
    if (!m_session.Update(dt)) return false;

    return m_session.GetRotation(yaw, pitch, roll);
}

bool Mod::GetPositionOffset(float& x, float& y, float& z) const {
    return m_session.GetPositionOffset(x, y, z);
}

} // namespace NMSHT
