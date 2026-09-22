#pragma once

#include <atomic>
#include <string>

#include "ads_pipeline.h"

namespace NMSHT {

// Whether this build can draw the aim marker that the `marker` slot promises.
//
// It cannot, and this constant is the one place that says so. The mod now has
// somewhere to draw - the crosshair moves with the shot, through the reticle
// element's own layout position - but a marker is a SECOND thing on screen,
// held at the aim while the sights are up and the head looks elsewhere, and
// there is nothing to draw that with: NMS renders through Vulkan and
// cameraunlock-core's overlay backends are D3D9, D3D11 and D3D12.
//
// It would have nothing to key off either. The address of the game's own
// weapon-zoom state is still unfound, so this build never knows the sights are
// up. Until both hold, the slot behaves exactly like `tracked` and says so once
// in the log rather than half-drawing.
constexpr bool kHasAimMarker = false;

// What head tracking does while the sights are up.
//
// The cycle, the value strings, the toast wording, the fade and the blend all
// come from cameraunlock-core. This owns the mod-side wiring: which mode is
// current, persisting it, and running one frame of the pipeline against the
// game's clock.
class Ads {
public:
    static Ads& Instance();

    // `iniPath` is where the chosen mode is written back on every cycle, so it
    // survives a restart.
    void Initialize(const std::string& iniPath, cameraunlock::ads::AdsMode mode);

    cameraunlock::ads::AdsMode Mode() const {
        return m_mode.load(std::memory_order_relaxed);
    }

    // Hotkey handler. Advances the cycle, persists it, re-runs the
    // tracking-state verdict so a change made mid-aim takes effect on that aim,
    // and reports the mode it switched to. Debouncing is the hotkey poller's
    // key edge detection, which is per key, so the nav key and the chord each
    // fire exactly once.
    void CycleMode();

    // One rendered frame, after the head pose has been produced and before it
    // reaches the camera.
    AdsPipeline::Pose Frame(bool aiming, bool live, const AdsPipeline::Pose& absolute);

    // Called on every frame the head pose is suppressed for any other reason -
    // menu, loading, another explorer, master toggle, tracker dropout - so the
    // next aim re-enters against a fresh entry pose instead of one captured
    // before the suppression.
    void Reset();

    Ads(const Ads&) = delete;
    Ads& operator=(const Ads&) = delete;

private:
    Ads() = default;
    ~Ads() = default;

    std::atomic<cameraunlock::ads::AdsMode> m_mode{cameraunlock::ads::kDefaultAdsMode};
    std::string m_iniPath;

    // Render-thread only, like the rest of the per-frame camera state.
    AdsPipeline m_pipeline;
    bool m_wasAiming = false;
    bool m_saidNoMarker = false;
};

}  // namespace NMSHT
