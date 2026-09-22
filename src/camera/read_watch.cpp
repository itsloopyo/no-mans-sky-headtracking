#include "pch.h"
#include "read_watch.h"

#include "hw_breakpoint.h"
#include "core/debug_log.h"

namespace NMSHT {

namespace {

// Distinct instruction addresses seen touching the watched bytes. A handful is
// expected; the cap only stops a runaway from walking off the array.
constexpr int kMaxSites = 64;

// Twenty minutes of waiting for the player to reach the world, then a settle so
// the burst catches steady-state rendering rather than the tail of the load.
constexpr int kGameplayPollMs = 500;
constexpr int kGameplayPollAttempts = 2400;
constexpr int kSceneSettleMs = 15000;

// Short bursts on purpose: a read watch on memory the engine touches thousands
// of times a second makes the game unplayable while it is armed.
constexpr int kBursts = 4;
constexpr int kBurstMs = 600;
constexpr int kBetweenBurstsMs = 2000;

struct Site {
    uintptr_t rva;
    volatile LONG64 hits;
    volatile LONG tids[4];
};

Site g_sites[kMaxSites]{};
volatile long g_siteCount = 0;
volatile long g_totalHits = 0;

// A slot of its own. Every other probe re-arms across every thread on a timer,
// so sharing one means whichever armed last wins and the loser silently reports
// the winner's hits - a collision that once produced a "writer" result naming
// the commit breakpoint itself.
constexpr int kSlot = kSlotDataWatch;

const float* volatile g_transform = nullptr;
volatile long g_inWorld = 0;
// This mod's own image, so the handler can skip our writes, and the game's, so
// it can report an RVA. Both are resolved on the watch thread before the
// handler is installed - GetModuleHandleW inside the handler would run once per
// hit, and the watch fires thousands of times a second while it is armed.
uintptr_t g_selfBase = 0;
uintptr_t g_selfEnd = 0;
uintptr_t g_gameBase = 0;
uintptr_t g_gameEnd = 0;
volatile long g_foreignHits = 0;
PVOID g_handler = nullptr;

// g_siteCount can be pushed past kMaxSites by a racing claim, so every reader
// of it clamps. The count itself is left alone: it is also the index claim.
long SiteCount() {
    const long n = InterlockedCompareExchange(&g_siteCount, 0, 0);
    if (n < 0) return kMaxSites;      // wrapped; every slot has been claimed
    return n < kMaxSites ? n : kMaxSites;
}

// The exception fires AFTER the access, so RIP is the instruction following the
// one that touched the memory. Close enough to find the function it sits in.
LONG CALLBACK OnDebugException(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    // This watch's slot only. The commit hook and the weapon decoupler own slots
    // of their own, and answering for one of theirs here swallows the camera
    // injection.
    if ((ctx->Dr6 & Dr6BitFor(kSlot)) == 0) return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 &= ~Dr6BitFor(kSlot);

    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    // This mod writes the camera transform every commit (OnRenderPhaseBegin);
    // recording our own writes would bury the engine's accesses under them.
    if (rip >= g_selfBase && rip < g_selfEnd) return EXCEPTION_CONTINUE_EXECUTION;

    // An access from another module reports an RVA that is not NMS.exe's, and a
    // huge bogus number in the readers list is worse than a missing line. Counted
    // rather than silently dropped: this probe exists to name the consumers of
    // the camera, so "how many came from somewhere else" is evidence too.
    if (rip < g_gameBase || rip >= g_gameEnd) {
        InterlockedIncrement(&g_foreignHits);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    InterlockedIncrement(&g_totalHits);
    const uintptr_t rva = rip - g_gameBase;
    const long count = SiteCount();
    const LONG tid = static_cast<LONG>(GetCurrentThreadId());
    for (long i = 0; i < count; ++i) {
        if (g_sites[i].rva == rva) {
            // The whole point of this probe is the ranking, and a plain ++ from
            // the ~35,000 exceptions a second this fires at under-counts by an
            // unknown amount.
            InterlockedIncrement64(&g_sites[i].hits);
            for (auto& t : g_sites[i].tids) {
                const LONG was = InterlockedCompareExchange(&t, tid, 0);
                if (was == 0 || was == tid) break;
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    // Claim the index with the increment. Reading the count and then writing at
    // it lets two threads take the same slot and push the count past the array,
    // after which both the search above and the report loop index off the end.
    const long idx = InterlockedIncrement(&g_siteCount) - 1;
    if (idx >= 0 && idx < kMaxSites) {
        g_sites[idx].rva = rva;
        g_sites[idx].hits = 1;
        g_sites[idx].tids[0] = tid;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool g_writeOnly = false;
// [Debug] ReadWatchOffset: bytes past the accessor's pointer to watch, so the
// render rows (+0x50 on the September layout) can be watched as well as the
// accessor block.
uintptr_t g_offsetBytes = 0;
uintptr_t g_fixedAddress = 0;
const char* g_label = "camera transform";
const char* WatchName() { return g_writeOnly ? "write watch" : "read watch"; }

// x86 has no read-only watch, so the read watch traps reads AND writes and our
// own writes are filtered by address in the handler above.
void SetWatchOnAllThreads(uintptr_t address, bool arm) {
    const int touched = SetBreakpointOnAllThreads(
        kSlot, address, g_writeOnly ? BreakKind::Write4 : BreakKind::ReadWrite4, arm);
    HT_LOG("Diag: %s %s on %d threads.", WatchName(), arm ? "ARMED" : "cleared", touched);
}

void WatchThread() {
    HMODULE game = GetModuleHandleW(nullptr);
    MODULEINFO gmi{};
    if (!GetModuleInformation(GetCurrentProcess(), game, &gmi, sizeof(gmi))) {
        HT_LOG("Diag: %s aborted - could not measure NMS.exe (%lu).",
               WatchName(), GetLastError());
        return;
    }
    g_gameBase = reinterpret_cast<uintptr_t>(gmi.lpBaseOfDll);
    g_gameEnd = g_gameBase + gmi.SizeOfImage;

    HMODULE self = nullptr;
    MODULEINFO mi{};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&OnDebugException), &self) ||
        !GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
        // Without our own bounds the "skip our own writes" filter is inert, and
        // the result is a readers list that is mostly this mod's own detour.
        // That looks like an answer, which is worse than no answer.
        HT_LOG("Diag: %s aborted - could not locate this mod's own module (%lu), "
               "so our writes could not be filtered out of the result.",
               WatchName(), GetLastError());
        return;
    }
    g_selfBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    g_selfEnd = g_selfBase + mi.SizeOfImage;

    if (g_fixedAddress == 0) {
        for (int i = 0; i < kGameplayPollAttempts &&
                        (g_transform == nullptr || g_inWorld == 0); ++i) {
            Sleep(kGameplayPollMs);
        }
        if (g_transform == nullptr || g_inWorld == 0) {
            HT_LOG("Diag: %s aborted - gameplay never started.", WatchName());
            return;
        }
        Sleep(kSceneSettleMs);
    }

    g_handler = AddVectoredExceptionHandler(1, OnDebugException);
    if (g_handler == nullptr) {
        HT_LOG("Diag: %s aborted - could not install the exception handler.", WatchName());
        return;
    }

    for (int burst = 1; burst <= kBursts; ++burst) {
        // Re-read every burst: the active transform can switch under us.
        const uintptr_t address = g_fixedAddress != 0
                                      ? g_fixedAddress
                                      : reinterpret_cast<uintptr_t>(g_transform) + g_offsetBytes;
        HT_LOG("Diag: burst %d watching 0x%016llX", burst, (unsigned long long)address);
        const long before = g_totalHits;
        SetWatchOnAllThreads(address, true);
        Sleep(kBurstMs);
        SetWatchOnAllThreads(address, false);
        HT_LOG("Diag: burst %d - %ld hits, %ld sites total.",
               burst, g_totalHits - before, SiteCount());
        Sleep(kBetweenBurstsMs);
    }

    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;

    const long sites = SiteCount();
    HT_LOG("Diag: %s %s (%ld hits, %ld sites, %ld from another module):", g_label,
           g_writeOnly ? "WRITERS" : "readers", g_totalHits, sites, g_foreignHits);
    for (long i = 0; i < sites; ++i) {
        HT_LOG("  RVA 0x%08llX -> %llu  tids %ld %ld %ld %ld",
               (unsigned long long)g_sites[i].rva,
               (unsigned long long)g_sites[i].hits, g_sites[i].tids[0],
               g_sites[i].tids[1], g_sites[i].tids[2], g_sites[i].tids[3]);
    }
}

// One watch at a time. All three entry points drive the same WatchThread over
// the same globals - g_writeOnly, g_handler, g_sites, one debug register - so a
// second one does not watch a second thing: it flips the first watch's mode
// mid-run, overwrites g_handler so one vectored registration is leaked and the
// other is removed out from under a live burst, and fights the first for the
// slot. [Debug] ReadWatch and WriteWatch can both be set in one file, which is
// the whole of how that happens.
std::atomic<bool> g_started{false};

bool ClaimWatch(const char* what) {
    if (!g_started.exchange(true)) return true;
    HT_LOG("Diag: %s not started - another memory watch is already running, and "
           "they share one debug register. Turn the other one off to run this.",
           what);
    return false;
}

} // namespace

void NoteInWorld() { g_inWorld = 1; }

bool SawInWorld() { return g_inWorld != 0; }

void NoteTransformAddress(const float* transform) {
    // Always the latest, never the first. The manager holds TWO transforms and
    // a selector byte picks between them, so the one handed out at the frontend
    // is not necessarily the one in use in the world - and watching the idle
    // one finds almost no readers. Skipped when unchanged, which is nearly
    // every call: the source accessor runs on ~100 threads and an
    // unconditional store keeps this line bouncing between their caches.
    if (g_transform != transform) g_transform = transform;
}

void StartReadWatch(uintptr_t offsetBytes) {
    if (!ClaimWatch("read watch")) return;
    g_offsetBytes = offsetBytes;
    std::thread(WatchThread).detach();
}

void StartWriteWatch() {
    if (!ClaimWatch("write watch")) return;
    g_writeOnly = true;
    std::thread(WatchThread).detach();
}

void StartWriteWatchAt(uintptr_t address, const char* label) {
    if (!ClaimWatch(label)) return;
    g_writeOnly = true;
    g_fixedAddress = address;
    g_label = label;
    std::thread(WatchThread).detach();
}

} // namespace NMSHT
