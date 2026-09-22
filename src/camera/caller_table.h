#pragma once

#include <cstdint>
#include <mutex>

#include "core/debug_log.h"

namespace NMSHT {

// Distinct call sites seen for one hooked function, so a single-caller
// assumption is proven rather than assumed: another consumer (audio listener,
// culling) reading the same camera transform would receive the tracked
// orientation and break aim decoupling.
//
// The whole point of the table is to find out whether a second thread calls
// the hooked function, so it must be safe while that is still unknown: two
// racing appends would otherwise both pass the capacity check and Report()
// would then walk off the end of the array.
class CallerTable {
public:
    // Sized for the source accessor, which turned out to have far more callers
    // than the copy accessor's two. At 16 the table filled with one-shot
    // startup sites and every later call - including the per-frame ones this
    // exists to find - fell into the overflow counter instead.
    static constexpr int kMaxCallers = 512;

    explicit CallerTable(const char* label) : m_label(label) {}

    // Logging every newly seen call site is discovery output, not something a
    // player wants in their log on a build that already knows its consumer.
    void SetVerbose(bool verbose) { m_verbose = verbose; }

    // Index of the call site this call came from, or -1 when the site is new
    // and the table is already full.
    int Record(uintptr_t returnAddr, uintptr_t moduleBase) {
        const uintptr_t rva = returnAddr - moduleBase;
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int i = 0; i < m_count; ++i) {
            if (m_entries[i].rva == rva) {
                ++m_entries[i].count;
                return i;
            }
        }
        if (m_count < kMaxCallers) {
            const int idx = m_count;
            m_entries[idx].rva = rva;
            m_entries[idx].count = 1;
            ++m_count;
            if (m_verbose) {
                HT_LOG("Diag: %s new call site #%d at RVA 0x%08llX",
                       m_label, idx, (unsigned long long)rva);
            }
            return idx;
        }
        ++m_overflow;
        return -1;
    }

    void Report(uint64_t totalCalls) {
        std::lock_guard<std::mutex> lock(m_mutex);
        HT_LOG("Diag: %s %llu calls total across %d call sites:",
               m_label, (unsigned long long)totalCalls, m_count);
        for (int i = 0; i < m_count; ++i) {
            HT_LOG("  site RVA 0x%08llX -> %llu calls",
                   (unsigned long long)m_entries[i].rva,
                   (unsigned long long)m_entries[i].count);
        }
        // Without this the per-site counts silently fail to add up to the
        // total, which reads as "these are all the callers" when they are not.
        if (m_overflow != 0) {
            HT_LOG("  (table full: %llu further calls from unrecorded sites)",
                   (unsigned long long)m_overflow);
        }
    }

    // True when totalCalls has advanced a further `interval` since the last
    // time this returned true, so a hot hook reports periodically rather than
    // every call.
    bool DueForReport(uint64_t totalCalls, uint64_t interval) {
        std::lock_guard<std::mutex> lock(m_mutex);
        // Not `totalCalls - m_lastReport`. totalCalls is a pre-increment value
        // handed in from ~100 threads, so it arrives out of order; the
        // subtraction then underflows in uint64_t for the thread that lost the
        // race, which dumps the whole table a second time and walks
        // m_lastReport backwards.
        if (totalCalls < m_lastReport + interval) return false;
        m_lastReport = totalCalls;
        return true;
    }

private:
    struct Entry {
        uintptr_t rva;
        uint64_t count;
    };

    const char* m_label;
    bool m_verbose = false;
    Entry m_entries[kMaxCallers]{};
    int m_count = 0;
    uint64_t m_overflow = 0;
    uint64_t m_lastReport = 0;
    std::mutex m_mutex;
};

}  // namespace NMSHT
