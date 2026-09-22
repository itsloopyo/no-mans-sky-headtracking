#pragma once

#include "config.h"
#include "input/hotkey_handler.h"

#include <atomic>
#include <string>

#include <cameraunlock/protocol/udp_receiver.h>
#include <cameraunlock/time/frame_clock.h>
#include <cameraunlock/tracking/head_tracking_session.h>

namespace NMSHT {

class Mod {
public:
    static Mod& Instance();

    bool Initialize(HMODULE hModule);

    bool IsEnabled() const { return m_enabled.load(std::memory_order_acquire); }

    void SetEnabled(bool enabled);
    void Toggle();

    // Three-state cycle: rotation and position -> rotation only ->
    // position only -> back.
    void CycleTrackingMode();

    Config& GetConfig() { return m_config; }
    const std::string& GameDir() const { return m_gameDir; }

    // Runs the shared pipeline for this frame (interpolation -> smooth ->
    // sensitivity) and returns the processed
    // yaw/pitch/roll in degrees. False if tracking is disabled or no fresh
    // data has arrived. Call once per render frame.
    bool GetProcessedRotation(float& yaw, float& pitch, float& roll);

    // Processed head position offset in meters (camera-local) from the
    // latest GetProcessedRotation call. False when position tracking is
    // off or no position sample exists.
    bool GetPositionOffset(float& x, float& y, float& z) const;

    Mod(const Mod&) = delete;
    Mod& operator=(const Mod&) = delete;

private:
    Mod()  = default;
    ~Mod() = default;

    // Edge-triggered tracker liveness. Every other reason the view can stop
    // moving already leaves a line - suppressed, disabled, dormant - so a
    // tracker that simply stopped sending was the one cause the log could not
    // name. Compared with exchange because the render and commit threads both
    // reach it.
    void NoteConnectionState();

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_receiving{false};

    Config m_config;
    std::string m_gameDir;

    cameraunlock::UdpReceiver m_receiver;
    using Session = cameraunlock::HeadTrackingSession<cameraunlock::UdpReceiver>;
    // Without this the session would silently report every tracker as local and
    // pin smoothing to localSmoothing forever, with no compile error.
    static_assert(Session::kHasRemoteConnection,
                  "receiver must expose IsRemoteConnection() for per-connection smoothing");
    Session m_session{m_receiver};
    cameraunlock::time::FrameClock m_frameClock;

    HotkeyHandler m_hotkeys;
};

} // namespace NMSHT
