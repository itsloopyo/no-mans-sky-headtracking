#include "pch.h"
#include "early_diag.h"

#include <cstdio>

namespace NMSHT {
namespace {

// Truncated on the first SUCCESSFUL write of each launch so the file only ever
// holds the session the user is reporting, then appended to for the rest of it.
// Latching before the file exists would let one failed CreateFile leave the
// previous session's lines in place, which is exactly the evidence a triage
// session would misread.
volatile LONG g_truncated = 0;

} // namespace

void WriteEarlyDiag(const char* message) {
    // Resolved from this function's own address rather than from a global the
    // caller has to remember to set: a null module handle would silently send
    // the diagnostic to the GAME's path instead of ours.
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(&WriteEarlyDiag), &self)) {
        return;
    }

    char dllPath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(self, dllPath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    std::string diagPath(dllPath, len);
    const auto dot = diagPath.rfind('.');
    if (dot != std::string::npos) diagPath = diagPath.substr(0, dot);
    diagPath += "_diag.txt";

    const bool first = InterlockedCompareExchange(&g_truncated, 1, 1) == 0;
    HANDLE h = CreateFileA(diagPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           first ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    InterlockedExchange(&g_truncated, 1);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1024];
    const int n = std::snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n",
                                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                                message);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(h, line, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(h);
}

} // namespace NMSHT
