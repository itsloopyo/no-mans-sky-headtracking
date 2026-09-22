#include "pch.h"
#include "screen_projection.h"

#include "camera_hook.h"
#include "camera_math.h"
#include "caller_table.h"
#include "core/debug_log.h"
#include "core/mod.h"

#include <atomic>
#include <cmath>
#include <intrin.h>

#include <cameraunlock/hooks/hook_manager.h>

namespace NMSHT {

namespace {

// Leaves no value in rax or xmm0: the epilogue restores its saved registers and
// returns, and its callers read their results out of the `out` buffer.
using WorldToScreenFn = void (*)(void* mgr, const float* rows, float fov,
                                 const void* points, void* out, int count,
                                 char flags, const void* screenSize);

WorldToScreenFn g_original = nullptr;
uintptr_t g_moduleBase = 0;

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_sawClean{0};
std::atomic<uint64_t> g_sawApplied{0};
std::atomic<uint64_t> g_sawOther{0};

CallerTable g_callers{"project"};
constexpr uint64_t kCallerReportInterval = 20000;

// Per-site samples of single-point projections. The crosshair is the one whose
// point sits on the camera's own forward ray, so the sample carries the angle
// between the projected point and each of the two forward vectors as well as
// the depth - which is what says whether the engine's crosshair distance is the
// real impact depth or a constant.
constexpr int kSiteSamples = 24;
constexpr int kSamplesPerSite = 3;
struct SiteSample {
    uintptr_t rva;
    int taken;
};
SiteSample g_siteSamples[kSiteSamples]{};
int g_siteSampleCount = 0;

// Engine basis rows are (right, up, BACKWARD) - verified in game by leaning:
// a forward lean (negative pipeline z) moves the camera forward when added
// along row 2, which only holds if row 2 points behind the eye.
void ForwardOf(const float* rows, float out[3]) {
    out[0] = -rows[8];
    out[1] = -rows[9];
    out[2] = -rows[10];
}

// World offset of a projected point from the camera the engine was handed.
// The engine computes (point.cell - camera.cell) + point.pos and lets the
// matrix subtract camera.pos, so the same expression is the offset here.
void OffsetFromCamera(const float* rows, const float* point, float out[3]) {
    for (int i = 0; i < 3; ++i) {
        out[i] = (point[4 + i] - rows[16 + i]) + point[i] - rows[12 + i];
    }
}

float Length3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float Dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// One sample per call site, a few times each: where the point sits relative to
// the two camera forwards, how far away it is, and where it landed on screen.
// A crosshair reads as angleApplied ~= 0; a world marker does not.
void SampleSinglePoint(const float* rows, const float* point, const float* out,
                       const float* screenSize, char flags, uintptr_t ret,
                       const float* clean, const float* applied) {
    const uintptr_t rva = ret - g_moduleBase;
    SiteSample* slot = nullptr;
    {
        // The detour fires around 150,000 times a minute across the HUD's
        // threads, and the bound check and the post-increment below are separate
        // operations: two threads at the last slot both pass the check and one
        // writes 16 bytes past the array. The function already does file I/O, so
        // a lock costs nothing worth measuring.
        static std::mutex s_tableMutex;
        std::lock_guard<std::mutex> lock(s_tableMutex);
        for (int i = 0; i < g_siteSampleCount; ++i) {
            if (g_siteSamples[i].rva == rva) { slot = &g_siteSamples[i]; break; }
        }
        if (slot == nullptr) {
            if (g_siteSampleCount >= kSiteSamples) return;
            slot = &g_siteSamples[g_siteSampleCount++];
            slot->rva = rva;
            slot->taken = 0;
        }
        if (slot->taken >= kSamplesPerSite) return;
        ++slot->taken;
    }

    float v[3];
    OffsetFromCamera(rows, point, v);
    const float d = Length3(v);
    float unit[3] = {0.0f, 0.0f, 0.0f};
    if (d > 1e-4f) { for (int i = 0; i < 3; ++i) unit[i] = v[i] / d; }

    float fApplied[3], fClean[3];
    ForwardOf(applied, fApplied);
    ForwardOf(clean, fClean);
    const float degApplied = std::acos(std::fmin(1.0f, std::fmax(-1.0f, Dot3(unit, fApplied))))
                             * kRadToDeg;
    const float degClean = std::acos(std::fmin(1.0f, std::fmax(-1.0f, Dot3(unit, fClean))))
                           * kRadToDeg;

    const float sx = (flags != 0 || screenSize == nullptr) ? 1.0f : screenSize[0];
    const float sy = (flags != 0 || screenSize == nullptr) ? 1.0f : screenSize[1];
    HT_LOG("Diag: point projection from RVA 0x%08llX flags=%d -> (%.4f %.4f) of "
           "(%.1f %.1f), depth %.3f m, angle applied %.2f deg / clean %.2f deg",
           (unsigned long long)rva, (int)flags, out[0], out[1], sx, sy, d,
           degApplied, degClean);
}

void WorldToScreenDetour(void* mgr, const float* rows, float fov,
                         const void* points, void* out, int count,
                         char flags, const void* screenSize) {
    const uint64_t n = g_calls.fetch_add(1, std::memory_order_relaxed);

    int basis = 0;
    if (rows != nullptr) {
        basis = ClassifyCameraBasis(rows);
        switch (basis) {
            case 1: g_sawClean.fetch_add(1, std::memory_order_relaxed); break;
            case 2: g_sawApplied.fetch_add(1, std::memory_order_relaxed); break;
            default: g_sawOther.fetch_add(1, std::memory_order_relaxed); break;
        }
    }

    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    g_callers.Record(ret, g_moduleBase);
    if (g_callers.DueForReport(n, kCallerReportInterval)) g_callers.Report(n);

    // Snapshotted once. Both accessors return null the moment the commit hook
    // stops applying a rotation - a menu opening is enough - and this detour
    // runs on threads that cannot see that transition, so re-reading them mid
    // function is a null dereference waiting for the next pause. It took the
    // game down once already.
    const float* const clean = CleanCameraRows();
    const float* const applied = AppliedCameraRows();
    const bool tracked = basis == 2 && clean != nullptr && applied != nullptr;

    g_original(mgr, rows, fov, points, out, count, flags, screenSize);

    if (count == 1 && tracked && points != nullptr && out != nullptr) {
        SampleSinglePoint(rows, static_cast<const float*>(points),
                          static_cast<const float*>(out),
                          static_cast<const float*>(screenSize), flags, ret,
                          clean, applied);
    }
}

}  // namespace

void InstallScreenProjection(std::uint32_t worldToScreenRva) {
    if (worldToScreenRva == 0 || !Mod::Instance().GetConfig().diagnostics) return;

    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* const target = reinterpret_cast<void*>(g_moduleBase + worldToScreenRva);

    auto& hm = cameraunlock::hooks::HookManager::Instance();
    auto st = hm.CreateHook(target, reinterpret_cast<void*>(&WorldToScreenDetour),
                            reinterpret_cast<void**>(&g_original));
    if (st == cameraunlock::hooks::HookStatus::Ok) st = hm.EnableHook(target);
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: world-to-screen hook FAILED (%s).",
               cameraunlock::hooks::HookStatusToString(st));
        hm.RemoveHook(target);
        return;
    }

    g_callers.SetVerbose(true);
    HT_LOG("World-to-screen hook installed at RVA 0x%08X - it reports which "
           "camera each HUD projection is handed and where its point lands.",
           worldToScreenRva);
}

std::uint64_t ProjectionCallCount() { return g_calls.load(); }
std::uint64_t ProjectionCleanCount() { return g_sawClean.load(); }
std::uint64_t ProjectionAppliedCount() { return g_sawApplied.load(); }
std::uint64_t ProjectionOtherCount() { return g_sawOther.load(); }

}  // namespace NMSHT
