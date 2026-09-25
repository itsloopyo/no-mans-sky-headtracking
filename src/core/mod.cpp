#include "pch.h"
#include "mod.h"
#include "debug_log.h"

#include "camera/camera_hook.h"

namespace NMSHT {

Mod& Mod::Instance() {
    static Mod s_instance;
    return s_instance;
}

static std::wstring DirectoryOf(HMODULE hModule) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(hModule, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0) return {};
        if (n < path.size()) {
            path.resize(n);
            break;
        }
        path.resize(path.size() * 2);
    }
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

static void LogLines(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) HT_LOG("%s", line.c_str());
}

bool Mod::Initialize(HMODULE hModule) {
    const std::wstring gameDir = DirectoryOf(hModule);
    if (gameDir.empty()) return false;

    // Opened before the config load so the old reader's diagnostics, when a
    // file an earlier build wrote is converted, reach the file.
    OpenLogFile();
    m_configOwner.emplace(ConfigOwnerOptions(gameDir + L"\\" + kConfigFileName));
    const auto loaded = m_configOwner->Load();
    m_config = loaded.config;
    if (!m_config.writeLog) CloseLogFile();

    HT_LOG("=== %s v%s ===", kModName, kModVersion);
    HT_LOG("Initialize: dir=%ls", gameDir.c_str());
    HT_LOG("Config: %s.", cameraunlock::config::ConfigLoadStatusName(loaded.status));
    LogLines(loaded.log);
    if (!loaded.reason.empty()) HT_LOG("%s", loaded.reason.c_str());

    // Position pipeline.
    cameraunlock::PositionSettings pos;
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
    m_session.SetMode(StartupTrackingMode(m_config));

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
    m_enabled.store(m_config.enableOnStartup, std::memory_order_release);

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
    const cameraunlock::TrackingMode mode = m_session.CycleMode();
    switch (mode) {
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

    // Applied above, saved here: a save that fails leaves the session on the
    // new mode and the file on the old one.
    const auto channels = cameraunlock::EncodeTrackingMode(mode);
    const auto saved = m_configOwner->Save([channels](Config& c) {
        c.rotationEnabled = channels.rotation_enabled;
        c.positionEnabled = channels.position_enabled;
    });
    LogLines(saved.log);
    if (!saved.reason.empty()) HT_LOG("%s", saved.reason.c_str());
}

std::optional<Config> Mod::ReloadChangedConfig() {
    if (!m_configOwner->FileChanged()) return std::nullopt;
    auto reloaded = m_configOwner->Reload();
    LogLines(reloaded.log);
    if (!reloaded.reason.empty()) HT_LOG("%s", reloaded.reason.c_str());
    return std::move(reloaded.config);
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
