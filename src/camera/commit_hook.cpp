#include "pch.h"
#include "commit_hook.h"
#include "build_profile.h"

#include "camera_hook.h"
#include "hw_breakpoint.h"
#include "core/debug_log.h"

namespace NMSHT {

namespace {

uintptr_t g_commitAddr = 0;
// Zero unless this build pins a separate third-person site (the Steam image
// does; the GDK one needs none). Armed in its own debug register.
uintptr_t g_thirdPersonAddr = 0;
PVOID g_handler = nullptr;
volatile long g_hits = 0;
BuildProfile::CommitCameraReg g_cameraReg = BuildProfile::CommitCameraReg::None;

// How often the breakpoint is re-armed, to catch threads the engine spawns
// after the last pass.
constexpr DWORD kRearmIntervalMs = 2000;

// The slot that raised this exception, or -1. DR6 can be cleared while an
// exception is pending, so the armed execute slot and the faulting instruction
// identify the breakpoint in that case: the slot's L bit set, its RW/LEN field
// clear (execute, length 1), and its address register holding the site.
int SlotForBreak(const EXCEPTION_POINTERS* info) {
    const CONTEXT* ctx = info->ContextRecord;
    const auto faultAddr =
        reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const struct { int slot; uintptr_t addr; uintptr_t armed; } sites[] = {
        {kSlotCommit, g_commitAddr, ctx->Dr0},
        {kSlotCommitThirdPerson, g_thirdPersonAddr, ctx->Dr2},
    };
    static_assert(kSlotCommit == 0 && kSlotCommitThirdPerson == 2,
                  "the DR pairing above names Dr0 and Dr2 explicitly");
    for (const auto& s : sites) {
        if (s.addr == 0) continue;
        const std::uint64_t field = (0xFull << (16 + 4 * s.slot)) | (1ull << (2 * s.slot));
        const std::uint64_t wanted = 1ull << (2 * s.slot);
        if (ctx->Rip == s.addr && s.armed == s.addr && faultAddr == s.addr &&
            (ctx->Dr7 & field) == wanted) {
            return s.slot;
        }
    }
    return -1;
}

LONG CALLBACK OnCommitBreak(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    const int slot = SlotForBreak(info);
    if (slot < 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Dr6 &= ~Dr6BitFor(slot);

    InterlockedIncrement(&g_hits);
    void* committed = nullptr;
    switch (g_cameraReg) {
        case BuildProfile::CommitCameraReg::Rcx:
            committed = reinterpret_cast<void*>(ctx->Rcx); break;
        case BuildProfile::CommitCameraReg::Rsi:
            committed = reinterpret_cast<void*>(ctx->Rsi); break;
        case BuildProfile::CommitCameraReg::None:
            break;
    }
    OnRenderPhaseBegin(committed);

    ctx->EFlags |= kEFlagsResumeFlag;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmThread() {
    g_handler = AddVectoredExceptionHandler(1, OnCommitBreak);
    if (g_handler == nullptr) {
        HT_LOG("ERROR: commit hook could not install its exception handler - "
               "head tracking inactive.");
        return;
    }

    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    bool announced = false;
    for (;;) {
        const int n = SetBreakpointOnAllThreads(kSlotCommit, g_commitAddr,
                                                BreakKind::Execute, true);
        int nThird = 0;
        if (g_thirdPersonAddr != 0) {
            nThird = SetBreakpointOnAllThreads(kSlotCommitThirdPerson, g_thirdPersonAddr,
                                               BreakKind::Execute, true);
        }
        if (!announced) {
            HT_LOG("Commit hook armed at RVA 0x%08llX on %d threads - the head "
                   "rotation goes in right after the engine writes the camera.",
                   (unsigned long long)(g_commitAddr - base), n);
            if (g_thirdPersonAddr != 0) {
                HT_LOG("Commit hook armed at RVA 0x%08llX on %d threads for the "
                       "third-person cameras.",
                       (unsigned long long)(g_thirdPersonAddr - base), nThird);
            }
            announced = true;
        }
        Sleep(kRearmIntervalMs);
    }
}

} // namespace

void InstallCommitHook(std::uint32_t commitRva, std::uint32_t thirdPersonCommitRva,
                       BuildProfile::CommitCameraReg cameraReg) {
    g_cameraReg = cameraReg;
    if (commitRva == 0) {
        HT_LOG("Head tracking DORMANT: this build has no camera-commit RVA "
               "pinned, and the swapchain window does not reach the renderer.");
        return;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_commitAddr = base + commitRva;
    if (thirdPersonCommitRva != 0) {
        g_thirdPersonAddr = base + thirdPersonCommitRva;
    } else {
        HT_LOG("Commit hook: no third-person site pinned for this build, so the "
               "view follows your head in first person only.");
    }
    std::thread(ArmThread).detach();
}

std::uint64_t CommitHitCount() {
    return static_cast<std::uint64_t>(InterlockedCompareExchange(&g_hits, 0, 0));
}


} // namespace NMSHT
