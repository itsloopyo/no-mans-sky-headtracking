#pragma once

#include <cstdint>

namespace NMSHT {

// x86 debug-register breakpoints, armed across every thread in the process.
//
// A breakpoint lives in a thread's debug registers, so a thread the engine
// spawns later has none: callers re-arm periodically rather than arming once.

enum class BreakKind {
    Execute,     // raised BEFORE the instruction at the address runs
    Write4,      // four bytes, writes only
    ReadWrite4,  // four bytes, reads or writes (x86 has no read-only watch)
};

// Keep the diagnostic slots stable across builds.
constexpr int kSlotCommit         = 0;
constexpr int kSlotSceneSample    = 1;
constexpr int kSlotDataWatch      = 2;
constexpr int kSlotCrosshairProbe = 3;

// x86 has four of these and no more. The third-person commit site shares the
// data-watch slot because that watch is a diagnostic nobody runs in play, and
// camera_hook leaves the third-person site unarmed when the watch is on.
constexpr int kSlotCommitThirdPerson = kSlotDataWatch;

// Bit in DR6 that a hit on `slot` sets, for a handler to test and clear.
constexpr std::uint64_t Dr6BitFor(int slot) { return 1ull << slot; }

// EFlags.RF. An execute breakpoint is raised before its instruction runs, so a
// handler that resumes without setting this faults on the same instruction
// again the moment it continues.
constexpr std::uint32_t kEFlagsResumeFlag = 0x10000;

// Arms or clears `slot` on every thread but this one. Returns how many threads
// were successfully updated.
int SetBreakpointOnAllThreads(int slot, std::uintptr_t address, BreakKind kind,
                              bool arm);

}  // namespace NMSHT
