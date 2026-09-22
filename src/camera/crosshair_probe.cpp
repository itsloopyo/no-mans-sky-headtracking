#include "pch.h"
#include "crosshair_probe.h"

#include "hw_breakpoint.h"
#include "core/debug_log.h"
#include "core/game_state.h"

#include <thread>
#include <vector>

namespace NMSHT {

namespace {

// Screen centres the crosshair could be stored as, paired so a match is two
// adjacent floats rather than one. A single float equal to 960 is everywhere; a
// 960 immediately followed by a 540 is not.
struct CentrePattern {
    const char* name;
    float x;
    float y;
};

constexpr CentrePattern kPatterns[] = {
    { "1920x1080 pixels", 960.0f, 540.0f },
    { "normalised",         0.5f,   0.5f },
    { "1280x720 pixels",  640.0f, 360.0f },
    { "1600x900 pixels",  800.0f, 450.0f },
};
constexpr int kPatternCount = static_cast<int>(sizeof(kPatterns) / sizeof(kPatterns[0]));

struct Candidate {
    float* address;
    int pattern;
    // Carried from the read-back rather than re-read through `address` when the
    // survivors are logged: tens of milliseconds pass between the two, and a
    // raw load into a block the game has freed by then faults on the probe
    // thread with no handler.
    float value[2];
};

constexpr size_t kMaxCandidates = 20000;
constexpr float kSentinel = 424242.0f;

// Long enough for the game to draw a frame and put its own value back over the
// sentinel, short enough that the odd numbers are never seen.
constexpr DWORD kSentinelWindowMs = 30;

// Candidates whose live value is logged individually; the rest are counted.
constexpr size_t kMaxLoggedLive = 64;

std::vector<Candidate> g_candidates;

// Read through ReadProcessMemory rather than with plain loads. VirtualQuery
// says the region was committed when it was asked; the game is free to free or
// decommit it before this loop reaches a given page, and a multi-GB walk of a
// live heap will eventually find one. A raw load there takes the game down with
// an access violation that looks like an engine crash.
bool ScanRegion(const MEMORY_BASIC_INFORMATION& mbi, int counts[kPatternCount]) {
    auto* const begin = static_cast<unsigned char*>(mbi.BaseAddress);
    constexpr size_t kChunk = 64 * 1024;
    std::vector<unsigned char> buf(kChunk);

    // Step by kChunk - 4, not kChunk - 8. A candidate needs bytes [a, a+8), so a
    // chunk covers first positions up to off + want - 8; the next chunk must
    // therefore start at off + want - 4. Stepping by kChunk - 8 re-scans that
    // last position, double-counting one match per 64 KB and burning a slot of
    // the candidate cap on a duplicate.
    for (size_t off = 0; off + 8 <= mbi.RegionSize; off += kChunk - 4) {
        const size_t want = (mbi.RegionSize - off) < kChunk ? (mbi.RegionSize - off) : kChunk;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), begin + off, buf.data(), want, &got) ||
            got < 8) {
            continue;
        }
        for (size_t q = 0; q + 8 <= got; q += 4) {
            const auto* const f = reinterpret_cast<const float*>(buf.data() + q);
            for (int i = 0; i < kPatternCount; ++i) {
                if (f[0] != kPatterns[i].x || f[1] != kPatterns[i].y) continue;
                ++counts[i];
                if (g_candidates.size() < kMaxCandidates) {
                    g_candidates.push_back(
                        { reinterpret_cast<float*>(begin + off + q), i, { f[0], f[1] } });
                }
                break;
            }
        }
        if (g_candidates.size() >= kMaxCandidates) return false;
    }
    return true;
}

void Scan() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t limit = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    int counts[kPatternCount]{};
    size_t bytes = 0;

    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < limit &&
           VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        // MEM_MAPPED is excluded on purpose: NMS maps its archives, and walking
        // them once cost 9 GB of disk reads and starved the game's own loading.
        // Executable protections are excluded on purpose, even the writable
        // ones. This probe STAMPS a sentinel over every candidate, and the byte
        // pair it matches on is unremarkable inside an x64 instruction stream,
        // so admitting PAGE_EXECUTE_READWRITE would overwrite four instruction
        // bytes in the game's own code - or in a trampoline - for a frame. A
        // spring-animated screen position is data; there is nothing to lose.
        const bool writable = (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
        if (mbi.State != MEM_COMMIT || !writable || (mbi.Protect & PAGE_GUARD) != 0) continue;
        if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) continue;
        bytes += mbi.RegionSize;
        if (!ScanRegion(mbi, counts)) break;
    }

    HT_LOG("CrosshairProbe: scanned %llu MB.", (unsigned long long)(bytes / (1024 * 1024)));
    for (int i = 0; i < kPatternCount; ++i) {
        HT_LOG("CrosshairProbe:   %-18s %d matches", kPatterns[i].name, counts[i]);
    }
    HT_LOG("CrosshairProbe: %llu candidates kept.",
           (unsigned long long)g_candidates.size());
}

// A crosshair whose position springs is written every frame. A constant in a
// data table is not. Writing a sentinel and seeing whether the game puts its own
// value back is what tells the two apart, and it is the filter that turns
// thousands of matches into a list worth looking at.
void FilterToLiveValues() {
    // One 30ms window with every candidate stamped at once, rather than 20,000
    // windows one at a time. Same evidence, a two-minute scan turned into a
    // single frame, and the game spends one frame with odd numbers in it instead
    // of two minutes.
    // Each candidate is re-read and re-checked against the pattern that matched
    // it, immediately before the stamp. Minutes and several GB pass between the
    // scan and here, and a block that has since been freed and handed to another
    // owner would otherwise get a sentinel written into it and, 30 ms later, a
    // stale float belonging to a dead allocation written back over that.
    std::vector<Candidate> stamped;
    std::vector<float> original;
    stamped.reserve(g_candidates.size());
    original.reserve(g_candidates.size());
    for (const Candidate& c : g_candidates) {
        float now[2];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), c.address, now, sizeof(now), &got) ||
            got != sizeof(now)) {
            continue;
        }
        if (now[0] != kPatterns[c.pattern].x || now[1] != kPatterns[c.pattern].y) continue;
        SIZE_T put = 0;
        if (!WriteProcessMemory(GetCurrentProcess(), c.address, &kSentinel,
                                sizeof(kSentinel), &put) || put != sizeof(kSentinel)) {
            continue;
        }
        stamped.push_back(c);
        original.push_back(now[0]);
    }
    Sleep(kSentinelWindowMs);
    std::vector<Candidate> live;
    for (size_t i = 0; i < stamped.size(); ++i) {
        // Read back and restore through the process APIs for the same reason the
        // scan uses them: the block can be freed during the 30 ms window, and a
        // raw store into it would fault exactly where the raw loads used to.
        float now[2] = { 0.0f, 0.0f };
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), stamped[i].address, now,
                               sizeof(now), &got) || got != sizeof(now)) {
            continue;
        }
        if (now[0] == kSentinel) {
            SIZE_T put = 0;
            WriteProcessMemory(GetCurrentProcess(), stamped[i].address,
                               &original[i], sizeof(float), &put);
        } else {
            Candidate c = stamped[i];
            c.value[0] = now[0];
            c.value[1] = now[1];
            live.push_back(c);
        }
    }
    g_candidates.swap(stamped);
    HT_LOG("CrosshairProbe: %llu of %llu candidates are rewritten every frame.",
           (unsigned long long)live.size(), (unsigned long long)g_candidates.size());
    for (size_t i = 0; i < live.size() && i < kMaxLoggedLive; ++i) {
        HT_LOG("CrosshairProbe:   live #%llu at 0x%p (%s) = (%.4f %.4f)",
               (unsigned long long)i, static_cast<void*>(live[i].address),
               kPatterns[live[i].pattern].name, live[i].value[0], live[i].value[1]);
    }
    g_candidates.swap(live);
}

// A slot of its own: slot 0 belongs to the camera commit and slot 1 to the
// weapon decoupler, and whichever slot armed last otherwise wins while the loser
// silently reports the winner's hits.
constexpr int kSlot = kSlotCrosshairProbe;

// How long each candidate is watched before moving on to the next.
constexpr DWORD kWatchWindowMs = 250;

constexpr int kMaxWriters = 4;
uintptr_t g_writerRips[kMaxWriters]{};
volatile long g_writerCount = 0;
uintptr_t g_moduleBase = 0;
PVOID g_handler = nullptr;

LONG CALLBACK OnCandidateWrite(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    if ((ctx->Dr6 & Dr6BitFor(kSlot)) == 0) return EXCEPTION_CONTINUE_SEARCH;
    ctx->Dr6 &= ~Dr6BitFor(kSlot);

    const uintptr_t rip = static_cast<uintptr_t>(ctx->Rip);
    const long n = InterlockedCompareExchange(&g_writerCount, 0, 0);
    for (long i = 0; i < n && i < kMaxWriters; ++i) {
        if (g_writerRips[i] == rip) return EXCEPTION_CONTINUE_EXECUTION;
    }
    // Claim the index with the increment, not before it. Reading the count and
    // then writing at it lets two threads take the same slot and push the count
    // past the array, after which the report loop reads off the end and logs a
    // garbage RVA as a crosshair writer.
    const long idx = InterlockedIncrement(&g_writerCount) - 1;
    if (idx >= 0 && idx < kMaxWriters) {
        g_writerRips[idx] = rip;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Names the instruction that writes each surviving candidate. Grouping the
// shortlist by its writer is what turns a list of heap addresses into something
// pinnable: an address moves every launch, the instruction does not, and the
// crosshair's writer is the one whose RVA sits in the HUD code.
void ReportWriters() {
    g_handler = AddVectoredExceptionHandler(1, OnCandidateWrite);
    if (g_handler == nullptr) {
        HT_LOG("CrosshairProbe: could not install an exception handler - cannot "
               "name the writers.");
        return;
    }
    for (size_t i = 0; i < g_candidates.size(); ++i) {
        InterlockedExchange(&g_writerCount, 0);
        const uintptr_t addr = reinterpret_cast<uintptr_t>(g_candidates[i].address);
        SetBreakpointOnAllThreads(kSlot, addr, BreakKind::Write4, true);
        Sleep(kWatchWindowMs);
        SetBreakpointOnAllThreads(kSlot, addr, BreakKind::Write4, false);
        const long n = InterlockedCompareExchange(&g_writerCount, 0, 0);
        if (n == 0) {
            HT_LOG("CrosshairProbe: #%llu 0x%p - no writer seen.",
                   (unsigned long long)i, static_cast<void*>(g_candidates[i].address));
            continue;
        }
        for (long w = 0; w < n && w < kMaxWriters; ++w) {
            HT_LOG("CrosshairProbe: #%llu 0x%p written from RVA 0x%08llX",
                   (unsigned long long)i, static_cast<void*>(g_candidates[i].address),
                   (unsigned long long)(g_writerRips[w] - g_moduleBase));
        }
    }
    RemoveVectoredExceptionHandler(g_handler);
    g_handler = nullptr;
    HT_LOG("CrosshairProbe: writer survey done.");
}

void ProbeThread() {
    // Nothing to look for until the HUD exists, and the scan is what makes the
    // shortlist, so it has to run with the player in world.
    constexpr DWORD kGameplayPollMs = 500;
    constexpr DWORD kHudSettleMs = 5000;
    while (!IsInGameplay()) Sleep(kGameplayPollMs);
    Sleep(kHudSettleMs);
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));

    HT_LOG("CrosshairProbe: scanning for the crosshair's screen position.");
    Scan();
    if (g_candidates.empty()) {
        HT_LOG("CrosshairProbe: no centre-valued float pair anywhere in writable "
               "memory - the crosshair is not stored as a screen position in any "
               "of the layouts searched.");
        return;
    }
    FilterToLiveValues();
    if (g_candidates.empty()) {
        HT_LOG("CrosshairProbe: every match is a constant the game never rewrites, "
               "so none of them is the live crosshair position.");
        return;
    }
    ReportWriters();
}

}  // namespace

void StartCrosshairProbe() {
    std::thread(ProbeThread).detach();
}

}  // namespace NMSHT
