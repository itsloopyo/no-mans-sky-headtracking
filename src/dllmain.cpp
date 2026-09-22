#include "pch.h"

#include "core/mod.h"
#include "core/early_diag.h"
#include "camera/frame_phase.h"
#include "core/window_centering.h"

static HMODULE g_hModule = nullptr;

// The engine is still building itself when the proxy loads, and Initialize
// resolves RTTI out of the running module. Letting the process settle first is
// cheaper than retrying the resolution.
static constexpr DWORD kInitDelayMs = 3000;

static DWORD WINAPI InitThread(LPVOID) {
    Sleep(kInitDelayMs);
    // Both outcomes go to the early diagnostic, not just the failure. It is the
    // only channel that survives [General] LogToFile=false, and without the
    // success line "the shim loaded but the init thread is stuck" and "init ran,
    // read the log" are the same empty file.
    if (NMSHT::Mod::Instance().Initialize(g_hModule)) {
        NMSHT::WriteEarlyDiag("InitThread: Mod::Initialize OK");
    } else {
        NMSHT::WriteEarlyDiag("InitThread: Mod::Initialize FAILED");
    }

    // Last, because it blocks until the game has a window that has stopped
    // moving: nothing above it may wait on this.
    NMSHT::CenterWindowWhenReady();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        g_hModule = hModule;
        NMSHT::WriteEarlyDiag("DllMain: DLL_PROCESS_ATTACH");

        // Before the init thread's settling delay, not after it: the engine
        // asks vkGetDeviceProcAddr for its frame entry points once, while it
        // creates the device, and a hook that lands later never sees a frame.
        NMSHT::StartFramePhaseInstaller();

        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    // No DLL_PROCESS_DETACH branch: the image is pinned and nothing it installs
    // can be revoked, so there is no unload path to run down.
    (void)lpReserved;
    return TRUE;
}
