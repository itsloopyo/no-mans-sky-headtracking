#include "pch.h"
#include "reticle.h"

#include "aim_projection.h"
#include "camera_hook.h"
#include "scene_sample.h"
#include "core/debug_log.h"
#include "core/game_state.h"

#include <atomic>
#include <cmath>
#include <intrin.h>
#include <vector>
#include <cstring>

#include <cameraunlock/hooks/hook_manager.h>

namespace NMSHT {

namespace {

// cTkNGui's by-name element lookup:
//   void* Find(void* layer, const TkID* name, char createIfMissing)
// It hashes the 16-byte name, walks the layer's element map and returns the
// element, or a shared dummy when `createIfMissing` is set and the name is
// absent.
using FindElementFn = void* (*)(void* layer, const void* name, char createIfMissing);

FindElementFn g_original = nullptr;
void* g_target = nullptr;
uintptr_t g_returnSite = 0;
bool g_sweep = false;
using RenderGuiFn = void (*)(void*);
RenderGuiFn g_renderOriginal = nullptr;
uintptr_t g_renderReturnSite = 0;

// An NGui element and the layout block the MXML gave it.
//   element + 0x40  parent
//   element + 0x48  layout data
//   data    + 0x2c  position, x then y
//   data    + 0x3b  non-zero when that position is a PERCENTAGE of the parent
//   data    + 0x61  hidden
constexpr std::ptrdiff_t kElementParent = 0x40;
constexpr std::ptrdiff_t kElementData = 0x48;
constexpr std::ptrdiff_t kDataHeight = 0x24;
constexpr std::ptrdiff_t kDataPositionX = 0x2c;
constexpr std::ptrdiff_t kDataPositionY = 0x30;
constexpr std::ptrdiff_t kDataWidth = 0x34;
constexpr std::ptrdiff_t kDataHeightRelative = 0x3e;
constexpr std::ptrdiff_t kDataRelative = 0x3b;
constexpr std::ptrdiff_t kDataWidthRelative = 0x41;

// The position is written rather than the runtime offset at `element + 0x20`
// that the toolkit's GetPosition adds to it. The two are summed, so either
// would serve the arithmetic - but the element render resolves an absolutely
// positioned element through a path that never reads the offset, and reads the
// position in every path there is. One field, every layout mode.
//
// The MXML value is captured once per element and the correction written on top
// of it, so nothing here ever reads back its own write and compounds.

// The gui the crosshair lives in is drawn into a fixed reference canvas rather
// than into the backbuffer: the draw pushes (1920, 1080) as its render size
// whatever resolution the game runs at, and the canvas is stretched to the
// frame. Every size below resolves into that space.
constexpr float kReferenceWidth = 1920.0f;
constexpr float kReferenceHeight = 1080.0f;

// How many corrections apart the geometry line is written. About every five
// seconds at a HUD frame rate, which is often enough to catch the pose moving
// and rare enough not to bury the log.
constexpr uint64_t kGeometryReportInterval = 500;

std::atomic<uint64_t> g_placements{0};
std::atomic<float> g_lastX{0.0f};
std::atomic<float> g_lastY{0.0f};

// The layout position each reticle element had before this mod touched it.
//
// A table rather than a single entry, because the reticle's name is the equipped
// weapon's: switching multi-tool modes swaps the element and swaps back, and
// re-reading its position on the way back would read this mod's own last
// correction and bake it in for good.
//
// Only ever written to an element the lookup has just handed over, which is live
// by construction. Nothing is written to a remembered pointer on its own, so a
// HUD teardown cannot turn a stale entry into a write into freed memory.
constexpr int kMaxRemembered = 8;
struct RememberedBase {
    void* element;
    float x, y;
};
RememberedBase g_bases[kMaxRemembered]{};
int g_baseCount = 0;

// EXACTLY ONE element is ever corrected: the first reticle the HUD asks for.
// Everything else it asks for is put back where its layout had it.
//
// The lookup hands back more than one - in a ship the HUD wants LASER and then
// SHIPGUN, siblings under the RETICLE layer - and correcting more than one moves
// the crosshair once per element. Measured in the cockpit at 8 degrees of head
// yaw: correcting two moved it 442 px against the 222 px the world had actually
// moved, and past about ten degrees it leaves the frame altogether. Correcting
// the first alone put it on 221 px against 222, and 450/454 and 700/695 at 16
// and 24 degrees.
//
// Their common parent is NOT the answer, tempting as one write for the whole
// group is: moving the layer moves what is drawn inside it twice as far, 442 px
// again on the same test. Nothing in the layout, the size flags or the parent
// chain accounts for that, so this does not try to reason about which element is
// worth what - it corrects the one the HUD asked for first and leaves the rest
// alone.
void* g_active = nullptr;
std::atomic<uint64_t> g_applied{0};
std::atomic<uint64_t> g_refused{0};

// The remembered layout position for this element, or null when it must be left
// alone - because another element is currently being corrected, or because the
// table is full and this mod cannot vouch for the position it would write.
//
// The table outlives the choice of element on purpose: an element that is
// dropped and later picked up again keeps our last correction in its position
// field, and re-reading that as its layout position would bake the correction
// in for good.
const RememberedBase* BaseFor(void* element, const float* position) {
    if (g_active == nullptr) g_active = element;
    if (element != g_active) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    g_applied.fetch_add(1, std::memory_order_relaxed);

    for (int i = 0; i < g_baseCount; ++i) {
        if (g_bases[i].element == element) return &g_bases[i];
    }
    if (g_baseCount >= kMaxRemembered) {
        static bool said = false;
        if (!said) {
            said = true;
            HT_LOG("Reticle: more than %d reticle elements have been seen, so "
                   "this one is left where the game draws it.", kMaxRemembered);
        }
        return nullptr;
    }
    RememberedBase& slot = g_bases[g_baseCount++];
    slot.element = element;
    slot.x = position[0];
    slot.y = position[1];

    const char* const data = *reinterpret_cast<const char* const*>(
        static_cast<char*>(element) + kElementData);
    char name[17] = {0};
    if (data != nullptr) {
        for (int i = 0; i < 16; ++i) {
            const char c = data[0x48 + i];
            if (c == 0) break;
            name[i] = c;
        }
    }
    HT_LOG("Reticle: correcting element %p (%s), layout (%.1f%%, %.1f%%).",
           element, name[0] ? name : "<unnamed>", position[0], position[1]);
    return &slot;
}

// The element's own size in reference pixels, by the toolkit's own rule: a size
// flagged relative is a percentage of its parent's, and the root's parent is the
// reference canvas.
//
// The correction is written as a percentage of the PARENT, so this is what turns
// a fraction of the frame into one. Taking the parent to span the frame is right
// for the on-foot reticle and wrong for anything hung under a smaller container,
// and being wrong there scales every correction - which is the shape of an
// over-correction that no amount of checking the projection would ever find.
float SizeOf(void* element, bool horizontal, int depth) {
    constexpr int kMaxDepth = 8;
    const float rootSize = horizontal ? kReferenceWidth : kReferenceHeight;
    if (element == nullptr || depth >= kMaxDepth) return rootSize;

    const char* const data = *reinterpret_cast<const char* const*>(
        static_cast<char*>(element) + kElementData);
    if (data == nullptr) return rootSize;

    const float value = *reinterpret_cast<const float*>(
        data + (horizontal ? kDataWidth : kDataHeight));
    if (data[horizontal ? kDataWidthRelative : kDataHeightRelative] == 0) {
        return value;   // already reference pixels
    }

    void* const parent = *reinterpret_cast<void* const*>(
        static_cast<char*>(element) + kElementParent);
    const float parentSize =
        (parent == nullptr) ? rootSize : SizeOf(parent, horizontal, depth + 1);
    return parentSize * value * 0.01f;
}

// Where an element actually sits in the reference canvas, walking its chain.
//
// A child is laid out inside its parent's rect, so its screen position is the
// parent's left edge plus its own position as a share of the parent's size.
// Ancestors therefore CARRY, and that is the whole reason this exists: in a
// ship the engine already moves the crosshair's container to follow the aim,
// so an offset nudged onto the element on top of that lands at twice the
// distance and, past about ten degrees, off the side of the screen.
float ScreenPosOf(void* element, bool horizontal, int depth) {
    constexpr int kMaxDepth = 8;
    const float canvas = horizontal ? kReferenceWidth : kReferenceHeight;
    if (element == nullptr || depth >= kMaxDepth) return 0.5f * canvas;

    const char* const data = *reinterpret_cast<const char* const*>(
        static_cast<char*>(element) + kElementData);
    if (data == nullptr) return 0.5f * canvas;

    void* const parent = *reinterpret_cast<void* const*>(
        static_cast<char*>(element) + kElementParent);
    const float parentSize = SizeOf(parent, horizontal, depth);
    const float parentOrigin =
        (parent == nullptr) ? 0.0f
                            : ScreenPosOf(parent, horizontal, depth + 1) - 0.5f * parentSize;

    const float pos = *reinterpret_cast<const float*>(
        data + (horizontal ? kDataPositionX : kDataPositionY));
    if (data[kDataRelative] == 0) return parentOrigin + pos;   // already pixels
    return parentOrigin + pos * parentSize * 0.01f;
}

float* PositionOf(void* element) {
    char* const data = *reinterpret_cast<char**>(
        static_cast<char*>(element) + kElementData);
    if (data == nullptr) return nullptr;
    // An absolutely positioned element is placed from its layout position in
    // pixels, and a percentage written into it would put the reticle within a
    // hundred pixels of the top-left corner. Refuse rather than move it there.
    if (data[kDataRelative] == 0) return nullptr;
    return reinterpret_cast<float*>(data + kDataPositionX);
}

static_assert(kDataPositionY - kDataPositionX == sizeof(float),
              "the position is one adjacent pair of floats");

// The aspect the engine's own projection divides by, read from the graphics
// manager it reads it from. This is the entire scale on the horizontal
// correction - the engine widens the vertical field of view into the horizontal
// one by exactly this ratio - so it comes from the engine rather than from the
// game window. Those are different rectangles: a window carries a caption and a
// border, and the engine renders into whatever size the upscaler asked for.
constexpr std::ptrdiff_t kGfxRenderWidth = 0x34;
constexpr std::ptrdiff_t kGfxRenderHeight = 0x38;
std::uint32_t g_gfxManagerPtrRva = 0;
uintptr_t g_moduleBase = 0;

bool EngineAspect(float& aspect) {
    if (g_gfxManagerPtrRva == 0) return false;
    const char* const gfx = *reinterpret_cast<const char* const*>(
        g_moduleBase + g_gfxManagerPtrRva);
    if (gfx == nullptr) return false;
    const int w = *reinterpret_cast<const int*>(gfx + kGfxRenderWidth);
    const int h = *reinterpret_cast<const int*>(gfx + kGfxRenderHeight);
    if (w <= 0 || h <= 0) return false;
    aspect = static_cast<float>(w) / static_cast<float>(h);
    return true;
}

// A large, slow, obviously visible square walked around the crosshair, for the
// one question a projection cannot answer on its own: whether writing this
// field moves the thing on screen at all, and which way its axes run. A quarter
// of the frame is far enough that nobody has to look for it, and each corner is
// held three seconds and logged, so a screenshot can be read against the value
// that produced it.
struct SweepStep {
    const char* name;
    float x, y;
};

void SweepOffset(float& x, float& y) {
    constexpr float kAmplitude = 25.0f;   // percent: a quarter of the frame
    constexpr unsigned long long kHoldMs = 3000;
    static const SweepStep kSteps[] = {
        { "right", kAmplitude, 0.0f },
        { "down",  0.0f, kAmplitude },
        { "left",  -kAmplitude, 0.0f },
        { "up",    0.0f, -kAmplitude },
    };
    constexpr int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

    const int step = static_cast<int>((GetTickCount64() / kHoldMs) % kStepCount);
    x = kSteps[step].x;
    y = kSteps[step].y;

    static int announced = -1;
    if (step == announced) return;
    announced = step;
    HT_LOG("ReticleSweep: moving the crosshair %s by %.0f%% of the frame. If it "
           "is not visibly off centre in that direction, the reticle is not "
           "placed from this field on this build.", kSteps[step].name, kAmplitude);
}

// Every term of the correction on ONE line, because each of them fits more than
// one fault and reading them off separate lines is how a scale error gets
// mistaken for a sign error. `swing` is the angle between the clean camera's
// forward and the tracked one's - the view movement the correction is answering
// - so the whole thing can be checked by hand: for a pure yaw,
// pct.x = -tan(swing) / (tanV * aspect) * 50.
// How the reticle's reads line up with the commits, over one report interval.
// `sameCommit` placements reused the previous placement's commit and `skipped`
// counts commits that no placement ever saw. On a build whose reticle follows
// the newest commit, both are the reticle stepping against the picture.
struct SyncStats {
    uint64_t lastCommit = 0;
    uint64_t sameCommit = 0;
    uint64_t skipped = 0;
    double ageSumUs = 0.0;
    double ageMaxUs = 0.0;
    uint64_t samples = 0;
};
SyncStats g_sync;

void NoteSync(const CameraPair& pair) {
    if (pair.commit == g_sync.lastCommit) {
        ++g_sync.sameCommit;
    } else if (g_sync.lastCommit != 0 && pair.commit > g_sync.lastCommit + 1) {
        g_sync.skipped += pair.commit - g_sync.lastCommit - 1;
    }
    g_sync.lastCommit = pair.commit;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double age = double(now.QuadPart - pair.qpc) * 1e6 / double(freq.QuadPart);
    g_sync.ageSumUs += age;
    if (age > g_sync.ageMaxUs) g_sync.ageMaxUs = age;
    ++g_sync.samples;
}

void ReportSync() {
    if (g_sync.samples == 0) return;
    uint64_t unmatched = 0, stale = 0;
    SceneSampleStats(unmatched, stale);
    HT_LOG("AIMSYNC placements=%llu sameCommit=%llu skippedCommits=%llu "
           "age mean=%.0fus max=%.0fus hudTid=%lu | scene samples=%llu "
           "unmatched=%llu staleFallbacks=%llu",
           (unsigned long long)g_sync.samples, (unsigned long long)g_sync.sameCommit,
           (unsigned long long)g_sync.skipped,
           g_sync.ageSumUs / double(g_sync.samples), g_sync.ageMaxUs, GetCurrentThreadId(),
           (unsigned long long)SceneSampleCount(), (unsigned long long)unmatched,
           (unsigned long long)stale);
    const uint64_t last = g_sync.lastCommit;
    g_sync = SyncStats{};
    g_sync.lastCommit = last;
}

void ReportGeometry(const float* clean, const float* applied, float fov,
                    float aspect, float parentW, float parentH,
                    const AimScreenOffset& aim, float x, float y) {
    const float dot = -(clean[8] * applied[8] + clean[9] * applied[9] +
                        clean[10] * applied[10]);
    const float swing = std::acos(std::fmin(1.0f, std::fmax(-1.0f, -dot))) * kRadToDeg;
    float field = 0.0f, scale = 0.0f;
    LiveCameraFovTerms(field, scale);
    HT_LOG("AIMGEO swing=%.2fdeg fov=%.2fv (field %.1f x scale %.4f) aspect=%.4f "
           "tanHalfV=%.4f parent=(%.0f %.0f) | ndc=(%.4f %.4f) pct=(%.2f %.2f)",
           swing, fov, field, scale, aspect, std::tan(0.5f * fov * kDegToRad),
           parentW, parentH, aim.ndcX, aim.ndcY, x, y);
    HT_LOG("AIMGEO placements=%llu", (unsigned long long)g_placements.load());
}

// This frame's correction, as a percentage of the frame, or (0, 0) whenever
// there is nothing to correct. Centre is the right answer for every one of
// those cases - tracking off, tracking suppressed, the camera not yet
// committed, the head exactly centred - because the frame is then drawn from
// the camera the game aims down.
// Fills x and y with either a position to write outright (`absolute`), or a
// step to add to the layout position. Centring is the second kind: with nothing
// to correct the element belongs exactly where its layout put it, and writing an
// absolute centre there would undo whatever its ancestors are doing.
void ComputeOffset(void* element, float& x, float& y, bool& absolute,
                   bool* visible = nullptr) {
    x = 0.0f;
    y = 0.0f;
    absolute = false;
    if (visible != nullptr) *visible = true;

    if (g_sweep) {
        SweepOffset(x, y);
        return;
    }

    // Snapshotted once. It goes null the moment the commit hook stops applying
    // a rotation - a menu opening is enough - and this runs on the HUD thread,
    // which cannot see that transition. Re-reading it mid-function is the null
    // dereference that took the game down from the projection detour.
    const CameraPair* const pair = ReticleCameraPair();
    if (pair == nullptr) return;
    NoteSync(*pair);
    const float* const clean = pair->clean;
    const float* const applied = pair->applied;

    float aspect = 0.0f;
    if (!EngineAspect(aspect)) {
        static bool said = false;
        if (!said) {
            said = true;
            HT_LOG("Reticle: the engine's render size is not readable, so the "
                   "crosshair is left where the game draws it rather than moved "
                   "against a guessed aspect ratio.");
        }
        return;
    }

    const float fov = LiveCameraFovDegrees();
    const AimScreenOffset aim =
        ProjectCleanAimIntoTrackedView(clean, applied, fov, aspect);
    if (!aim.valid) {
        if (visible != nullptr) *visible = false;
        return;
    }

    // Where the shot crosses the frame, in the reference canvas. NGui's vertical
    // axis runs DOWN from the top left, so a positive normalised height - the
    // top of the frame - is a smaller y here.
    const float targetX = 0.5f * kReferenceWidth * (1.0f + aim.ndcX);
    const float targetY = 0.5f * kReferenceHeight * (1.0f - aim.ndcY);

    // Placed ABSOLUTELY rather than nudged off the layout position, because the
    // layout position is not where the element ends up: its ancestors carry it,
    // and in a ship the engine moves that container to follow the aim itself.
    // Adding a correction on top of one the engine has already applied doubles
    // it - measured in the cockpit at 8 degrees of head yaw, where the crosshair
    // sat 442 px from centre against the 221 px the view had actually moved.
    // Resolving the chain and writing an absolute target is right whether the
    // engine moves the crosshair or leaves it alone.
    void* const parent = *reinterpret_cast<void* const*>(
        static_cast<char*>(element) + kElementParent);
    const float parentW = SizeOf(parent, true, 0);
    const float parentH = SizeOf(parent, false, 0);
    if (!(parentW > 0.0f) || !(parentH > 0.0f)) return;

    const float originX =
        (parent == nullptr) ? 0.0f : ScreenPosOf(parent, true, 0) - 0.5f * parentW;
    const float originY =
        (parent == nullptr) ? 0.0f : ScreenPosOf(parent, false, 0) - 0.5f * parentH;

    x = (targetX - originX) * 100.0f / parentW;
    y = (targetY - originY) * 100.0f / parentH;
    absolute = true;

    static uint64_t reports = 0;
    if ((reports++ % kGeometryReportInterval) == 0) {
        ReportGeometry(clean, applied, fov, aspect, parentW, parentH, aim, x, y);
        ReportSync();
    }
}

void* FindElementDetour(void* layer, const void* name, char createIfMissing) {
    void* const element = g_original(layer, name, createIfMissing);

    // Every HUD element in the game comes through this function. Only the one
    // call site that was handed the RETICLE layer is ours, and its return
    // address is what says so.
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_returnSite) return element;
    if (element == nullptr) return element;
    (void)layer;

    float* const position = PositionOf(element);
    if (position == nullptr) return element;

    const RememberedBase* const base = BaseFor(element, position);
    if (base == nullptr) {
        // Not the one being driven. Put back any correction left standing on it,
        // because it would add to the live one on the crosshair the player is
        // actually looking at. Safe to write: it was just handed over, so it is
        // live by construction.
        for (int i = 0; i < g_baseCount; ++i) {
            if (g_bases[i].element != element) continue;
            position[0] = g_bases[i].x;
            position[1] = g_bases[i].y;
            break;
        }
        return element;
    }

    float x = 0.0f, y = 0.0f;
    bool absolute = false;
    ComputeOffset(element, x, y, absolute);
    position[0] = absolute ? x : base->x + x;
    position[1] = absolute ? y : base->y + y;

    g_lastX.store(x, std::memory_order_relaxed);
    g_lastY.store(y, std::memory_order_relaxed);
    g_placements.fetch_add(1, std::memory_order_relaxed);
    return element;
}

const char* LayoutOf(void* element) {
    return *reinterpret_cast<const char* const*>(static_cast<char*>(element) + kElementData);
}

void* FindChild(void* element, const char* name) {
    const int count = *reinterpret_cast<const int*>(static_cast<char*>(element) + 0x5c);
    auto children = *reinterpret_cast<void***>(static_cast<char*>(element) + 0x60);
    for (int i = 0; i < count; ++i) {
        const char* data = LayoutOf(children[i]);
        if (data != nullptr && std::strncmp(data + 0x48, name, 16) == 0) return children[i];
    }
    return nullptr;
}

void RenderGuiDetour(void* gui) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_renderReturnSite) {
        g_renderOriginal(gui);
        return;
    }
    // Ship sights already project through the tracked view. Their steering
    // offsets must not be replaced with the walking camera's forward ray.
    if (IsInShip()) {
        g_lastX.store(0.0f, std::memory_order_relaxed);
        g_lastY.store(0.0f, std::memory_order_relaxed);
        g_renderOriginal(gui);
        return;
    }
    void* layer = FindChild(gui, "RETICLE");
    if (layer == nullptr || LayoutOf(layer)[0x61] != 0) {
        g_renderOriginal(gui);
        return;
    }
    struct SavedPosition { float* position; float x, y; char* hidden; char visibility; };
    struct DrawState {
        std::vector<SavedPosition> saved;
        ~DrawState() {
            for (const auto& entry : saved) {
                entry.position[0] = entry.x;
                entry.position[1] = entry.y;
                *entry.hidden = entry.visibility;
            }
        }
    } state;
    const int count = *reinterpret_cast<const int*>(static_cast<char*>(layer) + 0x5c);
    auto children = *reinterpret_cast<void***>(static_cast<char*>(layer) + 0x60);
    for (int i = 0; i < count; ++i) {
        void* group = children[i];
        const char* data = LayoutOf(group);
        if (data == nullptr || data[0x61] != 0) continue;
        float* position = PositionOf(group);
        if (position == nullptr) continue;
        void* anchor = nullptr;
        for (const char* name : {"DOT", "NORMAL", "NOSHOOT"}) {
            void* candidate = FindChild(group, name);
            if (candidate != nullptr && LayoutOf(candidate)[0x61] == 0) {
                anchor = candidate;
                break;
            }
        }
        if (anchor == nullptr) continue;
        float x, y;
        bool absolute, visible;
        ComputeOffset(group, x, y, absolute, &visible);
        if (absolute) {
            // The game already projects some sights. Correct only the remaining
            // distance, preserving the relative layout of rings and indicators.
            const float parentW = SizeOf(layer, true, 0);
            const float parentH = SizeOf(layer, false, 0);
            x -= position[0] + (ScreenPosOf(anchor, true, 0) -
                               ScreenPosOf(group, true, 0)) * 100.0f / parentW;
            y -= position[1] + (ScreenPosOf(anchor, false, 0) -
                               ScreenPosOf(group, false, 0)) * 100.0f / parentH;
        }
        char* hidden = const_cast<char*>(data) + 0x61;
        state.saved.push_back({position, position[0], position[1], hidden, *hidden});
        if (!visible) *hidden = 1;
        position[0] += x;
        position[1] += y;
        g_lastX.store(x, std::memory_order_relaxed);
        g_lastY.store(y, std::memory_order_relaxed);
        g_placements.fetch_add(1, std::memory_order_relaxed);
    }
    g_renderOriginal(gui);
}

}  // namespace

void InstallReticle(std::uint32_t nguiFindElementRva,
                    std::uint32_t reticleLookupReturnRva,
                    std::uint32_t gfxManagerPtrRva,
                    bool sweep,
                    std::uint32_t nguiRenderRva,
                    std::uint32_t reticleRenderReturnRva) {
    if (nguiRenderRva == 0 && (nguiFindElementRva == 0 || reticleLookupReturnRva == 0)) {
        HT_LOG("Reticle: this build pins no NGui element lookup, so the "
               "crosshair stays where the engine draws it. It marks the shot "
               "only while your head is centred.");
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_moduleBase = base;
    g_gfxManagerPtrRva = gfxManagerPtrRva;
    g_returnSite = base + reticleLookupReturnRva;
    g_sweep = sweep;
    if (nguiRenderRva != 0) {
        g_renderReturnSite = base + reticleRenderReturnRva;
        void* target = reinterpret_cast<void*>(base + nguiRenderRva);
        auto& hm = cameraunlock::hooks::HookManager::Instance();
        auto status = hm.CreateHook(target, reinterpret_cast<void*>(&RenderGuiDetour),
                                    reinterpret_cast<void**>(&g_renderOriginal));
        if (status == cameraunlock::hooks::HookStatus::Ok) status = hm.EnableHook(target);
        if (status != cameraunlock::hooks::HookStatus::Ok) {
            HT_LOG("ERROR: reticle render hook failed (%s)", cameraunlock::hooks::HookStatusToString(status));
            hm.RemoveHook(target);
            return;
        }
        HT_LOG("Reticle render hook installed at %08X, caller %08X.", nguiRenderRva, reticleRenderReturnRva);
        return;
    }
    g_target = reinterpret_cast<void*>(base + nguiFindElementRva);

    auto& hm = cameraunlock::hooks::HookManager::Instance();
    auto st = hm.CreateHook(g_target, reinterpret_cast<void*>(&FindElementDetour),
                            reinterpret_cast<void**>(&g_original));
    if (st == cameraunlock::hooks::HookStatus::Ok) st = hm.EnableHook(g_target);
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        HT_LOG("ERROR: reticle hook FAILED (%s) - the crosshair will stay at "
               "screen centre.", cameraunlock::hooks::HookStatusToString(st));
        hm.RemoveHook(g_target);
        g_target = nullptr;
        return;
    }

    HT_LOG("Reticle hook installed at RVA 0x%08X, claiming the lookup that "
           "returns to RVA 0x%08X - the crosshair is placed where the shot "
           "lands.%s", nguiFindElementRva, reticleLookupReturnRva,
           sweep ? " [Debug] ReticleSweep is ON, so it walks a fixed square "
                   "instead of following the aim." : "");
}

std::uint64_t ReticlePlacementCount() { return g_placements.load(); }

void LastReticleOffset(float& x, float& y) {
    x = g_lastX.load(std::memory_order_relaxed);
    y = g_lastY.load(std::memory_order_relaxed);
}

}  // namespace NMSHT
