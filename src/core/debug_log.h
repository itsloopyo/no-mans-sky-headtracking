#pragma once

#include "core/constants.h"

#include <string>

#include <cameraunlock/logging/file_log.h>

namespace NMSHT {

// Opens HeadTracking.log next to the game EXE (truncated each launch).
// HT_LOG lines emitted before this are dropped, matching file_log's
// not-open behavior.
inline void OpenLogFile() {
    wchar_t buf[MAX_PATH] = {};
    // GetModuleFileNameW does not guarantee null-termination on truncation,
    // so bound the path by the returned length.
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring dir;
    if (len > 0 && len < MAX_PATH) {
        std::wstring exe(buf, len);
        const auto slash = exe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir = exe.substr(0, slash + 1);
    }
    cameraunlock::logging::Open(dir + kLogFileName);
}

inline void CloseLogFile() {
    cameraunlock::logging::Close();
}

} // namespace NMSHT

#define HT_LOG(...) ::cameraunlock::logging::Line(__VA_ARGS__)
