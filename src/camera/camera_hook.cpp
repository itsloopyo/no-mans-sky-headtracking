#include "../core/constants.h"
#include "../core/mod.h"
#include "pch.h"
#include "camera_hook.h"

#include "build_profile.h"
#include "build_resolver.h"
#include "camera_math.h"
#include "frame_phase.h"
#include "commit_hook.h"
#include "scene_sample.h"
#include "weapon_decouple.h"
#include "read_watch.h"
#include "weapon_probe.h"
#include "crosshair_probe.h"
#include "reticle.h"
#include "screen_projection.h"
#include "caller_table.h"
#include "core/debug_log.h"
#include "core/game_state.h"
#include "core/mod.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <intrin.h>
#include <thread>

#include <cameraunlock/hooks/hook_manager.h>
#include <cameraunlock/memory/pe_fingerprint.h>
#include <cameraunlock/memory/rtti_vtable.h>

namespace NMSHT {

namespace {

// cGcCameraManager vtable[10]: float* f(void* mgr, float* outTransform).
//
// Copies the active cTkCamera's world node transform into the caller's
// 20-float buffer (5 rows of 4):
//   rows 0..2  orientation basis (right / up / BACKWARD), row-vector layout
//   row  3     position, fractional part of NMS's floating-origin split
//   row  4     position, quantised cell part
// then folds in the HMD offset when the display mode is VR.
//
// Row 2 points BEHIND the eye, not along the view. Verified in game rather than
// assumed: a forward lean is negative z out of the shared pipeline, it is added
// along row 2 unnegated, and the camera moves forward - which only holds if row
// 2 is the backward vector. The same fact makes the yaw sign work out: a
// rotation composed for a forward-Z basis and applied to this one turns the
// opposite way, which is why the head yaw is negated here and nowhere else.
using CameraTransformFn = float* (*)(void* mgr, float* outTransform);

CameraTransformFn g_copyOriginal = nullptr;
void* g_copyTarget = nullptr;
uintptr_t g_moduleBase = 0;

// [Debug] Diagnostics, latched at install time. The config is fully loaded
// before any hook is enabled and nothing writes it afterwards, so the detours
// read this rather than walking the mod singleton on a path the engine takes
// hundreds of thousands of times a minute across ~100 threads.
bool g_diagnostics = false;

std::atomic<uint64_t> g_callCount{0};

CallerTable g_transformCallers{"copy"};

// How many calls apart the periodic call-site reports are. The copy accessor
// runs at most a few times a frame; the source accessor is far hotter, so it
// reports on a longer stride to keep the log readable.
constexpr uint64_t kCopyReportInterval = 3000;
constexpr uint64_t kSourceReportInterval = 20000;

// cGcCameraManager vtable[9]: float* f(void* mgr). Returns a pointer to the
// active camera's live 5-row transform, which vtable[10] copies out of.
using ActiveTransformFn = float* (*)(void* mgr);

// Diagnostics probe on that same vtable[9].
//
// vtable[10] hands each caller a COPY of the camera transform, so rotating the
// copy only moves the view if the renderer is one of its callers. vtable[9]
// hands out a pointer to the live transform instead, and vtable[10] is itself
// just one of its callers. Counting vtable[9]'s callers therefore answers the
// question vtable[10] alone cannot: whether the camera manager is running at
// all, and which code actually reads the camera transform. The recorded return
// address RVAs are the functions to decompile next.
CallerTable g_sourceCallers{"source"};
std::atomic<uint64_t> g_sourceCallCount{0};
ActiveTransformFn g_sourceOriginal = nullptr;
std::initializer_list<std::uint32_t> g_aimCopyCallers;
std::initializer_list<std::uint32_t> g_aimTransformCallers;
std::atomic<uint64_t> g_aimCopies{0};
std::atomic<uint64_t> g_aimTransforms{0};
// Per caller, because one combined total cannot say WHICH aim path ran. A path
// that fires only while the player is shooting is worth a handful of calls
// against a per-frame one's thousands, and the combined figure hides it
// completely - which is exactly how a wrongly pinned weapon-ray caller reads as
// "working" in a log.
// [Debug] AimCallerSweep. Every consumer seen reading the camera in gameplay,
// as accessor return addresses. One at a time is served the CLEAN camera on top
// of the profile's own list, so whichever of them aims a thing that is aiming
// wrongly can be found by looking at the game instead of by guessing. Quit the
// moment it looks right; the last line logged is the answer.
constexpr std::uint32_t kSweepCandidates[] = {
    0x01554253u, 0x0083A23Cu, 0x00477961u, 0x005168DAu, 0x00A9A6FCu,
    0x016CCB2Du, 0x004A028Bu, 0x00B5DD87u, 0x0049705Eu, 0x01407755u,
    0x0083AE60u, 0x01313A91u, 0x015D558Cu, 0x0083EEAEu, 0x01669672u,
    0x016642BEu, 0x01547C49u, 0x017A0AB6u, 0x017CA4C9u, 0x005181D6u,
    0x0154D9BBu, 0x0051802Cu, 0x0049D17Fu, 0x007F809Du, 0x0155E314u,
};
constexpr int kSweepCount = static_cast<int>(sizeof(kSweepCandidates) /
                                             sizeof(kSweepCandidates[0]));
constexpr DWORD kSweepHoldMs = 4000;
std::atomic<std::uint32_t> g_sweepCurrent{0};
std::atomic<uint64_t> g_sweepHits{0};
std::atomic<bool> g_sweepFreeze{false};

// Every distinct return address that reaches the camera accessor, with a hit
// count. A profile's aim callers are return addresses in THIS image, and on a
// build nobody has censused they were carried over from another store, where
// they name unrelated code and are never return addresses here at all. Counting
// what actually calls is the only way to get a candidate set worth sweeping.
//
// Open addressing, no eviction: the table saturates at kCensusSlots distinct
// callers and stops taking new ones, which is honest - a full table is visible
// in the dump as a count that stops growing, where an evicting table would just
// lose the rare caller that the weapon path is.
constexpr int kCensusSlots = 512;
struct CensusSlot {
    std::atomic<std::uint32_t> rva{0};
    std::atomic<std::uint64_t> count{0};
};
CensusSlot g_census[kCensusSlots];
CensusSlot g_censusCopy[kCensusSlots];
bool g_censusOn = false;
bool g_overridesOn = false;

void RecordCaller(CensusSlot* table, std::uint32_t rva) {
    std::uint32_t h = rva * 2654435761u;
    for (int probe = 0; probe < 16; ++probe) {
        CensusSlot& slot = table[(h >> 20) % kCensusSlots];
        std::uint32_t have = slot.rva.load(std::memory_order_relaxed);
        if (have == rva) {
            slot.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (have == 0) {
            std::uint32_t expect = 0;
            if (slot.rva.compare_exchange_strong(expect, rva,
                                                 std::memory_order_relaxed)) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (expect == rva) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        h += 0x9E3779B9u;
    }
}

// Absolute counts, not deltas. Two dumps either side of a known action - firing
// the mining beam, walking - name the callers on that path by which counts
// moved, and that needs no reset and no interaction with the game.
void DumpOne(CensusSlot* table, const char* tag) {
    int live = 0;
    for (int i = 0; i < kCensusSlots; ++i) {
        const std::uint32_t rva = table[i].rva.load(std::memory_order_relaxed);
        if (rva == 0) continue;
        ++live;
        HT_LOG("            census %s 0x%08X %llu", tag, rva,
               (unsigned long long)table[i].count.load(std::memory_order_relaxed));
    }
    HT_LOG("            census %s: %d distinct callers", tag, live);
}

// The two paths a caller can take the camera by. Steam pins one aim caller on
// the copy path and two on the transform path, so a census of only one of them
// cannot find the set.
void DumpCallerCensus() {
    if (!g_censusOn) return;
    DumpOne(g_census, "xform");
    DumpOne(g_censusCopy, "copy");
}

// A caller list read out of the ini and read again whenever the file changes
// while the game runs, so a subset can be tested without a rebuild, a relaunch
// and a save load. Finding which callers decide aim is a search, and at eight
// minutes a round it is a search nobody finishes; at five seconds a round it is
// twenty minutes.
//
// Published as a pointer into two buffers rather than edited in place, because
// the detours read it from about a hundred threads while the census thread
// writes it.
struct CallerSet {
    std::uint32_t rva[kMaxOverrideCallers];
    int count;
};
CallerSet g_overrideBuf[3][2];
int g_overrideTurn[3] = {0, 0, 0};
std::atomic<const CallerSet*> g_override[3]{nullptr, nullptr, nullptr};

bool CallerListed(const std::atomic<const CallerSet*>& live,
                  std::initializer_list<std::uint32_t> pinned,
                  std::uint32_t caller) {
    const CallerSet* const set = live.load(std::memory_order_acquire);
    if (set != nullptr) {
        for (int i = 0; i < set->count; ++i) {
            if (set->rva[i] == caller) return true;
        }
        return false;
    }
    for (const auto rva : pinned) {
        if (rva == caller) return true;
    }
    return false;
}

void ApplyOverride(int which, const char* key, const std::vector<std::uint32_t>& callers) {
    if (callers.empty()) {
        g_override[which].store(nullptr, std::memory_order_release);
        return;
    }
    if (callers.size() > kMaxOverrideCallers) {
        HT_LOG("[Debug] %s lists %zu callers; the first %zu are used.", key, callers.size(),
               kMaxOverrideCallers);
    }
    CallerSet& dst = g_overrideBuf[which][g_overrideTurn[which] ^= 1];
    dst.count = static_cast<int>(std::min(callers.size(), kMaxOverrideCallers));
    std::copy(callers.begin(), callers.begin() + dst.count, dst.rva);
    g_override[which].store(&dst, std::memory_order_release);
}

void ApplyOverrides(const Config& cfg) {
    ApplyOverride(0, "AimTransformCallers", cfg.aimTransformCallers);
    ApplyOverride(1, "AimCopyCallers", cfg.aimCopyCallers);
    ApplyOverride(2, "TrackedTransformCallers", cfg.trackedTransformCallers);
}

constexpr DWORD kCensusDumpMs = 5000;

void DiagnosticThread() {
    bool overridesApplied = false;
    for (;;) {
        Sleep(kCensusDumpMs);
        if (g_overridesOn && !overridesApplied) {
            ApplyOverrides(Mod::Instance().GetConfig());
            overridesApplied = true;
        } else if (g_overridesOn) {
            if (const auto reloaded = Mod::Instance().ReloadChangedConfig()) ApplyOverrides(*reloaded);
        }
        if (!g_censusOn) {
            if (!g_overridesOn) return;
            continue;
        }
        HT_LOG("            census mark");
        DumpCallerCensus();
    }
}

constexpr int kMaxAimCallers = 8;
std::atomic<uint64_t> g_aimTransformHits[kMaxAimCallers]{};
void* g_sourceTarget = nullptr;

// The live transform and the rows written into it.
//
// These are NOT single-owner. The commit thread writes them; the ~100 threads
// in the accessor read the clean rows through the pointer SourceDetour hands
// back, the weapon-decouple exception handler reads both sets, and the weapon
// probe writes the applied set from its own thread. A plain 20-float copy under
// that is a torn read: a reader can take rows 0-1 from this frame and rows 2-4
// from the last, and a basis stitched from two frames is not a rotation. The
// engine then builds an aim ray, or the multitool's placement, out of it.
//
// So each set is published as a pointer into a ring of slots. A writer fills a
// slot nothing is currently published from and releases the pointer; a reader
// acquires the pointer once and uses only that. Four slots rather than two
// because SourceDetour hands its pointer to the engine, which reads through it
// later rather than immediately: at the engine's ~20 Hz commit rate that leaves
// a caller about 200 ms to finish with a slot before it comes round again.
float* volatile g_liveTransform = nullptr;

constexpr int kPublishSlots = 4;
float g_cleanSlots[kPublishSlots][kTransformFloats]{};
float g_appliedSlots[kPublishSlots][kTransformFloats]{};
std::atomic<const float*> g_cleanPub{nullptr};
std::atomic<const float*> g_appliedPub{nullptr};
static_assert(std::atomic<const float*>::is_always_lock_free,
              "read from the accessor on ~100 threads and from a vectored "
              "exception handler, so it must never take a lock");
// Both cursors are claimed with an interlocked increment, never a mutex and
// never a plain ++. A mutex is out because PublishApplied runs on the commit
// thread, which is inside a vectored exception handler while another thread may
// be suspending threads - the deadlock shape game_state.cpp documents. A plain
// ++ is out because NoteCommitThread exists precisely to say that a second
// commit thread is not structurally prevented, and two writers racing a cursor
// can land on the same slot and tear it under a reader.
volatile long g_cleanClaim = 0;
volatile long g_appliedClaim = 0;

// The copy the accessor hands out in place of the live camera, which is how
// aim decoupling works: the renderer reads the rotated camera out of memory
// while everything coming through the accessor gets the clean one.
//
// It is a snapshot taken at the engine's camera commit, so it only stands in
// faithfully for as many bytes as the caller reads AND for as long as nothing
// in it moves between commits. `BuildProfile::accessorMirrorBytes` is how many
// bytes that is, and zero means this build has no faithful length - the
// accessor is then left alone except for the profile's selected aim callers.
constexpr int kMaxMirrorFloats = 0x100 / static_cast<int>(sizeof(float));
float g_mirrorSlots[kPublishSlots][kMaxMirrorFloats]{};
volatile long g_mirrorClaim = 0;
std::atomic<const float*> g_mirrorPub{nullptr};
int g_mirrorFloats = 0;
int g_mirrorObjectFloats = 0;
bool g_sweepServesTracked = false;
bool g_decoupleByRowBlock = false;

// A whole-object copy whose accessor block carries the TRACKED rows, for the
// callers named in BuildProfile::trackedTransformCallers. The live object keeps
// clean rows there so gameplay aims straight; culling needs the opposite, and
// this is how it gets it without moving the split for everyone else.
float g_trackedSlots[kPublishSlots][kMaxMirrorFloats]{};
volatile long g_trackedClaim = 0;
std::atomic<const float*> g_trackedPub{nullptr};
std::initializer_list<std::uint32_t> g_trackedTransformCallers;
std::atomic<uint64_t> g_trackedServed{0};

void PublishTrackedMirror(const float* object, const float* trackedRows) {
    // Only meaningful where the accessor's block is kept clean. With the
    // tracked rows in both blocks there is nothing for this to correct.
    if (!g_decoupleByRowBlock) return;
    if (g_mirrorObjectFloats == 0) return;
    if (g_trackedTransformCallers.size() == 0 && !g_sweepServesTracked &&
        g_override[2].load(std::memory_order_acquire) == nullptr) return;
    const unsigned long n = static_cast<unsigned long>(InterlockedIncrement(&g_trackedClaim));
    float* const dst = g_trackedSlots[n % kPublishSlots];
    for (int i = 0; i < g_mirrorObjectFloats; ++i) dst[i] = object[i];
    CopyTransform(dst, trackedRows);
    g_trackedPub.store(dst, std::memory_order_release);
}

// Floats from the accessor's pointer to the rows the renderer projects from.
// Zero on June, where they are the same block; 0x50 bytes apart since the
// September re-declaration. See BuildProfile::renderRowsFromTransform - the
// accessor's own block is the one the commit writes behind a gate, so it is
// both staler and invisible to the renderer.
int g_renderRowsFloats = 0;

// `object` is the camera object base - what the accessor returns - and `clean`
// the rows the engine committed before this mod touched them.
//
// The copy is the whole object, and the clean rows go into BOTH row blocks. A
// copy that stopped at 0x50 ended exactly where the render rows begin, so any
// caller that followed this pointer to them read off the end of the buffer, and
// the frame came out black. Length was never the real variable: what matters is
// that every field a caller can reach through this pointer holds what the live
// object holds, except the two the decoupling deliberately rewrites.
void PublishMirror(const float* object, const float* clean) {
    if (g_mirrorFloats == 0) return;
    const unsigned long n = static_cast<unsigned long>(InterlockedIncrement(&g_mirrorClaim));
    float* const dst = g_mirrorSlots[n % kPublishSlots];
    for (int i = 0; i < g_mirrorFloats; ++i) dst[i] = object[i];
    CopyTransform(dst, clean);
    if (g_renderRowsFloats != 0 && g_renderRowsFloats + kTransformFloats <= g_mirrorFloats) {
        CopyTransform(dst + g_renderRowsFloats, clean);
    }
    g_mirrorPub.store(dst, std::memory_order_release);
}

unsigned long ClaimIndex(volatile long& claim) {
    return static_cast<unsigned long>(InterlockedIncrement(&claim)) % kPublishSlots;
}

void PublishClean(const float* src) {
    const unsigned long i = ClaimIndex(g_cleanClaim);
    CopyTransform(g_cleanSlots[i], src);
    g_cleanPub.store(g_cleanSlots[i], std::memory_order_release);
}

void PublishApplied(const float* src) {
    const unsigned long i = ClaimIndex(g_appliedClaim);
    CopyTransform(g_appliedSlots[i], src);
    g_appliedPub.store(g_appliedSlots[i], std::memory_order_release);
}

// Sixteen, not four: the reticle holds the pair the scene sampled until the
// HUD draws, and on Game Pass two commits can land in one frame.
constexpr int kPairSlots = 16;
CameraPair g_pairSlots[kPairSlots]{};
volatile long g_pairClaim = 0;
std::atomic<const CameraPair*> g_pairPub{nullptr};
std::atomic<const CameraPair*> g_scenePair{nullptr};
bool g_sceneSampling = false;
std::atomic<uint64_t> g_sceneUnmatched{0};
std::atomic<uint64_t> g_sceneStale{0};

void PublishPair(const float* clean, const float* applied, uint64_t commit) {
    const unsigned long i =
        static_cast<unsigned long>(InterlockedIncrement(&g_pairClaim)) % kPairSlots;
    CameraPair& dst = g_pairSlots[i];
    CopyTransform(dst.clean, clean);
    CopyTransform(dst.applied, applied);
    dst.commit = commit;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    dst.qpc = now.QuadPart;
    g_pairPub.store(&dst, std::memory_order_release);
}

// Injection instrumentation. The frame hooks were written and shipped without
// any way to tell, from a log, whether they run at all - so "no head tracking"
// and "the rotation reaches the renderer but the renderer sampled the camera
// earlier" were indistinguishable. These counters separate them.
std::atomic<uint64_t> g_beginCalls{0};
std::atomic<uint64_t> g_otherCamera{0};
std::atomic<uint64_t> g_endCalls{0};
std::atomic<uint64_t> g_noLive{0};
std::atomic<uint64_t> g_noPose{0};
std::atomic<uint64_t> g_disabled{0};
std::atomic<uint64_t> g_suppressed{0};
bool g_saidDisabled = false;
const char* volatile g_lastSuppression = nullptr;
std::atomic<uint64_t> g_servedClean{0};
std::atomic<uint64_t> g_applyCount{0};
std::atomic<uint64_t> g_notCommitted{0};
bool g_saidNotCommitted = false;

// Whether the last applied rows actually differ from the clean rows they came
// from. An exactly centred pose composes to the clean camera bit for bit, and
// then "the transform still holds our own write" and "the engine wrote the same
// camera again" are the same twelve floats - so the commit guard cannot tell
// them apart and must not run.
std::atomic<bool> g_appliedDiffers{false};
std::atomic<unsigned long> g_beginThread{0};

// The breakpoint is armed on every thread, so nothing structurally stops a
// second one from committing. Two commit threads would reintroduce exactly the
// compounding the publish ring exists to prevent, so say it out loud once
// rather than leave it to be inferred from a garbled camera.
void NoteCommitThread() {
    const unsigned long self = GetCurrentThreadId();
    unsigned long expected = 0;
    if (g_beginThread.compare_exchange_strong(expected, self,
                                              std::memory_order_acq_rel)) {
        return;
    }
    if (expected == self) return;
    static bool said = false;
    if (said) return;
    said = true;
    HT_LOG("WARN: the camera commit has been seen on more than one thread (%lu "
           "and %lu). Head tracking still works, but this is the shape the "
           "compounding-rotation bug had.", expected, self);
}

// The pose the commit hook last applied. The diagnostics line in the copy
// accessor reports THIS rather than asking the pipeline for a fresh one:
// Mod::GetProcessedRotation ticks the frame clock and advances the session's
// interpolation and smoothing state, and it is documented as once per render
// frame. The copy accessor does not run on the commit thread, so calling it
// there raced the real camera path over that state and stole its dt.
struct AppliedPose {
    float yaw, pitch, roll;
    float x, y, z;
};
AppliedPose g_lastPose{};
// The frame-phase counters are the answer to "is the pipeline reaching the
// renderer", so they have to keep appearing for the whole session: a single
// report at startup cannot date the moment tracking stopped. The rate is the
// part that has to differ. Every 300 commits is roughly five seconds, which is
// four log lines every five seconds for as long as the game runs, and that
// buries the state transitions the log is actually read for.
//
// The quiet rate is what a bug report is read from, so it is bounded by how
// long a player is willing to play before quitting and sending the log rather
// than by how tidy the file is. At 18000 it was about five minutes, and a
// session that ended after sixteen seconds of gameplay produced a log with the
// hooks, the profile and the tracker all confirmed and NOT ONE line saying
// whether the camera moved. Roughly half a minute costs four lines a minute and
// makes that session answerable.
constexpr uint64_t kFrameReportInterval = 300;
constexpr uint64_t kQuietFrameReportInterval = 1800;

void ReportFramePhase() {
    HT_LOG("Frame phase: commits=%llu presents=%llu applied=%llu noLive=%llu "
           "noPose=%llu disabled=%llu suppressed=%llu yaw=%.2f commitTid=%lu "
           "cleanServed=%llu",
           (unsigned long long)g_beginCalls.load(),
           (unsigned long long)g_endCalls.load(),
           (unsigned long long)g_applyCount.load(),
           (unsigned long long)g_noLive.load(),
           (unsigned long long)g_noPose.load(),
           (unsigned long long)g_disabled.load(),
           (unsigned long long)g_suppressed.load(),
           g_lastPose.yaw, g_beginThread.load(),
           (unsigned long long)g_servedClean.load());
    if (g_trackedServed.load() != 0) {
        HT_LOG("            tracked camera served to culling: %llu",
               (unsigned long long)g_trackedServed.load());
    }
    if (g_otherCamera.load() != 0) {
        HT_LOG("            commits skipped as another camera's: %llu",
               (unsigned long long)g_otherCamera.load());
    }
    if (g_notCommitted.load() != 0) {
        HT_LOG("            commits refused because the transform still held our "
               "own write: %llu", (unsigned long long)g_notCommitted.load());
    }
    HT_LOG("            commitHits=%llu projections=%llu "
           "proj(clean=%llu applied=%llu other=%llu) state=%s",
           (unsigned long long)CommitHitCount(),
           (unsigned long long)ProjectionCallCount(),
           (unsigned long long)ProjectionCleanCount(),
           (unsigned long long)ProjectionAppliedCount(),
           (unsigned long long)ProjectionOtherCount(),
           CurrentStateName());
    const float fov = LiveCameraFovDegrees();
    HT_LOG("            aim copies=%llu transforms=%llu",
           (unsigned long long)g_aimCopies.load(),
           (unsigned long long)g_aimTransforms.load());
    {
        int idx = 0;
        for (const auto rva : g_aimTransformCallers) {
            if (idx >= kMaxAimCallers) break;
            HT_LOG("              aim caller 0x%08X served clean %llu time(s)",
                   rva, (unsigned long long)g_aimTransformHits[idx].load());
            ++idx;
        }
    }
    HT_LOG("            field of view %.1f deg as Options > Camera states it, "
           "%.2f deg vertical", 2.0f * fov, fov);
    float rx = 0.0f, ry = 0.0f;
    LastReticleOffset(rx, ry);
    HT_LOG("            reticle placed %llu times, last correction (%.2f%% %.2f%%) of frame",
           (unsigned long long)ReticlePlacementCount(), rx, ry);
    DumpCallerCensus();
}

bool g_freeze = false;

// The camera manager embeds two cTkCamera objects and a byte picks the live one
// - vtable[12] returns the object, vtable[9] the transform inside it. So the
// pointer this hook already holds locates the field of view too, at whatever
// distance this build's class puts it: `cameraFovFromTransform`, because the
// September re-declaration moved the transform to the object start AND the FOV
// field with it (+0xA0 on June, +0xF0 on September and GDK).
//
// What the engine does with that field is what fixes the units: both projection
// helpers (0x0065CCD0, 0x0065CEE0 on June) pass `fovScale * <that field>` to
// cTkGraphicsManager's world-to-screen, which builds the frame's projection as
// top = near * tan(that * 0.25 degrees) and right = top * width / height. A
// QUARTER of the product is the vertical half-angle, so half of it is the
// vertical FOV in degrees.
//
// Of the two terms, the scale is the one that moves. The camera's field sits at 75
// whatever the player picked, and the setting reaches the projection as this
// global: 1.0 at 75 degrees, 1.3333 at 100.
std::uint32_t g_fovScaleRva = 0;
std::uint32_t g_cameraFovFromTransform = 0;

// A .data global that holds a copy of the camera basis and was proved to turn
// with the head by the differential scan. The viewmodel renders in view space,
// so it has no world transform of its own to correct; if it is placed from this
// instead, writing the CLEAN basis there while the live transform stays rotated
// will unglue it from the view.
//
// Pinned per build like every other RVA this mod writes through, rather than
// hard-coded: the older profiles carry zero and the probe stays off there. The
// second candidate this once cycled, 0x06E2F090, is deliberately not here - it
// took the game down when it was held clean.
std::uint32_t g_cameraGlobalRva = 0;
volatile long g_cleanGlobal = 0;

void CaptureClean(const float* object, const float* renderRows) {
    PublishClean(renderRows);
    PublishMirror(object, renderRows);
}

float* SourceDetour(void* mgr) {
    float* result = g_sourceOriginal(mgr);

    // Discovery bookkeeping only, and it takes a mutex: this accessor runs
    // hundreds of thousands of times a minute across ~100 threads, so leaving
    // it on for a player is both a contention point and several thousand lines
    // of log that bury the lines a bug report needs.
    if (g_diagnostics) {
        const uint64_t n = g_sourceCallCount.fetch_add(1, std::memory_order_relaxed);
        g_sourceCallers.Record(reinterpret_cast<uintptr_t>(_ReturnAddress()), g_moduleBase);
        if (g_sourceCallers.DueForReport(n, kSourceReportInterval)) {
            g_sourceCallers.Report(n);
        }
    }

    if (result == nullptr) return result;
    NoteTransformAddress(result);
    // Publishes WHERE the live transform is, so the commit hook knows what to
    // rotate; the manager holds two and a selector byte picks between them, so
    // this has to come from the engine rather than be assumed. Only written
    // when the manager has actually switched transforms: the same pointer comes
    // back on nearly every call, and storing it unconditionally from ~100
    // threads keeps one cache line bouncing between cores for nothing.
    float* const live = g_liveTransform;
    if (result != live) g_liveTransform = result;

    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_moduleBase;
    if (g_censusOn) RecordCaller(g_census, static_cast<std::uint32_t>(caller));
    // Culling and anything else that has to see the camera the frame is drawn
    // from, on a build whose accessor block is deliberately kept clean.
    if (CallerListed(g_override[2], g_trackedTransformCallers,
                     static_cast<std::uint32_t>(caller))) {
        const float* const tracked = g_trackedPub.load(std::memory_order_acquire);
        if (tracked != nullptr && result == live) {
            g_trackedServed.fetch_add(1, std::memory_order_relaxed);
            return const_cast<float*>(tracked);
        }
        return result;
    }
    const std::uint32_t swept = g_sweepCurrent.load(std::memory_order_relaxed);
    if (swept != 0 && caller == swept) {
        if (g_sweepServesTracked) {
            const float* const tracked = g_trackedPub.load(std::memory_order_acquire);
            if (tracked != nullptr && result == live) {
                g_sweepHits.fetch_add(1, std::memory_order_relaxed);
                return const_cast<float*>(tracked);
            }
            return result;
        }
        float* const aim = CleanCameraForTransform(result);
        if (aim != result) g_sweepHits.fetch_add(1, std::memory_order_relaxed);
        return aim;
    }
    {
        if (CallerListed(g_override[0], g_aimTransformCallers,
                         static_cast<std::uint32_t>(caller))) {
            float* const aim = CleanCameraForTransform(result);
            if (aim != result) g_aimTransforms.fetch_add(1, std::memory_order_relaxed);
            int idx = 0;
            for (const auto rva : g_aimTransformCallers) {
                if (caller == rva && idx < kMaxAimCallers) {
                    g_aimTransformHits[idx].fetch_add(1, std::memory_order_relaxed);
                }
                ++idx;
            }
            return aim;
        }
    }

    // Aim decoupling, and the whole reason this hook still exists.
    //
    // The renderer reads the transform MEMORY directly - a full copy-out sweep
    // moved no pixels for any accessor consumer, and an in-place write moves
    // the view - so rotating the memory reaches the renderer and nothing else
    // needs to. Gameplay is the other way round: handing GcPlayerInteract a
    // tracked camera demonstrably moved what the game thought the player was
    // looking at, so it reads through the pointer this accessor returns.
    //
    // Those two facts are what make the split possible. Rotate the memory for
    // the renderer, and hand every accessor caller a pointer to the clean rows
    // instead, so aim, raycasts and weapon fire keep using the camera the
    // player is actually pointing. Only readers come through here - the write
    // watch found the engine's one writer going via its own register, not this
    // accessor - so returning a read-only mirror is safe.
    if (g_mirrorFloats == 0) return result;
    const float* const clean = g_mirrorPub.load(std::memory_order_acquire);
    if (clean != nullptr && g_appliedPub.load(std::memory_order_acquire) != nullptr &&
        result == live) {
        g_servedClean.fetch_add(1, std::memory_order_relaxed);
        // const_cast because the accessor's signature is the engine's. Only
        // readers reach this hook - the write watch found the engine's one
        // writer going through its own register, not through here.
        return const_cast<float*>(clean);
    }
    return result;
}



int g_matrixDumps = 0;
constexpr int kMatrixDumpLimit = 3;

void DumpTransform(const float* m, const char* tag) {
    HT_LOG("Diag: %s transform (5 rows of 4):", tag);
    for (int r = 0; r < 5; ++r) {
        HT_LOG("  r%d: %12.5f %12.5f %12.5f %12.5f",
               r, m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
    }
}

// Only selected gameplay copies receive the clean transform. Projection and
// culling copies must retain the engine's tracked result.
float* Detour(void* mgr, float* outTransform) {
    float* result = g_copyOriginal(mgr, outTransform);
    if (result == nullptr) return result;

    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_moduleBase;
    if (g_censusOn) RecordCaller(g_censusCopy, static_cast<std::uint32_t>(caller));
    if (CallerListed(g_override[1], g_aimCopyCallers,
                     static_cast<std::uint32_t>(caller))) {
        const float* const clean = g_cleanPub.load(std::memory_order_acquire);
        if (clean != nullptr && g_appliedPub.load(std::memory_order_acquire) != nullptr) {
            CopyTransform(result, clean);
            g_aimCopies.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const uint64_t n = g_callCount.fetch_add(1, std::memory_order_relaxed);
    NoteInWorld();

    if (!g_diagnostics) return result;

    g_transformCallers.Record(reinterpret_cast<uintptr_t>(_ReturnAddress()), g_moduleBase);
    if (g_transformCallers.DueForReport(n, kCopyReportInterval)) {
        g_transformCallers.Report(n);
    }
    if (g_matrixDumps < kMatrixDumpLimit) {
        ++g_matrixDumps;
        DumpTransform(result, "clean");
    }

    if ((n % 600) == 0 && g_appliedPub.load(std::memory_order_acquire)) {
        const AppliedPose p = g_lastPose;
        HT_LOG("Diag: call %llu yaw=%.2f pitch=%.2f roll=%.2f pos=(%.3f %.3f %.3f)",
               (unsigned long long)n, p.yaw, p.pitch, p.roll, p.x, p.y, p.z);
    }
    return result;
}

// Logs the RTTI-discovered vtable of a class so a layout change after a game
// patch is visible in the user's log rather than inferred from a crash.
void LogVtable(void* moduleBase, const char* className, int entries) {
    cameraunlock::memory::VtableInfo info{};
    if (!cameraunlock::memory::FindVtableFromRTTI(moduleBase, className, info, entries)) {
        HT_LOG("Diag: RTTI lookup FAILED for %s", className);
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(moduleBase);
    HT_LOG("Diag: %s vtable @ RVA 0x%08llX (%d entries)", className,
           (unsigned long long)(info.vtable_address - base), info.vfunc_count);
    for (int i = 0; i < info.vfunc_count; ++i) {
        HT_LOG("  [%2d] RVA 0x%08llX", i,
               (unsigned long long)(info.vfuncs[i] - base));
    }
}

// What every frame that will not apply a pose has to do.
void StandDown() {
    g_appliedDiffers.store(false, std::memory_order_release);
    g_appliedPub.store(nullptr, std::memory_order_release);
    g_pairPub.store(nullptr, std::memory_order_release);
    g_scenePair.store(nullptr, std::memory_order_release);
}

// Edge-triggered, so a suppression that lasts a whole loading screen leaves one
// line rather than one per committed frame.
void NoteSuppressed(const char* reason) {
    g_suppressed.fetch_add(1, std::memory_order_relaxed);
    if (reason == g_lastSuppression) return;
    g_lastSuppression = reason;
    HT_LOG("Head tracking suppressed: %s.", reason ? reason : "unknown");
}

void NoteGameplayResumed() {
    if (g_lastSuppression == nullptr) return;
    g_lastSuppression = nullptr;
    HT_LOG("Head tracking active: in gameplay.");
}

// "no pose" lumped two very different causes together and read as a camera
// fault in a log that was actually saying the user had not switched tracking
// on. Say which.
void NoteNoPose() {
    if (Mod::Instance().IsEnabled()) {
        g_noPose.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_disabled.fetch_add(1, std::memory_order_relaxed);
    if (g_saidDisabled) return;
    g_saidDisabled = true;
    HT_LOG("Head tracking is INSTALLED and the engine is committing camera "
           "frames, but tracking is switched OFF. Press a key from [Hotkeys] ToggleKey (%s), "
           "or set [General] EnableOnStartup=true.",
           Mod::Instance().GetConfig().toggleKey.c_str());
}

// This frame's head pose. False when the tracker has nothing to give.
bool AcquirePose(AppliedPose& pose, bool& havePosition) {
    float yaw, pitch, roll;
    if (!Mod::Instance().GetProcessedRotation(yaw, pitch, roll)) return false;

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    havePosition = Mod::Instance().GetPositionOffset(px, py, pz);

    pose = { yaw, pitch, roll, px, py, pz };
    return true;
}

// [Debug] CleanGlobalCycle: holds the engine camera global at the clean basis
// while the live transform stays rotated.
void MirrorCleanToCycledGlobal(const float* clean) {
    if (g_cameraGlobalRva == 0) return;
    if (InterlockedCompareExchange(&g_cleanGlobal, 0, 0) == 0) return;
    // Basis rows only. The far side of row 3 has not been proved to be part of
    // the same structure, and a 20-float write would trample it.
    float* g = reinterpret_cast<float*>(g_moduleBase + g_cameraGlobalRva);
    for (int i = 0; i < 12; ++i) g[i] = clean[i];
}

}  // namespace

void RequestSweepFreeze() { g_sweepFreeze.store(true, std::memory_order_relaxed); }

void SweepThread() {
    // A press before the first candidate is served names no candidate.
    g_sweepFreeze.store(false, std::memory_order_relaxed);
    for (int i = 0;; i = (i + 1) % kSweepCount) {
        g_sweepHits.store(0, std::memory_order_relaxed);
        g_sweepHits.store(0, std::memory_order_relaxed);
        g_sweepCurrent.store(kSweepCandidates[i], std::memory_order_relaxed);
        HT_LOG("Aim sweep %d/%d: the CLEAN camera now goes to 0x%08X.",
               i + 1, kSweepCount, kSweepCandidates[i]);
        for (DWORD waited = 0; waited < kSweepHoldMs; waited += 50) {
            if (g_sweepFreeze.exchange(false, std::memory_order_relaxed)) {
                HT_LOG("=== AIM SWEEP FROZEN on 0x%08X (candidate %d of %d). "
                       "This is the caller that wants the clean camera. It "
                       "stays selected for the rest of this session.",
                       kSweepCandidates[i], i + 1, kSweepCount);
                return;
            }
        // A candidate the sweep never intercepted is not evidence about that
        // candidate - it is evidence it is not a call site on this path. Two
        // full sweeps were scored without this number and could not be read.
        HT_LOG("Aim sweep %d/%d: 0x%08X intercepted %llu time(s).",
               i + 1, kSweepCount, kSweepCandidates[i],
               (unsigned long long)g_sweepHits.load(std::memory_order_relaxed));
            Sleep(50);
        }
        HT_LOG("Aim sweep %d/%d: 0x%08X took it %llu time(s).", i + 1,
               kSweepCount, kSweepCandidates[i],
               (unsigned long long)g_sweepHits.load());
    }
}

void OnRenderPhaseBegin(void* committedCamera) {
    // Not our camera. On a build whose commit is shared between every camera
    // behaviour this is most of them, and acting on one would capture a clean
    // basis from a camera the engine had not just written.
    if (committedCamera != nullptr && g_liveTransform != nullptr &&
        committedCamera != static_cast<void*>(g_liveTransform)) {
        g_otherCamera.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t n = g_beginCalls.fetch_add(1, std::memory_order_relaxed);
    NoteCommitThread();
    const uint64_t reportInterval =
        g_diagnostics ? kFrameReportInterval : kQuietFrameReportInterval;
    if ((n % reportInterval) == 0) ReportFramePhase();

    float* const live = g_liveTransform;
    if (live == nullptr) {
        g_noLive.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // What the renderer actually projects from, which is not what the accessor
    // hands back on any build since the September re-declaration. Everything
    // below reads and writes THIS block; the accessor's own is written too, so
    // the frustum and every gameplay reader agree with the picture.
    float* const render = live + g_renderRowsFloats;

    const float* const appliedNow = g_appliedPub.load(std::memory_order_acquire);

    // Frozen: keep re-writing the same rows so the pattern a scan is hunting
    // for stays put. Capturing clean here would capture our own write.
    if (g_freeze) {
        if (appliedNow != nullptr) {
            WriteBasisAndPosition(render, appliedNow);
            if (g_renderRowsFloats != 0) WriteBasisAndPosition(live, appliedNow);
        }
        return;
    }

    // The engine commits into a transform the camera manager picks between two
    // of, and `live` is published from the accessor rather than read out of the
    // trapped instruction. If those ever diverge, what is sitting under `live`
    // is our OWN last write, and capturing it as clean would compose another
    // head rotation onto an already rotated basis - every frame, compounding.
    // That is what once sent weapon fire in random directions. Refuse the frame
    // instead: dormant is recoverable, a runaway is not.
    if (appliedNow != nullptr && g_appliedDiffers.load(std::memory_order_acquire) &&
        WrittenRowsEqual(render, appliedNow)) {
        g_notCommitted.fetch_add(1, std::memory_order_relaxed);
        if (!g_saidNotCommitted) {
            g_saidNotCommitted = true;
            HT_LOG("ERROR: the transform the accessor publishes still holds this "
                   "mod's own last write at commit time, so the engine committed "
                   "somewhere else. Head tracking is standing down rather than "
                   "compounding the rotation.");
        }
        return;
    }

    // No restore first. This runs immediately after the engine has written the
    // frame's camera itself, so the rows under `live` are already the clean
    // value - putting last frame's copy back here would overwrite a fresher one
    // with a staler one. The engine's own commit is the restore.
    CaptureClean(live, render);
    const float* const clean = g_cleanPub.load(std::memory_order_acquire);

    // Outside gameplay the camera belongs to the engine. The frontend, every
    // loading screen, the galaxy map, the death screen and any in-game menu page
    // all still commit camera frames, so without this gate the head would swing
    // the view behind a menu the player is reading. It also covers the one case
    // that is not about the camera at all: another explorer in the session.
    const char* suppression = nullptr;
    if (!TrackingApplies(suppression)) {
        StandDown();
        NoteSuppressed(suppression);
        return;
    }
    NoteGameplayResumed();

    AppliedPose pose;
    bool havePosition = false;
    if (!AcquirePose(pose, havePosition)) {
        NoteNoPose();
        StandDown();
        return;
    }
    g_lastPose = { pose.yaw, pose.pitch, pose.roll,
                   havePosition ? pose.x : 0.0f,
                   havePosition ? pose.y : 0.0f,
                   havePosition ? pose.z : 0.0f };

    float rows[kTransformFloats];
    float walkingUp[3];
    const bool walking = GetWalkingUp(walkingUp);
    static std::atomic<int> previousYawMode{-1};
    if (previousYawMode.exchange(static_cast<int>(walking), std::memory_order_relaxed) !=
        static_cast<int>(walking)) {
        HT_LOG("Yaw: %s.", walking ? "gravity-relative" : "camera-local");
    }
    ComposeTrackedRows(clean, pose.yaw, pose.pitch, pose.roll, havePosition,
                       pose.x, pose.y, pose.z, rows, walking ? walkingUp : nullptr);
    WriteBasisAndPosition(render, rows);
    // After the write, so the copy carries this frame's object with only its
    // accessor block swapped for the tracked rows.
    PublishTrackedMirror(live, rows);
    if (g_renderRowsFloats != 0) {
        // Two blocks, two readers. Normally both get the tracked rows, so the
        // frustum culls against the camera the frame is drawn from and aim is
        // decoupled per caller instead. Where no caller list exists, the blocks
        // themselves do the decoupling: the renderer projects from `render`, so
        // it gets the head rotation, while everything that asks the manager for
        // the camera lands on `live` and gets this frame's CLEAN rows.
        WriteBasisAndPosition(live, g_decoupleByRowBlock ? clean : rows);
    }
    MirrorCleanToCycledGlobal(clean);

    g_appliedDiffers.store(!WrittenRowsEqual(rows, clean), std::memory_order_release);
    PublishApplied(rows);
    PublishPair(clean, rows, n);
    g_applyCount.fetch_add(1, std::memory_order_relaxed);
}

uint64_t RenderPhaseBeginCount() { return g_beginCalls.load(); }

const float* CleanCameraRows() { return g_cleanPub.load(std::memory_order_acquire); }

const float* AppliedCameraRows() { return g_appliedPub.load(std::memory_order_acquire); }

const CameraPair* CurrentCameraPair() { return g_pairPub.load(std::memory_order_acquire); }

void NoteSceneSample() {
    const float* const live = g_liveTransform;
    if (live == nullptr || g_pairPub.load(std::memory_order_acquire) == nullptr) return;
    const float* const render = live + g_renderRowsFloats;
    const CameraPair* hit = nullptr;
    for (const CameraPair& p : g_pairSlots) {
        if (std::memcmp(p.applied, render, sizeof(p.applied)) != 0) continue;
        if (hit == nullptr || p.commit > hit->commit) hit = &p;
    }
    if (hit == nullptr) {
        g_sceneUnmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_scenePair.store(hit, std::memory_order_release);
}

// A sampled pair more than this many commits behind the newest means the
// sample site has stopped running - a camera mode that sets the frame up
// elsewhere - and holding it would freeze the reticle.
constexpr uint64_t kMaxSceneLagCommits = 4;

const CameraPair* ReticleCameraPair() {
    const CameraPair* const newest = g_pairPub.load(std::memory_order_acquire);
    if (newest == nullptr || !g_sceneSampling) return newest;
    const CameraPair* const scene = g_scenePair.load(std::memory_order_acquire);
    if (scene == nullptr || newest->commit - scene->commit > kMaxSceneLagCommits) {
        g_sceneStale.fetch_add(1, std::memory_order_relaxed);
        return newest;
    }
    return scene;
}

void SceneSampleStats(uint64_t& unmatched, uint64_t& stale) {
    unmatched = g_sceneUnmatched.load(std::memory_order_relaxed);
    stale = g_sceneStale.load(std::memory_order_relaxed);
}

bool FreezeWithYaw(float yawDegrees) {
    const float* const clean = g_cleanPub.load(std::memory_order_acquire);
    if (clean == nullptr) return false;
    // The freeze is NOT dropped first. Clearing it here let the commit thread
    // fall through to CaptureClean while the live transform still held the
    // previous pass's rotated pattern, and the two-pass differential scan then
    // measured every angle against a reference already tens of degrees off.
    float rows[kTransformFloats];
    CopyTransform(rows, clean);
    float h[9];
    BuildHeadRot3x3(-yawDegrees, 0.0f, 0.0f, h);
    PreMultiplyRotation(rows, h);
    g_appliedDiffers.store(!WrittenRowsEqual(rows, clean), std::memory_order_release);
    PublishApplied(rows);
    g_freeze = true;
    return true;
}

void UnfreezeAppliedRows() { g_freeze = false; }

// Since the September re-declaration the accessor hands back the camera OBJECT,
// and a caller reads whole-object fields through it - the render rows at +0x50,
// the field of view at +0xF0. Handing such a caller the bare 20-float clean rows
// gives it whatever follows them in memory, which is what made every per-caller
// search on Game Pass "blank the frame" and read as the aim ray not coming
// through the accessor at all. So it gets a copy of the live object taken NOW,
// not at the commit, with only the rows swapped: a snapshot from the commit went
// stale in a ship, where everything moves between commits.
//
// A ring per thread because the caller reads through the pointer after we
// return, and the thread may ask again before it has finished with the last one.
constexpr int kCameraObjectFloats = 0x100 / static_cast<int>(sizeof(float));
constexpr int kCleanObjectSlots = 8;
thread_local float t_cleanObjects[kCleanObjectSlots][kCameraObjectFloats];
thread_local unsigned t_cleanObjectTurn = 0;

float* CleanCameraForTransform(float* transform) {
    const float* const clean = g_cleanPub.load(std::memory_order_acquire);
    if (transform != g_liveTransform || clean == nullptr ||
        g_appliedPub.load(std::memory_order_acquire) == nullptr) return transform;
    if (g_renderRowsFloats == 0) return const_cast<float*>(clean);
    float* const dst = t_cleanObjects[t_cleanObjectTurn++ % kCleanObjectSlots];
    std::memcpy(dst, transform, sizeof(t_cleanObjects[0]));
    CopyTransform(dst, clean);
    CopyTransform(dst + g_renderRowsFloats, clean);
    return dst;
}

float* WeaponCameraForTransform(float* transform) {
    const CameraPair* const pair = g_pairPub.load(std::memory_order_acquire);
    if (transform != g_liveTransform || pair == nullptr) return transform;
    float rows[kTransformFloats];
    CopyTransform(rows, pair->clean);
    rows[12] = pair->applied[12];
    rows[13] = pair->applied[13];
    rows[14] = pair->applied[14];
    float* const dst = t_cleanObjects[t_cleanObjectTurn++ % kCleanObjectSlots];
    if (g_renderRowsFloats == 0) {
        CopyTransform(dst, rows);
        return dst;
    }
    std::memcpy(dst, transform, sizeof(t_cleanObjects[0]));
    CopyTransform(dst, rows);
    CopyTransform(dst + g_renderRowsFloats, rows);
    return dst;
}

int ClassifyCameraBasis(const float* rows) {
    const float* const applied = g_appliedPub.load(std::memory_order_acquire);
    const float* const clean = g_cleanPub.load(std::memory_order_acquire);
    if (applied == nullptr || clean == nullptr) return 0;
    if (BasisEqual(rows, clean)) return 1;
    if (BasisEqual(rows, applied)) return 2;
    return 0;
}

float LiveCameraFovDegrees() {
    float field = 0.0f;
    float scale = 0.0f;
    if (!LiveCameraFovTerms(field, scale)) return 0.0f;
    return 0.5f * scale * field;
}

bool LiveCameraFovTerms(float& field, float& scale) {
    const float* const live = g_liveTransform;
    if (live == nullptr || g_fovScaleRva == 0 || g_cameraFovFromTransform == 0) {
        return false;
    }
    field = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(live) + g_cameraFovFromTransform);
    scale = *reinterpret_cast<const float*>(g_moduleBase + g_fovScaleRva);
    return true;
}

void CountPresent() { g_endCalls.fetch_add(1, std::memory_order_relaxed); }

CameraHook& CameraHook::Instance() {
    static CameraHook s;
    return s;
}

void CameraHook::Install() {
    if (m_installed) return;
    m_installed = true;

    void* moduleBase = reinterpret_cast<void*>(GetModuleHandleW(nullptr));
    g_moduleBase = reinterpret_cast<uintptr_t>(moduleBase);

    // Latched before the first EnableHook below, because a detour can fire the
    // instant its hook goes live.
    const Config& cfg = Mod::Instance().GetConfig();
    g_diagnostics = cfg.diagnostics;
    g_transformCallers.SetVerbose(cfg.diagnostics);
    g_sourceCallers.SetVerbose(cfg.diagnostics);

    cameraunlock::memory::PeFingerprint running{};
    const BuildProfile* primary = nullptr;
    const BuildProfile* profile = SelectProfile(moduleBase, &running, &primary);

    HT_LOG("PE fingerprint: tds=0x%08X size=0x%08X csum=0x%08X",
           running.TimeDateStamp, running.SizeOfImage, running.CheckSum);

    // The failsafe, and it comes before ANY modification of the process. A
    // build the mod has never seen may not have the same camera-manager layout,
    // and hooking, arming a breakpoint or stamping a probe against RVAs derived
    // from a different binary is worse than no head tracking. Everything below
    // this point patches the game; nothing above it does.
    if (profile == nullptr) {
        using cameraunlock::memory::ClassifyMismatch;
        using cameraunlock::memory::FingerprintMismatch;
        const char* dir = "unknown";
        if (primary) {
            switch (ClassifyMismatch(running, primary->fingerprint)) {
                case FingerprintMismatch::Newer:
                    dir = "newer than this mod knows about; check the releases "
                          "page for an updated mod";
                    break;
                case FingerprintMismatch::Older:
                    dir = "older than any known build; let the store finish "
                          "updating";
                    break;
                case FingerprintMismatch::Differs:
                    dir = "repacked or tampered; this mod will not engage on a "
                          "modified binary";
                    break;
            }
        }
        HT_LOG("This EXE has no build profile (%s). Nothing has been written "
               "into the game. Trying to recognise the engine structure in it "
               "before standing down.", dir);

        // A pinned profile is always preferred and this only runs when there is
        // none. Anything it cannot establish to its own satisfaction leaves the
        // mod exactly as dormant as it was a moment ago.
        static BuildProfile resolved{};
        profile = ResolveProfileFromImage(moduleBase, resolved);
        if (profile == nullptr) {
            HT_LOG("Head tracking DORMANT: the engine structure in this EXE did "
                   "not match what this mod knows how to recognise either. No "
                   "camera hook, breakpoint or probe is installed and nothing "
                   "is written into the game; only the frame-phase present "
                   "counter, which is resolved by export name and touches no "
                   "pinned address, is live.");
            return;
        }
        HT_LOG("Head tracking is running on addresses RESOLVED from this EXE, "
               "not pinned ones, so it has never been confirmed in a running "
               "game on this build. The view should follow your head; aim "
               "decoupling and reticle correction are off. If anything looks "
               "wrong, turn the mod off with End and report this log.");
    }

    if (cfg.diagnostics) {
        LogVtable(moduleBase, "cGcCameraManager", 32);
        LogVtable(moduleBase, "cGcApplicationSimulationState", 16);
        LogVtable(moduleBase, "cGcCameraBehaviourFirstPerson", 16);
        LogVtable(moduleBase, "cGcApplication", 16);
    }

    // Resolve the hook target by RTTI rather than by RVA: the vtable slot is a
    // class-layout property that survives the code motion an ordinary patch
    // causes, so the mod keeps working on builds it has never seen.
    const int copySlot = profile->copyTransformVfunc;
    const int activeSlot = profile->activeTransformVfunc;
    cameraunlock::memory::VtableInfo mgr{};
    if (!cameraunlock::memory::FindVtableFromRTTI(moduleBase, "cGcCameraManager",
                                                  mgr, copySlot + 1)) {
        HT_LOG("Camera hook DORMANT: cGcCameraManager RTTI not found.");
        return;
    }
    if (mgr.vfunc_count <= copySlot) {
        HT_LOG("Camera hook DORMANT: cGcCameraManager vtable has %d entries, "
               "need at least %d.", mgr.vfunc_count, copySlot + 1);
        return;
    }

    g_copyTarget = reinterpret_cast<void*>(mgr.vfuncs[copySlot]);
    const uintptr_t targetRva = mgr.vfuncs[copySlot] - g_moduleBase;

    HT_LOG("Build profile matched: %s.", profile->name);
    // Before the diagnostics below, which decide whether to arm on it.
    g_decoupleByRowBlock = profile->decoupleByRowBlock != 0;
    g_mirrorFloats = static_cast<int>(profile->accessorMirrorBytes / sizeof(float));
    g_censusOn = cfg.callerCensus;
    g_overridesOn = cfg.liveCallerOverrides;
    if (g_overridesOn) {
        HT_LOG("Live caller overrides on: [Debug] AimTransformCallers, AimCopyCallers "
               "and TrackedTransformCallers apply in %d seconds, and again whenever the "
               "ini changes.",
               kCensusDumpMs / 1000);
    }
    if (g_censusOn || g_overridesOn) {
        std::thread(DiagnosticThread).detach();
    }
    if (g_censusOn) {
        HT_LOG("Caller census on: every accessor caller will be counted, "
               "dumped every %d seconds. Two dumps either side of an action "
               "name the callers on that path.", kCensusDumpMs / 1000);
    }
    if (cfg.cullCallerSweep) {
        g_sweepServesTracked = true;
        HT_LOG("Cull sweep ENABLED: %d candidates, %d ms each. One at a time is "
               "served the TRACKED camera while the rest of the build stays "
               "clean, so whichever of them decides visibility shows up as the "
               "frame edges filling back in. A key from [Debug] SweepFreezeKey "
               "(%s) freezes it on the candidate it is serving.",
               kSweepCount, (int)kSweepHoldMs, cfg.sweepFreezeKey.c_str());
        std::thread(SweepThread).detach();
    } else if (cfg.aimCallerSweep) {
        HT_LOG("Aim sweep ENABLED: %d candidates, %d ms each, about %d seconds "
               "for a full pass. Hold whatever is aiming wrongly and press "
               "a key from [Debug] SweepFreezeKey (%s) the moment it snaps onto "
               "the crosshair - that freezes it and names the caller.",
               kSweepCount, (int)kSweepHoldMs,
               kSweepCount * (int)kSweepHoldMs / 1000, cfg.sweepFreezeKey.c_str());
        std::thread(SweepThread).detach();
    }
    g_trackedTransformCallers = profile->trackedTransformCallers;
    g_mirrorObjectFloats = kMaxMirrorFloats;
    if (g_trackedTransformCallers.size() != 0) {
        HT_LOG("Culling: %u caller(s) are served the head-tracked camera so the "
               "frustum follows your head; everything else keeps the clean one.",
               static_cast<unsigned>(g_trackedTransformCallers.size()));
    }
    g_aimCopyCallers = profile->aimCopyCallers;
    g_aimTransformCallers = profile->aimTransformCallers;
    if (g_aimCopyCallers.size() != 0 || g_aimTransformCallers.size() != 0) {
        HT_LOG("Aim decoupling: %u copy callers and %u transform callers; "
               "other camera readers retain the live tracked camera.",
               static_cast<unsigned>(g_aimCopyCallers.size()),
               static_cast<unsigned>(g_aimTransformCallers.size()));
    }
    if (g_mirrorFloats > kMaxMirrorFloats) g_mirrorFloats = kMaxMirrorFloats;
    if (g_decoupleByRowBlock) {
        HT_LOG("Aim decoupling is ON, through the camera's own row blocks: the "
               "renderer projects from the block this mod turns, and everything "
               "that asks the engine for the camera reads the block it keeps "
               "clean.%s",
               g_trackedTransformCallers.size() != 0
                   ? " Culling is served the turned camera by name, so the "
                     "frustum follows your head."
                   : " Culling reads the clean block too, so scenery can pop at "
                     "the edges of a hard head turn.");
    } else if (g_mirrorFloats == 0 && g_aimCopyCallers.size() == 0 &&
               g_aimTransformCallers.size() == 0) {
        HT_LOG("Aim decoupling is OFF on this build: the engine reads the camera "
               "through the accessor on the render path as well as the gameplay "
               "path here, so handing it this mod's own copy breaks what is "
               "drawn. Head tracking moves the view and the aim follows it, as "
               "it would without the mod.");
    }
    g_fovScaleRva = profile->fovScaleRva;
    g_cameraFovFromTransform = profile->cameraFovFromTransform;
    g_renderRowsFloats =
        static_cast<int>(profile->renderRowsFromTransform / sizeof(float));
    g_cameraGlobalRva = profile->cameraGlobalRva;
    if (profile->cameraTransformRva != targetRva) {
        HT_LOG("Camera hook DORMANT: vtable[%d] resolves to RVA 0x%08llX but "
               "profile %s pins 0x%08X - the class layout moved.",
               copySlot, (unsigned long long)targetRva,
               profile->name, profile->cameraTransformRva);
        return;
    }

    auto& hm = cameraunlock::hooks::HookManager::Instance();
    if (hm.Initialize() != cameraunlock::hooks::HookStatus::Ok && !hm.IsInitialized()) {
        HT_LOG("ERROR: MinHook init failed - head tracking inactive.");
        return;
    }

    auto st = hm.CreateHook(g_copyTarget, reinterpret_cast<void*>(&Detour),
                            reinterpret_cast<void**>(&g_copyOriginal));
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: CreateHook failed (%s).",
               cameraunlock::hooks::HookStatusToString(st));
        return;
    }
    st = hm.EnableHook(g_copyTarget);
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: EnableHook failed (%s).",
               cameraunlock::hooks::HookStatusToString(st));
        hm.RemoveHook(g_copyTarget);
        return;
    }

    HT_LOG("Camera hook installed at RVA 0x%08llX.", (unsigned long long)targetRva);

    if (cfg.writeWatch) {
        HT_LOG("Diag: write watch armed - it will burst once the scene has settled.");
        StartWriteWatch();
    }
    if (cfg.readWatch) {
        HT_LOG("Diag: read watch armed - it will burst once the scene has settled.");
        StartReadWatch(cfg.readWatchOffset);
    }
    if (cfg.cleanGlobalCycle && g_cameraGlobalRva == 0) {
        HT_LOG("Diag: clean-global cycle NOT armed - this build profile pins no "
               "camera-global RVA, and the probe writes through it.");
    } else if (cfg.cleanGlobalCycle) {
        HT_LOG("Diag: clean-global cycle armed - the engine camera global at RVA "
               "0x%08X is held CLEAN for 15s at a time while the live transform "
               "stays rotated. Watch the multitool, not the world.",
               g_cameraGlobalRva);
        std::thread([] {
            Sleep(20000);
            for (;;) {
                InterlockedExchange(&g_cleanGlobal, 1);
                HT_LOG("CleanGlobal: holding it clean.");
                Sleep(15000);
                InterlockedExchange(&g_cleanGlobal, 0);
                HT_LOG("CleanGlobal: released (baseline).");
                Sleep(15000);
            }
        }).detach();
    }
    if (cfg.crosshairProbe) {
        HT_LOG("Diag: crosshair probe armed - it scans for the crosshair's screen "
               "position once the player is in world.");
        StartCrosshairProbe();
    }
    if (cfg.weaponProbe) {
        HT_LOG("Diag: weapon probe armed - it reports once the camera has committed.");
        StartWeaponProbe();
    }
    GameStateOffsets gs{};
    gs.appStateSlotRva = profile->appStateSlotRva;
    gs.gameGlobalsPtrRva = profile->gameGlobalsPtrRva;
    gs.netPlayerSlotsBegin = profile->netPlayerSlotsBegin;
    gs.netPlayerSlotsEnd = profile->netPlayerSlotsEnd;
    gs.netPlayerConnectedByte = profile->netPlayerConnectedByte;
    gs.menuPageModeOffset = profile->menuPageModeOffset;
    gs.playerFromGlobals = profile->playerFromGlobals;
    gs.playerShipRva = profile->playerShipRva;
    InstallGameStateProbe(gs);
    LogFramePhaseStatus();
    // The read watch owns the same debug register as the third-person site, so
    // a session running that diagnostic keeps the first-person site only.
    const std::uint32_t thirdPersonCommit =
        cfg.readWatch ? 0u : profile->cameraCommitThirdPersonRva;
    if (cfg.readWatch && profile->cameraCommitThirdPersonRva != 0) {
        HT_LOG("Diag: read watch armed, so the third-person commit site is not - "
               "they share a debug register.");
    }
    InstallCommitHook(profile->cameraCommitRva, thirdPersonCommit,
                      profile->commitCameraReg);
    {
        std::uint32_t sampleRva = profile->sceneSampleRva;
        if (cfg.sceneSampleRva.size() == 1) {
            sampleRva = cfg.sceneSampleRva.front();
        } else if (!cfg.sceneSampleRva.empty()) {
            HT_LOG("[Debug] SceneSampleRva lists %zu addresses and takes one, so the "
                   "build's own is used.", cfg.sceneSampleRva.size());
        }
        g_sceneSampling = sampleRva != 0;
        InstallSceneSampleHook(sampleRva);
    }
    // Aim is decoupled either by naming the callers that get the clean camera
    // or by keeping the clean rows in the accessor block, which reaches a
    // reader that never goes through the accessor at all. Both count.
    InstallWeaponDecouple(profile->weaponCameraRva,
                          profile->decoupleByRowBlock != 0 ||
                              profile->aimCopyCallers.size() != 0 ||
                              profile->aimTransformCallers.size() != 0);
    InstallScreenProjection(profile->worldToScreenRva);
    InstallReticle(profile->nguiFindElementRva, profile->reticleLookupReturnRva,
                   profile->gfxManagerPtrRva, cfg.reticleSweep,
                   profile->nguiRenderRva, profile->reticleRenderReturnRva);

    g_sourceTarget = reinterpret_cast<void*>(mgr.vfuncs[activeSlot]);
    st = hm.CreateHook(g_sourceTarget, reinterpret_cast<void*>(&SourceDetour),
                       reinterpret_cast<void**>(&g_sourceOriginal));
    if (st == cameraunlock::hooks::HookStatus::Ok) {
        st = hm.EnableHook(g_sourceTarget);
    }
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: camera accessor hook FAILED (%s) - head tracking inactive.",
               cameraunlock::hooks::HookStatusToString(st));
        hm.RemoveHook(g_sourceTarget);
        return;
    }
    HT_LOG("Camera accessor hooked at RVA 0x%08llX.",
           (unsigned long long)(mgr.vfuncs[activeSlot] - g_moduleBase));
}


}  // namespace NMSHT
