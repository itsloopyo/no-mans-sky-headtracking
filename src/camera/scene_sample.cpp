#include "pch.h"
#include "scene_sample.h"

#include "camera_hook.h"
#include "hw_breakpoint.h"
#include "core/debug_log.h"

namespace NMSHT {

namespace {

uintptr_t g_sampleAddr = 0;
volatile long g_hits = 0;

constexpr int kSlot = kSlotSceneSample;
constexpr DWORD kRearmIntervalMs = 2000;

LONG CALLBACK OnSampleBreak(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = info->ContextRecord;
    if (ctx->Rip != g_sampleAddr || (ctx->Dr6 & Dr6BitFor(kSlot)) == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ctx->Dr6 &= ~Dr6BitFor(kSlot);
    InterlockedIncrement(&g_hits);
    NoteSceneSample();
    ctx->EFlags |= kEFlagsResumeFlag;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ArmThread() {
    if (AddVectoredExceptionHandler(1, OnSampleBreak) == nullptr) {
        HT_LOG("ERROR: scene sample hook could not install its exception handler - "
               "the reticle follows the newest commit instead.");
        return;
    }
    bool announced = false;
    for (;;) {
        const int n = SetBreakpointOnAllThreads(kSlot, g_sampleAddr, BreakKind::Execute, true);
        if (!announced) {
            HT_LOG("Scene sample hook armed at RVA 0x%08llX on %d threads - the "
                   "reticle is placed from the commit the frame was set up with.",
                   (unsigned long long)(g_sampleAddr - reinterpret_cast<uintptr_t>(
                                            GetModuleHandleW(nullptr))),
                   n);
            announced = true;
        }
        Sleep(kRearmIntervalMs);
    }
}

}  // namespace

void InstallSceneSampleHook(std::uint32_t sampleRva) {
    if (sampleRva == 0) {
        HT_LOG("Reticle: no scene sample site pinned for this build, so the reticle "
               "is placed from the newest camera commit.");
        return;
    }
    g_sampleAddr = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + sampleRva;
    std::thread(ArmThread).detach();
}

std::uint64_t SceneSampleCount() {
    return static_cast<std::uint64_t>(InterlockedCompareExchange(&g_hits, 0, 0));
}

}  // namespace NMSHT
