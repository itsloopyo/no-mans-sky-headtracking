#include "pch.h"
#include "weapon_probe.h"

#include "camera_hook.h"
#include "camera_math.h"
#include "read_watch.h"
#include "core/debug_log.h"

#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace NMSHT {

namespace {

constexpr size_t kMaxRegionBytes = 256u * 1024u * 1024u;
constexpr int    kMaxLogged      = 40;
// Each candidate keeps only rows 0 and 2 - enough for both the stability
// compare and the angle test - so the cap can be high enough that a real scene
// never reaches it. A truncated pass A silently returns "nothing turned", which
// reads exactly like a negative result and is how one run was wasted.
constexpr size_t kMaxCandidates  = 4000000;

// Anything parented to the eye - the multitool, the arms, a held light - is
// within a couple of metres of it. Widening this past a room's width just
// collects scenery.
constexpr float kNearCameraM = 6.0f;

constexpr float kProbeYawA = 0.0f;
constexpr float kProbeYawB = 40.0f;

bool Readable(const MEMORY_BASIC_INFORMATION& mbi) {
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    // MEM_MAPPED is excluded for the same reason the crosshair probe excludes
    // it: NMS maps its archives, and paging them in off disk once cost 9 GB of
    // reads over 128 seconds and starved the game's own loading.
    if (mbi.Type != MEM_PRIVATE && mbi.Type != MEM_IMAGE) return false;
    // Read-only protections are excluded because the thing being hunted is a
    // node transform the engine rewrites every frame, which cannot live in one.
    const DWORD ok = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & ok) != 0;
}

float Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool IsBasis(const float* r) {
    const float l1 = Dot(r + 4, r + 4);
    const float l2 = Dot(r + 8, r + 8);
    if (std::fabs(l1 - 1.0f) > 0.005f) return false;
    if (std::fabs(l2 - 1.0f) > 0.005f) return false;
    if (std::fabs(Dot(r, r + 4)) > 0.005f) return false;
    if (std::fabs(Dot(r, r + 8)) > 0.005f) return false;
    if (std::fabs(Dot(r + 4, r + 8)) > 0.005f) return false;
    return true;
}

// A thread stack is a reservation whose committed pages sit above a PAGE_GUARD
// page, so the region immediately below the one holding the address gives it
// away. This matters because the engine builds transforms in stack temporaries
// that match every test the scan applies, and watching one of those for writes
// reports thousands of hits across dozens of unrelated modules - the signature
// of ordinary stack reuse, not of a node being placed.
bool LooksLikeThreadStack(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) !=
        sizeof(mbi)) {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (base == 0) return false;
    MEMORY_BASIC_INFORMATION below{};
    if (VirtualQuery(reinterpret_cast<void*>(base - 1), &below, sizeof(below)) !=
        sizeof(below)) {
        return false;
    }
    return (below.Protect & PAGE_GUARD) != 0;
}

struct Candidate {
    uintptr_t address;
    float r0[3];
    float r2[3];
};

bool g_capped = false;

void CollectNearCamera(const float* cam, std::vector<Candidate>& out,
                       uint64_t& bytesScanned) {
    std::vector<unsigned char> buf;
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = 0;
    while (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) ==
           sizeof(mbi)) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const size_t size = mbi.RegionSize;
        addr = base + size;
        if (addr <= base) break;
        if (!Readable(mbi) || size > kMaxRegionBytes || size < 0x40) continue;

        buf.resize(size);
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), mbi.BaseAddress, buf.data(),
                               size, &got) || got < 0x40) {
            continue;
        }
        bytesScanned += got;

        const unsigned char* p = buf.data();
        for (size_t off = 0; off + 0x40 <= got; off += 4) {
            const float* r = reinterpret_cast<const float*>(p + off);
            // Cheap gate first: row 0 has to be a unit vector. Everything after
            // this runs on a tiny fraction of the address space.
            const float l0 = Dot(r, r);
            if (std::fabs(l0 - 1.0f) > 0.005f) continue;
            if (!IsBasis(r)) continue;

            const float* pos = r + 12;
            const float dx = pos[0] - cam[12];
            const float dy = pos[1] - cam[13];
            const float dz = pos[2] - cam[14];
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(dz)) continue;
            if (dx * dx + dy * dy + dz * dz > kNearCameraM * kNearCameraM) continue;

            Candidate c{};
            c.address = base + off;
            std::memcpy(c.r0, r, sizeof(c.r0));
            std::memcpy(c.r2, r + 8, sizeof(c.r2));
            out.push_back(c);
            if (out.size() >= kMaxCandidates) { g_capped = true; return; }
        }
    }
}

void ProbeThread() {
    for (int i = 0; AppliedCameraRows() == nullptr; ++i) {
        if (i == 120 || (i > 0 && i % 600 == 0)) {
            HT_LOG("WeaponProbe: waiting for a tracker pose to be applied "
                   "(%d minutes so far).", i / 120);
        }
        Sleep(500);
    }
    // Long settle on purpose. The multitool holsters when idle, and a scan run
    // against a scene with no viewmodel in it returns a confident negative that
    // means nothing - which is exactly what happened on the first run. This
    // window is for getting the tool drawn and keeping it drawn.
    HT_LOG("WeaponProbe: pose applied. Scanning in 25s - get the multitool OUT "
           "and keep it out for the next minute.");
    Sleep(25000);

    if (!FreezeWithYaw(kProbeYawA)) {
        HT_LOG("WeaponProbe: could not freeze the camera.");
        return;
    }
    Sleep(600);

    float cam[20];
    const float* frozen = AppliedCameraRows();
    if (frozen == nullptr) {
        // FreezeWithYaw publishes the rows and then sets the freeze flag, so a
        // commit landing between the two reaches StandDown and clears them
        // again. Narrow, and this exact null class has taken the game down once
        // already (crash 170671_0x2B33EB8).
        HT_LOG("WeaponProbe: the camera stood down between the freeze and the "
               "read - aborting rather than dereferencing a null basis.");
        UnfreezeAppliedRows();
        return;
    }
    for (int i = 0; i < 20; ++i) cam[i] = frozen[i];

    HT_LOG("WeaponProbe: pass A frozen at yaw %.0f, camera at [%.2f %.2f %.2f]. "
           "The view will hold still until both passes finish. Only private and "
           "image regions that are writable are scanned, so a node in a mapped or "
           "read-only pool would not be found and 'nothing turned' is not proof "
           "of absence.",
           kProbeYawA, cam[12], cam[13], cam[14]);

    std::vector<Candidate> cands;
    uint64_t bytes = 0;
    const DWORD t0 = GetTickCount();
    CollectNearCamera(cam, cands, bytes);
    HT_LOG("WeaponProbe: pass A scanned %llu MB in %lums, %zu oriented nodes "
           "within %.0fm of the eye.",
           (unsigned long long)(bytes / (1024 * 1024)), GetTickCount() - t0,
           cands.size(), kNearCameraM);
    if (g_capped) {
        HT_LOG("WeaponProbe: WARNING - hit the %zu candidate cap and stopped "
               "scanning partway. Anything this run reports as absent may "
               "simply be in the memory it never reached. This is NOT a "
               "negative result.", kMaxCandidates);
    }

    // Anything that merely CHANGED is not evidence: the heap churns hard enough
    // that thousands of these addresses hold unrelated data a second later. Two
    // things make the test decisive. First a stability pass at the SAME angle,
    // which drops everything that moves on its own. Then the survivors have to
    // rotate by the angle actually injected - a node parented to the eye turns
    // with it exactly, while unrelated data that happens to change does not
    // land on 40 degrees.
    Sleep(1500);
    std::vector<Candidate> stable;
    for (const Candidate& c : cands) {
        float now[12];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               reinterpret_cast<void*>(c.address), now,
                               sizeof(now), &got) || got != sizeof(now)) {
            continue;
        }
        if (std::memcmp(now, c.r0, sizeof(c.r0)) == 0 &&
            std::memcmp(now + 8, c.r2, sizeof(c.r2)) == 0) {
            stable.push_back(c);
        }
    }
    HT_LOG("WeaponProbe: %zu of %zu held still at the same angle - those are the "
           "ones worth turning.", stable.size(), cands.size());

    if (!FreezeWithYaw(kProbeYawB)) {
        HT_LOG("WeaponProbe: could not re-freeze for pass B.");
        UnfreezeAppliedRows();
        return;
    }
    Sleep(1500);

    const float wantDeg = kProbeYawB - kProbeYawA;
    size_t turned = 0;
    int logged = 0;
    uintptr_t best = 0;
    float bestScore = 1e9f;
    for (const Candidate& c : stable) {
        float now[16];
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               reinterpret_cast<void*>(c.address), now,
                               sizeof(now), &got) || got != sizeof(now)) {
            continue;
        }
        if (std::memcmp(now, c.r0, sizeof(c.r0)) == 0 &&
            std::memcmp(now + 8, c.r2, sizeof(c.r2)) == 0) {
            continue;
        }
        if (!IsBasis(now)) continue;

        const float dFwd = Dot(c.r2, now + 8);
        const float dRight = Dot(c.r0, now);
        if (dFwd < -1.0f || dFwd > 1.0f || dRight < -1.0f || dRight > 1.0f) continue;
        const float aFwd = std::acos(dFwd) * kRadToDeg;
        const float aRight = std::acos(dRight) * kRadToDeg;
        if (std::fabs(aFwd - wantDeg) > 4.0f) continue;
        if (std::fabs(aRight - wantDeg) > 4.0f) continue;

        ++turned;
        const float dx = now[12] - cam[12];
        const float dy = now[13] - cam[13];
        const float dz = now[14] - cam[14];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const bool verbatim = std::memcmp(c.r0, cam, sizeof(c.r0)) == 0 &&
                              std::memcmp(c.r2, cam + 8, sizeof(c.r2)) == 0;
        const bool onStack = LooksLikeThreadStack(c.address);
        if (logged < kMaxLogged) {
            HT_LOG("  turns with head: 0x%016llX  %.3fm from the eye "
                   "[%+.3f %+.3f %+.3f]  fwd %.0fdeg right %.0fdeg%s%s",
                   (unsigned long long)c.address, dist, dx, dy, dz, aFwd, aRight,
                   verbatim ? "  (verbatim camera basis)" : "",
                   onStack ? "  (stack temporary)" : "");
            ++logged;
        }
        // The eye's own copies say nothing - we already write those. What is
        // wanted is a node carrying its OWN orientation a short reach away,
        // which is where a held tool sits.
        if (!verbatim && !onStack && dist > 0.02f && dist < 2.5f) {
            const float score = std::fabs(dist - 0.5f);
            if (score < bestScore) { bestScore = score; best = c.address; }
        }
    }

    UnfreezeAppliedRows();
    HT_LOG("WeaponProbe: %zu nodes turned by %.0f degrees with the head. "
           "Head tracking resumed.", turned, wantDeg);

    if (best != 0) {
        HT_LOG("WeaponProbe: watching 0x%016llX for writes - the instruction "
               "that writes it is the one to sandwich.",
               (unsigned long long)best);
        StartWriteWatchAt(best, "head-parented node");
    } else {
        HT_LOG("WeaponProbe: no non-camera node turned with the head. The "
               "viewmodel is not placed through a world transform near the eye.");
    }
}

}  // namespace

void StartWeaponProbe() {
    std::thread(ProbeThread).detach();
}

} // namespace NMSHT
