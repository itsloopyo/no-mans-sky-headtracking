#include "../src/camera/commit_hook.cpp"
#include "../src/camera/hw_breakpoint.cpp"

#include <cstdlib>

namespace {
std::atomic<unsigned long> renders{0};
std::atomic<unsigned long> escaped{0};
std::atomic<unsigned long> missingStatus{0};
std::atomic<bool> eraseStatus{false};
volatile LONG executed = 0;
int failures = 0;

void Check(bool passed, const char* label) {
    if (!passed) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

__declspec(noinline) void CommitSite() {
    InterlockedIncrement(&executed);
}

LONG CALLBACK Dispatch(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP ||
        info->ContextRecord->Rip != NMSHT::g_commitAddr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if ((info->ContextRecord->Dr6 & 1) == 0) ++missingStatus;
    if (eraseStatus.load()) info->ContextRecord->Dr6 &= ~1ull;
    const LONG result = NMSHT::OnCommitBreak(info);
    if (result == EXCEPTION_CONTINUE_SEARCH) {
        ++escaped;
        info->ContextRecord->Dr6 &= ~1ull;
        info->ContextRecord->EFlags |= NMSHT::kEFlagsResumeFlag;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

void ContextRegression() {
    CONTEXT ctx{};
    ctx.Rip = NMSHT::g_commitAddr;
    ctx.Dr0 = ctx.Rip;
    ctx.Dr7 = 1;
    ctx.EFlags = 0x246;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_SINGLE_STEP;
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    EXCEPTION_POINTERS info{&record, &ctx};
    const auto before = renders.load();
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_EXECUTION,
          "dump context with DR6 zero is handled");
    Check(renders.load() == before + 1, "camera callback runs once");
    Check((ctx.EFlags & NMSHT::kEFlagsResumeFlag) != 0,
          "resume flag prevents recurring execution breakpoint");

    ctx.Rip += 8;
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    ctx.Dr6 = 1;
    ctx.EFlags = 0x246;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "stale status at another instruction is not swallowed");
    Check(ctx.Dr6 == 1 && ctx.EFlags == 0x246, "foreign context is unchanged");

    ctx.Rip = NMSHT::g_commitAddr;
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    ctx.Dr7 = 0;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "unarmed slot does not claim a debugger step");
    ctx.Dr7 = 1;
    ctx.Dr0 += 8;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "another slot owner is not claimed");
    ctx.Dr0 = ctx.Rip;
    ctx.Dr7 = 0x10001;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "a data breakpoint is not claimed as a camera commit");
    ctx.Dr7 = 1;
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip + 8);
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "a different exception address is not claimed");
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_SEARCH,
          "access violations are not swallowed");

    record.ExceptionCode = EXCEPTION_SINGLE_STEP;
    ctx.Dr6 = 5;
    Check(NMSHT::OnCommitBreak(&info) == EXCEPTION_CONTINUE_EXECUTION,
          "normal commit is handled");
    Check(ctx.Dr6 == 4, "another slot's status survives commit handling");

    ctx.Dr6 = 5;
    Check(!NMSHT::ApplyToContext(ctx, 0, ctx.Rip, NMSHT::BreakKind::Execute, true),
          "unchanged breakpoint does not rewrite thread context");
    Check(ctx.Dr6 == 5, "rearming preserves pending exception status");
    Check(NMSHT::ApplyToContext(ctx, 0, ctx.Rip + 8, NMSHT::BreakKind::Execute, true),
          "changed address requires thread context update");
    Check(ctx.Dr6 == 4, "changing one slot preserves another slot's status");
    const auto commitAddress = ctx.Dr0;
    ctx.Dr6 = 5;
    Check(NMSHT::ApplyToContext(ctx, 2, 0x1000, NMSHT::BreakKind::Write4, true),
          "diagnostic slot can be armed beside commit");
    Check(ctx.Dr0 == commitAddress && (ctx.Dr7 & 1) == 1 && ctx.Dr6 == 1,
          "arming diagnostic slot preserves commit configuration and status");
    Check(NMSHT::ApplyToContext(ctx, 2, 0, NMSHT::BreakKind::Write4, false),
          "diagnostic slot can be disabled");
    Check(ctx.Dr0 == commitAddress && ctx.Dr7 == 1 && ctx.Dr6 == 1,
          "disabling diagnostic slot preserves commit");
}

void Stress(unsigned seconds, bool clearStatus, bool paced = false) {
    eraseStatus.store(clearStatus);
    escaped.store(0);
    missingStatus.store(0);
    const auto rendersBefore = renders.load();
    PVOID handler = AddVectoredExceptionHandler(1, Dispatch);
    if (!handler) std::abort();
    std::atomic<bool> stop{false};
    std::thread workers[3];
    for (auto& worker : workers) {
        worker = std::thread([&] {
            while (!stop.load()) {
                CommitSite();
                if (paced) Sleep(1);
                else SwitchToThread();
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    auto reported = start;
    unsigned sweeps = 0;
    do {
        NMSHT::SetBreakpointOnAllThreads(0, NMSHT::g_commitAddr,
                                         NMSHT::BreakKind::Execute, true);
        ++sweeps;
        Sleep(paced ? 20 : 1);
        const auto now = std::chrono::steady_clock::now();
        if (now - reported >= std::chrono::seconds(60)) {
            std::printf("progress: sweeps=%u callbacks=%lu missing-status=%lu escaped=%lu\n",
                        sweeps, renders.load() - rendersBefore,
                        missingStatus.load(), escaped.load());
            std::fflush(stdout);
            reported = now;
        }
    } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds));
    stop.store(true);
    for (auto& worker : workers) worker.join();
    NMSHT::SetBreakpointOnAllThreads(0, 0, NMSHT::BreakKind::Execute, false);
    if (!RemoveVectoredExceptionHandler(handler)) std::abort();
    std::printf("stress: clear=%d sweeps=%u callbacks=%lu missing-status=%lu escaped=%lu\n",
                clearStatus, sweeps, renders.load() - rendersBefore, missingStatus.load(), escaped.load());
    Check(escaped.load() == 0, "no commit exception escapes during rearming");
    Check(renders.load() > rendersBefore, "hardware breakpoint actually fires");
}
}

namespace NMSHT {
// The camera the breakpoint reports, so the filter below can be checked.
std::atomic<void*> lastCommitted{nullptr};
void OnRenderPhaseBegin(void* committedCamera) {
    lastCommitted.store(committedCamera, std::memory_order_relaxed);
    ++renders;
}
}

namespace {
// Which camera the handler reports is what lets camera_hook drop a commit that
// belongs to another camera - the thing that made the Game Pass reticle step,
// because its commit is one routine shared by every camera behaviour.
void CommitCameraIsReported() {
    CONTEXT ctx{};
    ctx.Rip = NMSHT::g_commitAddr;
    ctx.Dr0 = ctx.Rip;
    ctx.Dr7 = 1;
    ctx.Dr6 = 1;
    ctx.EFlags = 0x246;
    ctx.Rcx = 0xC0FFEE;
    ctx.Rsi = 0xBEEF;
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_SINGLE_STEP;
    record.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
    EXCEPTION_POINTERS info{&record, &ctx};

    const auto reg = NMSHT::g_cameraReg;

    NMSHT::g_cameraReg = NMSHT::BuildProfile::CommitCameraReg::None;
    NMSHT::lastCommitted.store(reinterpret_cast<void*>(1), std::memory_order_relaxed);
    ctx.Dr6 = 1; ctx.EFlags = 0x246;
    NMSHT::OnCommitBreak(&info);
    Check(NMSHT::lastCommitted.load() == nullptr,
          "no register named reports no camera, so every commit is acted on");

    NMSHT::g_cameraReg = NMSHT::BuildProfile::CommitCameraReg::Rcx;
    ctx.Dr6 = 1; ctx.EFlags = 0x246;
    NMSHT::OnCommitBreak(&info);
    Check(NMSHT::lastCommitted.load() == reinterpret_cast<void*>(0xC0FFEE),
          "rcx is reported when the profile names it");

    NMSHT::g_cameraReg = NMSHT::BuildProfile::CommitCameraReg::Rsi;
    ctx.Dr6 = 1; ctx.EFlags = 0x246;
    NMSHT::OnCommitBreak(&info);
    Check(NMSHT::lastCommitted.load() == reinterpret_cast<void*>(0xBEEF),
          "rsi is reported when the profile names it");

    NMSHT::g_cameraReg = reg;
}
}

int main(int argc, char** argv) {
    NMSHT::g_commitAddr = reinterpret_cast<uintptr_t>(&CommitSite);
    if (argc >= 2) {
        Stress(static_cast<unsigned>(std::stoul(argv[1])), argc == 4, argc >= 3);
    } else {
        ContextRegression();
        CommitCameraIsReported();
        Stress(1, false);
        Stress(1, true);
    }
    return failures == 0 ? 0 : 1;
}
