#include "pch.h"
#include "frame_phase.h"

#include "camera_hook.h"
#include "read_watch.h"
#include "core/debug_log.h"

#include <cstring>

#include <cameraunlock/hooks/hook_manager.h>

namespace NMSHT {

namespace {

// Declared with opaque handles rather than pulling in the Vulkan headers: the
// mod never inspects the arguments, it only needs to know a frame started or
// finished.
using VoidFn = void (__stdcall*)();
using QueuePresentFn = int (__stdcall*)(void* queue, const void* presentInfo);
using GetDeviceProcAddrFn = VoidFn (__stdcall*)(void* device, const char* name);

QueuePresentFn g_presentOriginal = nullptr;
GetDeviceProcAddrFn g_gdpaOriginal = nullptr;
void* g_gdpaTarget = nullptr;

int __stdcall PresentDetour(void* queue, const void* presentInfo) {
    // No restore here. The engine commits the camera around 20 times a second
    // while presenting at over 100, so restoring per present left the rotation
    // standing for roughly one frame in six and the view did not move at all.
    // The clean camera comes back on the engine's next commit instead.
    CountPresent();
    return g_presentOriginal(queue, presentInfo);
}

// The frame boundaries are intercepted HERE, at the point the engine asks the
// loader for them, rather than by patching vulkan-1.dll's exported
// vkAcquireNextImageKHR / vkQueuePresentKHR.
//
// Patching the exports is what this mod did first, and it never fired a single
// time: NMS imports vkGetDeviceProcAddr and calls the per-device pointers that
// returns, which are the loader's own dispatch entries and not the exported
// symbols. The exports sat patched and unused for the whole session, which
// looked exactly like a mod that was working but had nothing to show.
VoidFn __stdcall GetDeviceProcAddrDetour(void* device, const char* name) {
    VoidFn real = g_gdpaOriginal(device, name);
    if (real == nullptr || name == nullptr) return real;

    if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
        g_presentOriginal = reinterpret_cast<QueuePresentFn>(real);
        return reinterpret_cast<VoidFn>(&PresentDetour);
    }
    return real;
}

enum class InstallState { Pending, NoVulkan, NoExport, NotPinned, HookFailed, Installed };

std::atomic<InstallState> g_state{InstallState::Pending};
const char* volatile g_hookError = "";

// The check whose absence let a dead injection path be mistaken for a working
// one: if the game has been in world for this long and not one frame has
// reached OnRenderPhaseBegin, the mod is hooked to something the renderer never
// calls and must say so instead of sitting there looking installed.
//
// Timed from the game reaching world, NOT from vulkan-1.dll appearing. The
// engine does not commit a camera at the frontend at all, and a cold start
// spends far more than this on logos, the frontend and a save load, so timing
// it from the loader made every launch log this as an ERROR.
constexpr int kWatchdogSeconds = 25;
constexpr int kInWorldPollMs = 250;

// How long to wait for the game to load the Vulkan loader before giving up:
// two minutes, which covers a cold start off a hard disk.
constexpr int kVulkanPollMs = 50;
constexpr int kVulkanPollAttempts = 2400;

void Watchdog() {
    while (!SawInWorld()) {
        if (RenderPhaseBeginCount() != 0) return;
        Sleep(kInWorldPollMs);
    }
    for (int elapsed = 0; elapsed < kWatchdogSeconds * 1000; elapsed += kInWorldPollMs) {
        if (RenderPhaseBeginCount() != 0) return;
        Sleep(kInWorldPollMs);
    }
    if (RenderPhaseBeginCount() != 0) return;
    HT_LOG("ERROR: %d seconds in world and not one frame has reached the head "
           "tracking injection point. The commit-site breakpoint is not being "
           "hit, so head tracking will not appear.",
           kWatchdogSeconds);
}

void InstallThread() {
    // The engine caches whatever GetDeviceProcAddrDetour returns for the life of
    // the device, and there is no way to hand those pointers back. Pin the image
    // so a FreeLibrary can never unmap the code they point at.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(&PresentDetour), &self)) {
        g_hookError = "GetModuleHandleExW with GET_MODULE_HANDLE_EX_FLAG_PIN failed";
        g_state = InstallState::NotPinned;
        return;
    }

    HMODULE vk = nullptr;
    for (int i = 0; i < kVulkanPollAttempts && vk == nullptr; ++i) {
        vk = GetModuleHandleW(L"vulkan-1.dll");
        if (vk == nullptr) Sleep(kVulkanPollMs);
    }
    if (vk == nullptr) {
        g_state = InstallState::NoVulkan;
        return;
    }

    g_gdpaTarget = reinterpret_cast<void*>(GetProcAddress(vk, "vkGetDeviceProcAddr"));
    if (g_gdpaTarget == nullptr) {
        g_state = InstallState::NoExport;
        return;
    }

    auto& hm = cameraunlock::hooks::HookManager::Instance();
    if (hm.Initialize() != cameraunlock::hooks::HookStatus::Ok && !hm.IsInitialized()) {
        g_hookError = "MinHook init failed";
        g_state = InstallState::HookFailed;
        return;
    }

    auto st = hm.CreateHook(g_gdpaTarget, reinterpret_cast<void*>(&GetDeviceProcAddrDetour),
                            reinterpret_cast<void**>(&g_gdpaOriginal));
    if (st == cameraunlock::hooks::HookStatus::Ok) st = hm.EnableHook(g_gdpaTarget);
    if (st != cameraunlock::hooks::HookStatus::Ok) {
        g_hookError = cameraunlock::hooks::HookStatusToString(st);
        g_gdpaTarget = nullptr;
        g_state = InstallState::HookFailed;
        return;
    }

    g_state = InstallState::Installed;
    std::thread(Watchdog).detach();
}

} // namespace

void StartFramePhaseInstaller() {
    std::thread(InstallThread).detach();
}

void LogFramePhaseStatus() {
    switch (g_state.load()) {
        case InstallState::Pending:
            HT_LOG("Frame hooks: still waiting for vulkan-1.dll.");
            break;
        // None of these stops head tracking. The head rotation goes in at the
        // engine's camera commit, through the execute breakpoint in
        // commit_hook.cpp; all this hook does is count presents. Saying
        // "head tracking inactive" here sent a working mod's user to file
        // against the wrong subsystem.
        case InstallState::NoVulkan:
            HT_LOG("WARN: vulkan-1.dll never loaded, so the presents= column "
                   "will stay 0. Head tracking is unaffected.");
            break;
        case InstallState::NoExport:
            HT_LOG("WARN: vkGetDeviceProcAddr not exported by vulkan-1.dll, so "
                   "the presents= column will stay 0. Head tracking is "
                   "unaffected.");
            break;
        case InstallState::NotPinned:
            HT_LOG("ERROR: could not pin this mod's image (%s). Nothing was "
                   "hooked, so the presents= column will stay 0; head tracking "
                   "itself is unaffected.", g_hookError);
            break;
        case InstallState::HookFailed:
            HT_LOG("WARN: could not hook vkGetDeviceProcAddr (%s), so the "
                   "presents= column will stay 0. Head tracking is unaffected.",
                   g_hookError);
            break;
        case InstallState::Installed:
            HT_LOG("Frame hooks installed via vkGetDeviceProcAddr (present "
                   "counting only - the head rotation goes in at the engine's "
                   "camera commit, not at the swapchain).");
            break;
    }
}

} // namespace NMSHT
