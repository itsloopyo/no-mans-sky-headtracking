#include "pch.h"
#include "ads.h"

#include "debug_log.h"
#include "game_state.h"

namespace NMSHT {

using cameraunlock::ads::AdsMode;

Ads& Ads::Instance() {
    static Ads s_instance;
    return s_instance;
}

void Ads::Initialize(const std::string& iniPath, AdsMode mode) {
    m_iniPath = iniPath;
    m_mode.store(mode, std::memory_order_relaxed);
    HT_LOG("ADS mode: %s (%s).", cameraunlock::ads::AdsModeValue(mode),
           cameraunlock::ads::AdsModeLabel(mode));
}

void Ads::CycleMode() {
    const AdsMode next = cameraunlock::ads::NextAdsMode(Mode());
    m_mode.store(next, std::memory_order_relaxed);

    const char* const value = cameraunlock::ads::AdsModeValue(next);
    // Written key by key rather than by rewriting the file, so the comments and
    // every other setting in HeadTracking.ini survive a mid-session cycle.
    if (!m_iniPath.empty() &&
        !WritePrivateProfileStringA("ADS", "Mode", value, m_iniPath.c_str())) {
        HT_LOG("ADS: could not write [ADS] Mode=%s to %s (error %lu) - the mode is "
               "live for this session but will not survive a restart.",
               value, m_iniPath.c_str(), GetLastError());
    }

    // Before the toast, so a change made mid-aim takes effect on that aim
    // instead of riding the verdict the poll thread last computed.
    RefreshTrackingVerdict();

    HT_LOG("%s", cameraunlock::ads::AdsModeToast(next));
}

AdsPipeline::Pose Ads::Frame(bool aiming, bool live, const AdsPipeline::Pose& absolute) {
    const AdsMode mode = Mode();

    if (aiming != m_wasAiming) {
        m_wasAiming = aiming;
        if (aiming) {
            HT_LOG("ADS: sights up - %s.", cameraunlock::ads::AdsModeLabel(mode));
        } else {
            HT_LOG("ADS: sights down - head tracking back at the hip.");
        }
    }

    if constexpr (!kHasAimMarker) {
        if (aiming && mode == AdsMode::Marker && !m_saidNoMarker) {
            m_saidNoMarker = true;
            HT_LOG("ADS: `marker` is selected, but this build draws no aim marker - "
                   "NMS renders through Vulkan and its crosshair is a centre-pinned "
                   "sprite this mod cannot move yet. The mode behaves as `tracked` "
                   "until that changes.");
        }
    }

    return m_pipeline.Frame(mode, aiming, live, GetTickCount64(), absolute);
}

void Ads::Reset() {
    m_pipeline.Reset();
    m_wasAiming = false;
}

}  // namespace NMSHT
