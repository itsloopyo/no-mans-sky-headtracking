#include <set>
#include "pch.h"
#include "hw_breakpoint.h"

#include <tlhelp32.h>

namespace NMSHT {

namespace {

// DR7 packs, per slot, a local-enable bit at 2*slot and a four-bit RW/LEN
// nibble at 16 + 4*slot. RW is the low half (00 execute, 01 write, 11 read or
// write) and LEN the high half (00 one byte, 11 four bytes).
constexpr std::uint64_t LocalEnableBit(int slot) { return 1ull << (2 * slot); }
constexpr int NibbleShift(int slot) { return 16 + 4 * slot; }

std::uint64_t NibbleFor(BreakKind kind) {
    switch (kind) {
        case BreakKind::Write4:     return 0b1101ull;  // RW=01, LEN=11
        case BreakKind::ReadWrite4: return 0b1111ull;  // RW=11, LEN=11
        case BreakKind::Execute:    break;
    }
    return 0b0000ull;  // RW=00, LEN=00
}

// Everything outside this belongs to another slot and must survive the re-arm.
std::uint64_t SlotMask(int slot) {
    return LocalEnableBit(slot) | (0xFull << NibbleShift(slot));
}

bool ApplyToContext(CONTEXT& ctx, int slot, std::uintptr_t address,
                    BreakKind kind, bool arm) {
    const DWORD64 value = arm ? static_cast<DWORD64>(address) : 0;
    DWORD64* reg;
    switch (slot) {
        case 0: reg = &ctx.Dr0; break;
        case 1: reg = &ctx.Dr1; break;
        case 2: reg = &ctx.Dr2; break;
        default: reg = &ctx.Dr3; break;
    }
    const DWORD64 dr7 = arm ? ((ctx.Dr7 & ~SlotMask(slot)) |
                     LocalEnableBit(slot) | (NibbleFor(kind) << NibbleShift(slot)))
                  : (ctx.Dr7 & ~SlotMask(slot));
    // Rewriting an unchanged slot can erase the status of a pending exception.
    if (*reg == value && ctx.Dr7 == dr7) return false;
    *reg = value;
    ctx.Dr6 &= ~Dr6BitFor(slot);
    ctx.Dr7 = dr7;
    return true;
}

}  // namespace

int SetBreakpointOnAllThreads(int slot, std::uintptr_t address, BreakKind kind,
                              bool arm) {
    // Every sweep is a read-modify-write of another thread's DR7, and up to
    // four of them run on their own timers. Two sweeps interleaving on the same
    // target thread is a lost update: the loser's Get happens before the
    // winner's Set, and its own Set puts back a DR7 with the winner's enable bit
    // and RW/LEN nibble cleared. When the commit slot loses, the view stops
    // following the head until that sweep's next pass - up to two seconds, and
    // self-healing, which is what makes it impossible to diagnose from a report.
    //
    // Only threads this slot has not already been armed on are touched. The
    // sweep exists to catch threads the engine spawns after the last pass, and
    // suspending every thread in the process on a two-second timer is not free:
    // No Man's Sky runs 45 worker threads through a global load, and suspending
    // one that holds a lock the rest are waiting on converts a load that takes
    // seconds into one that does not visibly finish.
    static std::set<DWORD> s_armed[4];
    // Windows reuses thread ids, so a thread that inherits a retired id would
    // be skipped forever. Every fifteenth pass forgets what it knows and sweeps
    // the process properly, which also re-arms any thread whose DR7 was
    // clobbered elsewhere. At a two-second timer that is one full sweep every
    // thirty seconds instead of every two.
    static int s_pass[4];
    const bool fullSweep = arm && (++s_pass[slot] % 15) == 0;
    if (fullSweep) s_armed[slot].clear();
    static std::mutex s_sweepMutex;
    std::lock_guard<std::mutex> sweepLock(s_sweepMutex);

    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int touched = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            if (arm && !s_armed[slot].insert(te.th32ThreadID).second &&
                !fullSweep) continue;
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT |
                                       THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (th == nullptr) continue;
            if (SuspendThread(th) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(th, &ctx)) {
                    if (!ApplyToContext(ctx, slot, address, kind, arm) ||
                        SetThreadContext(th, &ctx)) ++touched;
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (!arm) s_armed[slot].clear();
    return touched;
}

}  // namespace NMSHT
