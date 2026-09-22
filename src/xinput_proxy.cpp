// XInput proxy.
//
// NMS.exe statically imports XInputGetState and XInputSetState from
// XINPUT9_1_0.dll. That DLL is not shipped next to the game - Windows
// resolves it from System32. We hijack that load by dropping our shim
// as XINPUT9_1_0.dll into the Binaries folder: the application
// directory is searched before System32 for non-KnownDLLs, so the game
// loads ours and calls our DllMain at startup.
//
// Every export the game (or any other module in the process) might use
// is re-exported here and forwarded to the genuine System32 DLL,
// resolved on first use via LoadLibrary + GetProcAddress and
// tail-called, so controller input is completely unaffected.
//
// The ordinals come from src/xinput.def, not from __declspec(dllexport):
// they have to match the genuine DLL's, because an importer is free to
// bind by ordinal.
//
// Unlike a game-shipped DLL there is no local copy to back up; we load
// the real one straight from the system directory.

#include "pch.h"

#include "core/debug_log.h"
#include "core/early_diag.h"

// --- XInput export signatures (from Xinput.h) -----------------------
//
// Struct pointers are forwarded verbatim as void*; we never inspect
// them, so the exact layout is irrelevant to the proxy. On x64 there is
// a single calling convention, so WINAPI is implicit.

using FnXInputGetState                  = DWORD (*)(DWORD, void*);
using FnXInputSetState                  = DWORD (*)(DWORD, void*);
using FnXInputGetCapabilities           = DWORD (*)(DWORD, DWORD, void*);
using FnXInputGetDSoundAudioDeviceGuids = DWORD (*)(DWORD, void*, void*);

namespace {

HMODULE g_realXInput = nullptr;
std::once_flag g_loadOnce;

// Every forwarding target is resolved in one pass rather than per call. The
// game polls XInputGetState once a frame per pad, and GetProcAddress walks the
// export table by name each time it is asked - work the proxy has no reason to
// repeat for the life of the process.
FnXInputGetState                  g_getState        = nullptr;
FnXInputSetState                  g_setState        = nullptr;
FnXInputGetCapabilities           g_getCapabilities = nullptr;
FnXInputGetDSoundAudioDeviceGuids g_getGuids        = nullptr;

void LoadRealXInput() {
    char sysDir[MAX_PATH] = {};
    UINT n = GetSystemDirectoryA(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    std::string path(sysDir, n);
    path += "\\XINPUT9_1_0.dll";

    g_realXInput = LoadLibraryA(path.c_str());
    if (!g_realXInput) {
        // The game polls the pad from its input loop, which starts inside the
        // init thread's settling delay, so the log is not open yet and HT_LOG
        // would drop this. The early diagnostic is the only channel that works
        // this early, and a dead controller is exactly what the user reports.
        const DWORD err = GetLastError();
        std::string diag = "ERROR: failed to load the system XINPUT9_1_0.dll at ";
        diag += path;
        diag += " (err=" + std::to_string(err) +
                "); controller input will not work until the mod is uninstalled";
        NMSHT::WriteEarlyDiag(diag.c_str());
        HT_LOG("ERROR: Failed to load system XINPUT9_1_0.dll at %s (err=%lu). "
               "Controller input will not work until the mod is uninstalled.",
               path.c_str(), err);
        return;
    }

    g_getState = reinterpret_cast<FnXInputGetState>(
        GetProcAddress(g_realXInput, "XInputGetState"));
    g_setState = reinterpret_cast<FnXInputSetState>(
        GetProcAddress(g_realXInput, "XInputSetState"));
    g_getCapabilities = reinterpret_cast<FnXInputGetCapabilities>(
        GetProcAddress(g_realXInput, "XInputGetCapabilities"));
    g_getGuids = reinterpret_cast<FnXInputGetDSoundAudioDeviceGuids>(
        GetProcAddress(g_realXInput, "XInputGetDSoundAudioDeviceGuids"));

    // A missing export means the forwarder returns "no controller" for the
    // life of the process. Say which one, on both channels, rather than
    // leaving the user with a dead pad and an empty log.
    struct { const char* name; const void* fn; } resolved[] = {
        {"XInputGetState",                  reinterpret_cast<const void*>(g_getState)},
        {"XInputSetState",                  reinterpret_cast<const void*>(g_setState)},
        {"XInputGetCapabilities",           reinterpret_cast<const void*>(g_getCapabilities)},
        {"XInputGetDSoundAudioDeviceGuids", reinterpret_cast<const void*>(g_getGuids)},
    };
    for (const auto& e : resolved) {
        if (e.fn != nullptr) continue;
        std::string msg = "ERROR: the system XINPUT9_1_0.dll does not export ";
        msg += e.name;
        msg += "; that call will report no controller for this session";
        NMSHT::WriteEarlyDiag(msg.c_str());
        HT_LOG("%s.", msg.c_str());
    }
}

// call_once is what publishes the four pointers to every other thread, so it
// stays on the call path rather than being short-circuited by a null check.
void EnsureLoaded() { std::call_once(g_loadOnce, LoadRealXInput); }

} // namespace

// ERROR_DEVICE_NOT_CONNECTED - the documented "no controller" return. Only
// reachable when resolution failed, which LoadRealXInput has already reported
// on both channels.
constexpr DWORD kNotConnected = 1167;

extern "C"
DWORD XInputGetState(DWORD dwUserIndex, void* pState) {
    EnsureLoaded();
    return g_getState ? g_getState(dwUserIndex, pState) : kNotConnected;
}

extern "C"
DWORD XInputSetState(DWORD dwUserIndex, void* pVibration) {
    EnsureLoaded();
    return g_setState ? g_setState(dwUserIndex, pVibration) : kNotConnected;
}

extern "C"
DWORD XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, void* pCapabilities) {
    EnsureLoaded();
    return g_getCapabilities ? g_getCapabilities(dwUserIndex, dwFlags, pCapabilities)
                             : kNotConnected;
}

extern "C"
DWORD XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, void* pRender, void* pCapture) {
    EnsureLoaded();
    return g_getGuids ? g_getGuids(dwUserIndex, pRender, pCapture) : kNotConnected;
}
