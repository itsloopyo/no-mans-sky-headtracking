#include "pch.h"
#include "game_state.h"

#include "ads.h"
#include "debug_log.h"
#include "mod.h"
#include "tracking_verdict.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include <cameraunlock/memory/rtti_vtable.h>

namespace NMSHT {

namespace {

struct KnownState {
    const char* className;
    bool gameplay;
    uintptr_t vtable;
};

// Every application state the engine declares. Only the simulation state is
// gameplay; the rest are the frontend, the loaders, the galaxy map and the
// death screen, and head tracking has no business moving the camera in any of
// them. Listed in full rather than as "simulation vs not" so an unrecognised
// vtable is visibly unrecognised instead of silently counting as a menu.
KnownState g_states[] = {
    { "cGcApplicationSimulationState",     true,  0 },
    { "cGcApplicationTitleScreenState",    false, 0 },
    { "cGcApplicationGameModeSelectorState", false, 0 },
    { "cGcApplicationBootState",           false, 0 },
    { "cGcApplicationGlobalLoadState",     false, 0 },
    { "cGcApplicationLocalLoadState",      false, 0 },
    { "cGcApplicationBaseLoadingState",    false, 0 },
    { "cGcApplicationDeathState",          false, 0 },
    { "cGcApplicationGalacticMapState",    false, 0 },
    { "cGcApplicationAmbientGameState",    false, 0 },
    { "cGcApplicationScratchpadState",     false, 0 },
    { "cGcApplicationShutdownState",       false, 0 },
    { "cGcApplicationCoreServicesState",   false, 0 },
    { "cGcApplicationSmokeTestState",      false, 0 },
};
constexpr int kStateCount = static_cast<int>(sizeof(g_states) / sizeof(g_states[0]));

constexpr int kMaxCandidates = 32;

struct Candidate {
    uintptr_t address;
    int lastState;
    int distinctSeen;
    unsigned int seenMask;
};

Candidate g_candidates[kMaxCandidates]{};
int g_candidateCount = 0;
volatile long g_selected = -1;

uintptr_t g_moduleBase = 0;
std::atomic<bool> g_multiplayer{false};
std::atomic<int> g_currentState{-1};
std::atomic<int> g_menuPage{0};

// The gate and the reason for it, published together.
//
// They are read as a SET on the commit thread - "if not in gameplay, log why" -
// and written from two threads: the 100 ms poll loop and the ADS hotkey. Two
// separate stores let a frame pair one verdict's gate with another's reason and
// name the wrong cause in the log.
//
// Published by POINTER into a ring, not as an atomic struct. A
// std::atomic<Verdict> of these two fields is 16 bytes, and MSVC does not make a
// 16-byte atomic lock-free on x64 - it takes an internal lock, on the read, on
// every committed frame, on a thread that is inside a vectored exception
// handler. A pointer-sized atomic is lock-free, so the read is a plain load.
//
// Not a claim that this thread never locks: the suppression line it logs
// immediately afterwards goes through the file logger's mutex. The difference is
// that the logger is edge-triggered and this would be every frame.
//
// Every field is immortal - the reasons are string literals and the state names
// live in the static g_states table - so a reader may keep the pointer.
struct Verdict {
    bool poseApplies;
    const char* reason;
};

constexpr int kVerdictSlots = 8;
Verdict g_verdictSlots[kVerdictSlots] = { { false, "starting up" } };
std::atomic<const Verdict*> g_verdict{&g_verdictSlots[0]};
static_assert(std::atomic<const Verdict*>::is_always_lock_free,
              "the commit thread reads this from inside an exception handler, so "
              "it must never take a lock");

// Serialised because the poll thread and the hotkey thread both publish, and at
// 10 Hz plus a keypress there is nothing to contend for.
std::mutex g_verdictMutex;
int g_verdictNext = 1;

void PublishVerdictValue(bool poseApplies, const char* reason) {
    std::lock_guard<std::mutex> lock(g_verdictMutex);
    Verdict* const slot = &g_verdictSlots[g_verdictNext];
    g_verdictNext = (g_verdictNext + 1) % kVerdictSlots;
    slot->poseApplies = poseApplies;
    slot->reason = reason;
    g_verdict.store(slot, std::memory_order_release);
}

// Address of the engine's weapon-zoom byte, published once the global block has
// resolved and the byte has been validated as readable. The render thread reads
// the aim state every frame, so it dereferences this rather than re-walking the
// block pointer and calling VirtualQuery on the hot path. Zero until the block
// exists, which reads as "not aiming".
std::atomic<uintptr_t> g_aimFlag{0};

GameStateOffsets g_offsets{};

// The last sampled inputs the verdict walk needs, so RefreshTrackingVerdict can
// re-run it off the poll thread's cadence.
std::atomic<long> g_lastSelected{-1};
std::atomic<bool> g_lastMultiplayer{false};

// How often the poll thread re-reads the FSM slot and the two gates.
constexpr DWORD kPollIntervalMs = 100;

// Nothing below this is a valid user-mode pointer, so a value in that range is
// data that merely looks like one.
constexpr uintptr_t kMinUserAddress = 0x10000;

constexpr DWORD kReadableProtections = PAGE_READONLY | PAGE_READWRITE |
                                       PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                       PAGE_EXECUTE_READWRITE |
                                       PAGE_EXECUTE_WRITECOPY;

bool IsReadableRegion(const MEMORY_BASIC_INFORMATION& mbi) {
    return mbi.State == MEM_COMMIT && (mbi.Protect & kReadableProtections) != 0 &&
           (mbi.Protect & PAGE_GUARD) == 0;
}

// Committed readable regions, sampled once. A raw dereference of every qword in
// the module's writable data would fault on the first value that merely looks
// like a pointer, and an exception filter around a scan that reads tens of
// millions of words is both slow and a good way to swallow a real fault.
struct Region { uintptr_t begin, end; };
std::vector<Region> g_readable;

void CollectReadableRegions() {
    g_readable.clear();
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < limit && VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (IsReadableRegion(mbi)) {
            const uintptr_t b = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            if (!g_readable.empty() && g_readable.back().end == b) {
                g_readable.back().end = b + mbi.RegionSize;
            } else {
                g_readable.push_back({ b, b + mbi.RegionSize });
            }
        }
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
}

// Live check, for the handful of reads the poll thread makes. The cached list
// below is a snapshot taken at install time, and the game allocates the block
// these gates read from well after that, so a cached answer would reject a
// perfectly valid pointer and silently leave both gates dead.
bool ReadableLive(uintptr_t p, size_t size) {
    if (p < kMinUserAddress) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (!IsReadableRegion(mbi)) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return p + size <= end;
}

bool Readable(uintptr_t p, size_t size) {
    size_t lo = 0, hi = g_readable.size();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (g_readable[mid].end <= p) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_readable.size() && g_readable[lo].begin <= p &&
           p + size <= g_readable[lo].end;
}

int StateIndexOfObject(uintptr_t object, bool cached) {
    if (object < kMinUserAddress || (object & 7) != 0) return -1;
    if (!(cached ? Readable(object, sizeof(uintptr_t))
                 : ReadableLive(object, sizeof(uintptr_t)))) return -1;
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(object);
    for (int i = 0; i < kStateCount; ++i) {
        if (g_states[i].vtable != 0 && g_states[i].vtable == vt) return i;
    }
    return -1;
}

// Scans the running module's writable sections for slots holding a pointer to
// an application state object. The FSM's current-state pointer is one of them;
// so, usually, are its previous/next slots and any table the engine keeps.
void ScanForCandidates() {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_moduleBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_moduleBase + dos->e_lfanew);
    const auto* sec = IMAGE_FIRST_SECTION(nt);

    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if ((sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
        const uintptr_t begin = g_moduleBase + sec[i].VirtualAddress;
        const uintptr_t end = begin + sec[i].Misc.VirtualSize;
        for (uintptr_t p = (begin + 7) & ~uintptr_t(7); p + 8 <= end; p += 8) {
            if (!Readable(p, sizeof(uintptr_t))) continue;
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(p);
            const int idx = StateIndexOfObject(value, true);
            if (idx < 0) continue;
            if (g_candidateCount >= kMaxCandidates) return;
            Candidate& c = g_candidates[g_candidateCount++];
            c.address = p;
            c.lastState = idx;
            c.distinctSeen = 1;
            c.seenMask = 1u << idx;
            HT_LOG("Game state: FSM candidate #%d at RVA 0x%08llX holds %s.",
                   g_candidateCount - 1,
                   (unsigned long long)(p - g_moduleBase), g_states[idx].className);
        }
    }
}

// The engine's global block. Everything the two gates below read is an offset
// from it, and it is a pointer, so it is null until the game has built it.
uintptr_t GameGlobals() {
    if (g_offsets.gameGlobalsPtrRva == 0) return 0;
    const uintptr_t slot = g_moduleBase + g_offsets.gameGlobalsPtrRva;
    if (!ReadableLive(slot, sizeof(uintptr_t))) return 0;
    const uintptr_t base = *reinterpret_cast<const uintptr_t*>(slot);
    return ReadableLive(base, sizeof(uintptr_t)) ? base : 0;
}

// Half-open range of the network player slot array inside the block. False when
// the block is not up yet or the range is unreadable, in which case every
// counter built on it reads 0.
bool NetPlayerSlotRange(uintptr_t base, uintptr_t& begin, uintptr_t& end) {
    if (base == 0) return false;
    begin = base + g_offsets.netPlayerSlotsBegin;
    end = base + g_offsets.netPlayerSlotsEnd;
    return end > begin && ReadableLive(begin, end - begin);
}

// Counts the players in the session by reading the network player slot array:
// a slot holds a player pointer, and a connection byte above 1 marks that
// player present. The slot range, the byte and the threshold are measured
// offsets recorded in the build profile; the read below is this mod's own.
// The local player is not counted - in solo play every slot reads empty.
int ConnectedPlayers(uintptr_t base) {
    uintptr_t begin = 0, end = 0;
    if (!NetPlayerSlotRange(base, begin, end)) return 0;
    int count = 0;
    for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(p);
        if (player == 0) continue;
        const uintptr_t flag = player + g_offsets.netPlayerConnectedByte;
        if (!ReadableLive(flag, 1)) continue;
        if (*reinterpret_cast<const unsigned char*>(flag) > 1) ++count;
    }
    return count;
}

// cGcGenericSectionConditionInventoryOpen reads this int and calls a menu open
// for any value other than 0 and 2, so those two are the "nothing is up" values.
int MenuPageMode(uintptr_t base) {
    if (base == 0 || g_offsets.menuPageModeOffset == 0) return 0;
    const uintptr_t p = base + g_offsets.menuPageModeOffset;
    if (!ReadableLive(p, sizeof(int))) return 0;
    return *reinterpret_cast<const int*>(p);
}

bool MenuIsUp(int page) { return page != 0 && page != 2; }

// Resolves the weapon-zoom byte's address once the block is up. Called from the
// poll thread, never from the render thread.
void PublishAimFlag(uintptr_t base) {
    if (g_offsets.weaponZoomOffset == 0) return;

    // Re-checked every poll rather than validated once and trusted for the life
    // of the process: the render thread dereferences this on every committed
    // frame, and a block the engine has torn down would fault inside the camera
    // commit. This NARROWS that window to one poll interval, it does not close
    // it - the check and the render thread's read are 100 ms apart at worst.
    const uintptr_t p = base == 0 ? 0 : base + g_offsets.weaponZoomOffset;
    const uintptr_t live = (p != 0 && ReadableLive(p, 1)) ? p : 0;
    const uintptr_t was = g_aimFlag.exchange(live, std::memory_order_relaxed);
    if (was == live) return;
    // Edge-latched. GameGlobals() reads 0 across every load and teardown, so an
    // unlatched log here writes a line at up to 10 Hz while the block comes and
    // goes.
    static bool s_saidLost = false;
    if (live != 0) {
        s_saidLost = false;
        HT_LOG("ADS: weapon-zoom state at 0x%p (block + 0x%08X).",
               reinterpret_cast<void*>(live), g_offsets.weaponZoomOffset);
    } else if (!s_saidLost) {
        s_saidLost = true;
        HT_LOG("ADS: the weapon-zoom state at 0x%p is no longer readable; "
               "reporting not-aiming until it comes back.",
               reinterpret_cast<void*>(was));
    }
}

// The verdict, from one sampled view of the game. Split out so the ADS hotkey
// can re-run it immediately instead of riding the 100 ms poll.
void PublishVerdict(int selectedCandidate, int stateIndex, int page, bool multiplayer) {
    TrackingInputs in;
    in.fsmLocated = selectedCandidate >= 0;
    in.stateRecognised = stateIndex >= 0;
    in.gameplayState = stateIndex >= 0 && g_states[stateIndex].gameplay;
    in.stateName = stateIndex >= 0 ? g_states[stateIndex].className : nullptr;
    in.menuUp = MenuIsUp(page);
    in.multiplayer = multiplayer;
    in.aiming = IsAimingDownSights();
    in.adsMode = Ads::Instance().Mode();

    const TrackingVerdict v = EvaluateTracking(in);
    PublishVerdictValue(v.poseApplies, v.reason);
}

int ReadCandidate(const Candidate& c) {
    if (!ReadableLive(c.address, sizeof(uintptr_t))) return -1;
    return StateIndexOfObject(*reinterpret_cast<const uintptr_t*>(c.address), false);
}

// Once, as soon as the block exists. Proves the two gates are reading live
// memory rather than quietly answering 0 because the block pointer is null.
bool AnnounceGlobalBlock(uintptr_t base) {
    if (base == 0) return false;

    uintptr_t begin = 0, end = 0;
    int occupied = 0;
    int slots = 0;
    if (NetPlayerSlotRange(base, begin, end)) {
        slots = static_cast<int>((end - begin) / sizeof(uintptr_t));
        for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
            if (*reinterpret_cast<const uintptr_t*>(p) != 0) ++occupied;
        }
    }
    HT_LOG("Game state: global block at 0x%p, %d of %d network player slots "
           "occupied, %d connected, menu page %d.",
           reinterpret_cast<void*>(base), occupied, slots, ConnectedPlayers(base),
           MenuPageMode(base));
    return true;
}

void UpdateCandidates() {
    for (int i = 0; i < g_candidateCount; ++i) {
        Candidate& c = g_candidates[i];
        const int idx = ReadCandidate(c);
        if (idx == c.lastState) continue;
        c.lastState = idx;
        if (idx >= 0 && (c.seenMask & (1u << idx)) == 0) {
            c.seenMask |= 1u << idx;
            ++c.distinctSeen;
        }
        HT_LOG("Game state: candidate #%d (RVA 0x%08llX) -> %s", i,
               (unsigned long long)(c.address - g_moduleBase),
               idx < 0 ? "unrecognised" : g_states[idx].className);
    }
}

// The FSM's own current-state slot is the one that MOVES. A table of the
// engine's state objects holds one class forever, so a candidate that has only
// ever been seen holding a single state is not it. Returns the already-selected
// index unchanged once one has been settled on.
long SelectLiveCandidate(long selected) {
    if (selected >= 0) return selected;
    for (int i = 0; i < g_candidateCount; ++i) {
        if (g_candidates[i].distinctSeen < 2) continue;
        InterlockedExchange(&g_selected, i);
        HT_LOG("Game state: tracking candidate #%d (RVA 0x%08llX) - it has held "
               "%d different states, so it is the FSM's live slot. Pin it as "
               "appStateSlotRva.", i,
               (unsigned long long)(g_candidates[i].address - g_moduleBase),
               g_candidates[i].distinctSeen);
        return i;
    }
    return -1;
}

void PollThread() {
    bool announcedGlobals = false;
    // A rescan re-snapshots every committed region in the address space and
    // then walks every writable section of the image, so it is far too heavy to
    // leave running at a fixed rate for a whole session on a build where it
    // never succeeds. It is frequent while the answer is still coming - the
    // engine builds its first application state object within a few seconds of
    // launch - and rare afterwards.
    constexpr int kRescanFastTicks = 20;    // 2s at a 100ms poll
    constexpr int kRescanSlowTicks = 300;   // 30s
    constexpr int kRescanFastAttempts = 30; // ~1 minute of trying hard
    int rescanTicks = kRescanFastTicks;
    int rescanAttempts = 0;
    for (;;) {
        // Resolved once a poll. Every gate below reads an offset from it, and
        // each resolution costs two VirtualQuery calls.
        const uintptr_t globals = GameGlobals();
        if (!announcedGlobals) announcedGlobals = AnnounceGlobalBlock(globals);

        // The scan at install time runs while the game is still booting, before
        // the engine has constructed any application state object, so on a build
        // with no pinned slot it finds nothing and the gameplay gate never
        // opens - "the application state machine has not been located", for the
        // whole session. Keep looking until it does. Rate-limited rather than
        // every poll: it walks every writable section of the image.
        const int rescanEvery =
            rescanAttempts < kRescanFastAttempts ? kRescanFastTicks : kRescanSlowTicks;
        if (g_candidateCount == 0 && ++rescanTicks >= rescanEvery) {
            rescanTicks = 0;
            ++rescanAttempts;
            // The region snapshot has to be retaken first. It was sampled at
            // install time, and the state object lives in memory the engine
            // commits later, so the scan's own readability filter would reject
            // the very pointer it is looking for.
            CollectReadableRegions();
            ScanForCandidates();
            if (g_candidateCount != 0) {
                HT_LOG("Game state: %d FSM candidate(s) found on scan %d; the "
                       "engine had not built a state object yet when the mod "
                       "first looked.", g_candidateCount, rescanAttempts);
            } else if (rescanAttempts == kRescanFastAttempts) {
                HT_LOG("Game state: no FSM slot after %d scans, so they drop to "
                       "one every %d seconds. Head tracking stays suppressed "
                       "until one is found - nothing else about the mod is "
                       "affected.", rescanAttempts, kRescanSlowTicks / 10);
            }
        }

        UpdateCandidates();
        const long sel =
            SelectLiveCandidate(InterlockedCompareExchange(&g_selected, -1, -1));

        const int page = MenuPageMode(globals);
        const int players = ConnectedPlayers(globals);
        const bool multiplayer = players > 0;
        // Diagnostics only. The page mode walks 0 -> 3 -> 4 -> 5 -> 0 for a
        // single PDA open and close, so four lines per menu visit, and what a
        // player needs from it - whether tracking stood down - is already said
        // once by the suppression line in the camera hook.
        if (page != g_menuPage.exchange(page, std::memory_order_relaxed) &&
            Mod::Instance().GetConfig().diagnostics) {
            HT_LOG("Game state: menu page mode -> %d (%s)", page,
                   MenuIsUp(page) ? "menu up" : "no menu");
        }
        if (multiplayer != g_multiplayer.exchange(multiplayer, std::memory_order_relaxed)) {
            HT_LOG("Game state: %d other player(s) in the session - head tracking "
                   "is %s.", players, multiplayer ? "suppressed" : "available again");
        }

        const int idx = sel >= 0 ? g_candidates[sel].lastState : -1;
        g_currentState.store(idx, std::memory_order_relaxed);

        PublishAimFlag(globals);
        g_lastSelected.store(sel, std::memory_order_relaxed);
        g_lastMultiplayer.store(multiplayer, std::memory_order_relaxed);
        PublishVerdict(static_cast<int>(sel), idx, page, multiplayer);

        Sleep(kPollIntervalMs);
    }
}

}  // namespace

void InstallGameStateProbe(const GameStateOffsets& offsets) {
    g_offsets = offsets;
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* const module = reinterpret_cast<void*>(g_moduleBase);

    int resolved = 0;
    for (int i = 0; i < kStateCount; ++i) {
        cameraunlock::memory::VtableInfo info{};
        if (cameraunlock::memory::FindVtableFromRTTI(module, g_states[i].className,
                                                     info, 1)) {
            g_states[i].vtable = info.vtable_address;
            ++resolved;
        } else {
            HT_LOG("Game state: RTTI lookup failed for %s.", g_states[i].className);
        }
    }
    if (g_states[0].vtable == 0) {
        HT_LOG("ERROR: cGcApplicationSimulationState has no RTTI vtable - the "
               "gameplay gate cannot be built and head tracking would run in "
               "menus. Head tracking stays off.");
        return;
    }
    HT_LOG("Game state: resolved %d/%d application state vtables.", resolved, kStateCount);

    CollectReadableRegions();

    if (g_offsets.gameGlobalsPtrRva == 0) {
        HT_LOG("Game state: no global block pinned for this build - the menu and "
               "multiplayer gates are inactive.");
    }
    if (g_offsets.weaponZoomOffset == 0) {
        HT_LOG("ADS: no weapon-zoom offset pinned for this build, so the aim state "
               "reports 'not aiming' on every frame and head tracking behaves at "
               "the sights exactly as it does at the hip. The ADS mode cycle is "
               "live and saved; it has nothing to act on yet.");
    }

    if (g_offsets.appStateSlotRva != 0) {
        const uintptr_t pinned = g_moduleBase + g_offsets.appStateSlotRva;
        const int idx = ReadableLive(pinned, sizeof(uintptr_t))
                            ? StateIndexOfObject(*reinterpret_cast<const uintptr_t*>(pinned), false)
                            : -1;
        g_candidates[0].address = pinned;
        g_candidates[0].lastState = idx;
        g_candidates[0].distinctSeen = 1;
        g_candidates[0].seenMask = idx >= 0 ? (1u << idx) : 0u;
        g_candidateCount = 1;
        InterlockedExchange(&g_selected, 0);
        HT_LOG("Game state: using pinned FSM slot at RVA 0x%08X (currently %s).",
               g_offsets.appStateSlotRva,
               idx < 0 ? "unrecognised" : g_states[idx].className);
    } else {
        ScanForCandidates();
        HT_LOG("Game state: %d FSM candidates found by scan; the live one is "
               "picked once it changes state.", g_candidateCount);
    }

    std::thread(PollThread).detach();
}

bool IsInGameplay() {
    return g_verdict.load(std::memory_order_acquire)->poseApplies;
}

bool TrackingApplies(const char*& reason) {
    const Verdict* const v = g_verdict.load(std::memory_order_acquire);
    reason = v->reason;
    return v->poseApplies;
}

bool IsInShip() {
    if (g_offsets.playerFromGlobals == 0) return false;
    const uintptr_t globals = GameGlobals();
    if (globals == 0) return false;
    const uintptr_t player = globals + g_offsets.playerFromGlobals;
    if (!ReadableLive(player, 0x170)) return false;
    using GetShip = void* (*)(uintptr_t);
    const auto getShip = reinterpret_cast<GetShip>(g_moduleBase + g_offsets.playerShipRva);
    return getShip(player) != nullptr;
}

bool GetWalkingUp(float up[3]) {
    if (g_offsets.playerFromGlobals == 0 || IsInShip()) return false;
    const uintptr_t globals = GameGlobals();
    if (globals == 0) return false;
    const uintptr_t player = globals + g_offsets.playerFromGlobals;
    if (!ReadableLive(player, 0x32C)) return false;

    const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(player + 0x148);
    if (!ReadableLive(wrapper, 0x18)) return false;
    const uintptr_t character = *reinterpret_cast<const uintptr_t*>(wrapper + 0x10);
    if (!ReadableLive(character + 0x214, sizeof(int))) return false;
    const int state = *reinterpret_cast<const int*>(character + 0x214);
    // Death, full-body override and both EVA states have no walking horizon.
    // LowGWalk (18) and LowGRun (19) still use the body's gravity-aligned up.
    if (state >= 14 && state <= 17) return false;

    const auto* bodyUp = reinterpret_cast<const float*>(player + 0x320);
    const float lengthSquared = bodyUp[0] * bodyUp[0] + bodyUp[1] * bodyUp[1] +
                                bodyUp[2] * bodyUp[2];
    if (!std::isfinite(lengthSquared) || lengthSquared < 0.5f || lengthSquared > 1.5f) {
        static std::atomic<ULONGLONG> lastDiagnostic{0};
        const ULONGLONG now = GetTickCount64();
        ULONGLONG previous = lastDiagnostic.load(std::memory_order_relaxed);
        if (now - previous >= 1000 && lastDiagnostic.compare_exchange_strong(previous, now)) {
            HT_LOG("Yaw: invalid player up vector (length squared %.6f).", lengthSquared);
        }
        return false;
    }
    const float inverseLength = 1.0f / std::sqrt(lengthSquared);
    for (int i = 0; i < 3; ++i) up[i] = bodyUp[i] * inverseLength;
    return true;
}

bool IsAimingDownSights() {
    const uintptr_t p = g_aimFlag.load(std::memory_order_relaxed);
    if (p == 0) return false;
    return *reinterpret_cast<const volatile unsigned char*>(p) != 0;
}

void RefreshTrackingVerdict() {
    const long sel = g_lastSelected.load(std::memory_order_relaxed);
    const int idx = g_currentState.load(std::memory_order_relaxed);
    PublishVerdict(static_cast<int>(sel), idx, g_menuPage.load(std::memory_order_relaxed),
                   g_lastMultiplayer.load(std::memory_order_relaxed));
}

const char* CurrentStateName() {
    const int idx = g_currentState.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= kStateCount) return "unresolved";
    return g_states[idx].className;
}

}  // namespace NMSHT
