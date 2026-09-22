#include "pch.h"
#include "commit_hook.h"
#include "build_profile.h"

#include "camera_hook.h"
#include "hw_breakpoint.h"
#include "core/debug_log.h"

namespace NMSHT {

namespace {

uintptr_t g_commitAddr = 0;
PVOID g_handler = nullptr;
volatile long g_hits = 0;
BuildProfile::CommitCameraReg g_cameraReg = BuildProfile::CommitCameraReg::None;

constexpr int kSlot = kSlotCommit;

// How often the breakpoint is re-armed, to catch threads the engine spawns
// after the last pass.
constexpr DWORD kRearmIntervalMs = 2000;

LONG CALLBACK OnCommitBreak(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    // DR6 can be cleared while an exception is pending. The armed execute
    // slot and faulting instruction identify our breakpoint even in that case.
    static_assert(kSlot == 0);
    if (ctx->Rip != g_commitAddr || ctx->Dr0 != g_commitAddr ||
        reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) != g_commitAddr ||
        (ctx->Dr7 & 0xF0001ull) != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Dr6 &= ~Dr6BitFor(kSlot);

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

    bool announced = false;
    for (;;) {
        const int n = SetBreakpointOnAllThreads(kSlot, g_commitAddr,
                                                BreakKind::Execute, true);
        if (!announced) {
            HT_LOG("Commit hook armed at RVA 0x%08llX on %d threads - the head "
                   "rotation goes in right after the engine writes the camera.",
                   (unsigned long long)(g_commitAddr - reinterpret_cast<uintptr_t>(
                                            GetModuleHandleW(nullptr))),
                   n);
            announced = true;
        }
        Sleep(kRearmIntervalMs);
    }
}

} // namespace

void InstallCommitHook(std::uint32_t commitRva,
                       BuildProfile::CommitCameraReg cameraReg) {
    g_cameraReg = cameraReg;
    if (commitRva == 0) {
        HT_LOG("Head tracking DORMANT: this build has no camera-commit RVA "
               "pinned, and the swapchain window does not reach the renderer.");
        return;
    }
    g_commitAddr = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + commitRva;
    std::thread(ArmThread).detach();
}

std::uint64_t CommitHitCount() {
    return static_cast<std::uint64_t>(InterlockedCompareExchange(&g_hits, 0, 0));
}


} // namespace NMSHT
