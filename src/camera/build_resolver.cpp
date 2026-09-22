#include "pch.h"
#include "build_resolver.h"

#include "core/debug_log.h"

#include <windows.h>

#include <cstring>

#include <cameraunlock/memory/pe_fingerprint.h>
#include <cameraunlock/memory/rtti_vtable.h>

namespace NMSHT {

namespace {

using cameraunlock::memory::FindVtableFromRTTI;
using cameraunlock::memory::VtableInfo;

struct Section {
    const std::uint8_t* data = nullptr;
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    bool valid() const { return data != nullptr && size != 0; }
};

struct Image {
    const std::uint8_t* base = nullptr;
    Section text;
    Section pdata;

    std::uint32_t RvaOf(const std::uint8_t* p) const {
        return static_cast<std::uint32_t>(p - base);
    }
    template <typename T>
    T Read(std::uint32_t rva) const {
        T v{};
        std::memcpy(&v, base + rva, sizeof(T));
        return v;
    }
};

bool MapImage(void* moduleBase, Image& img) {
    const auto* const base = static_cast<const std::uint8_t*>(moduleBase);
    const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* const nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    img.base = base;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        std::memcpy(name, sec->Name, 8);
        Section s{base + sec->VirtualAddress, sec->VirtualAddress,
                  sec->Misc.VirtualSize};
        if (std::strcmp(name, ".text") == 0) img.text = s;
        else if (std::strcmp(name, ".pdata") == 0) img.pdata = s;
    }
    return img.text.valid() && img.pdata.valid();
}

// Every offset in a `Find` is an RVA, and kNotFound is deliberately a value no
// real RVA takes: zero is a legitimate answer for several profile fields, so a
// resolver that signalled failure with 0 would be indistinguishable from one
// that correctly found nothing to pin.
constexpr std::uint32_t kNotFound = 0xFFFFFFFFu;

std::uint32_t FindBytes(const Section& sec, const void* pat,
                        std::size_t len, std::uint32_t fromRva) {
    if (sec.size < len) return kNotFound;
    const std::uint32_t start = fromRva > sec.rva ? fromRva - sec.rva : 0;
    for (std::uint32_t i = start; i + len <= sec.size; ++i) {
        if (std::memcmp(sec.data + i, pat, len) == 0) return sec.rva + i;
    }
    return kNotFound;
}

// ---------------------------------------------------------------------------
// .pdata: the function an address belongs to.
//
// The camera commit sits in a chunked function whose .pdata entry starts mid
// function, so its low address is not an entry point and nothing references it.
// UNWIND_INFO's chain flag leads back to the primary entry, which is the form
// that can be compared against a vtable slot.
// ---------------------------------------------------------------------------

constexpr std::uint8_t kUnwFlagChainInfo = 0x4;

struct RuntimeFunction {
    std::uint32_t begin, end, unwind;
};

bool EntryFor(const Image& img, std::uint32_t rva, RuntimeFunction& out) {
    const std::uint32_t count = img.pdata.size / sizeof(RuntimeFunction);
    std::uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        RuntimeFunction f{};
        std::memcpy(&f, img.pdata.data + mid * sizeof(RuntimeFunction), sizeof(f));
        if (rva < f.begin) hi = mid;
        else if (rva >= f.end) lo = mid + 1;
        else { out = f; return true; }
    }
    return false;
}

// Follows UNWIND_INFO chaining to the function's real entry point.
std::uint32_t PrimaryEntry(const Image& img, std::uint32_t rva) {
    RuntimeFunction f{};
    if (!EntryFor(img, rva, f)) return kNotFound;
    for (int hop = 0; hop < 8; ++hop) {
        const std::uint8_t verFlags = img.Read<std::uint8_t>(f.unwind);
        if ((verFlags >> 3 & kUnwFlagChainInfo) == 0) return f.begin;
        const std::uint8_t codes = img.Read<std::uint8_t>(f.unwind + 2);
        const std::uint32_t off = f.unwind + 4 + ((codes + 1) & ~1u) * 2u;
        RuntimeFunction next{};
        std::memcpy(&next, img.base + off, sizeof(next));
        if (next.begin == f.begin) return f.begin;
        f = next;
    }
    return f.begin;
}

// ---------------------------------------------------------------------------
// Resolvers. Each returns false on any self-check failure.
// ---------------------------------------------------------------------------

// cGcCameraManager's vtable gives three accessors. Slot 9 hands back a pointer
// into the live camera and is slot 9 on every build seen so far, June included,
// which is why it is the anchor rather than something to search for.
//
// The copy accessor is the slot that CALLS slot 9 - `call qword ptr [rax+0x48]`
// - which is what distinguishes it from the second copy accessor September
// inserted ahead of it, and is why its slot number moved from 10 to 11 without
// anything else about it changing. The weapon accessor is the bare
// `lea rax,[rcx+imm32]; ret` that skips the selector byte entirely.
bool ResolveCameraManager(void* moduleBase, BuildProfile& p) {
    VtableInfo mgr{};
    if (!FindVtableFromRTTI(moduleBase, "cGcCameraManager", mgr, 20)) {
        HT_LOG("Resolver: no cGcCameraManager RTTI vtable.");
        return false;
    }
    if (mgr.vfunc_count <= 13) {
        HT_LOG("Resolver: cGcCameraManager vtable has only %d entries.",
               mgr.vfunc_count);
        return false;
    }

    p.activeTransformVfunc = 9;

    const std::uint8_t kCallSlot9[] = {0xFF, 0x50, 0x48};   // call [rax+0x48]
    const std::uint8_t kLeaRcx[] = {0x48, 0x8D, 0x81};      // lea rax,[rcx+imm32]

    int copySlot = -1, weaponSlot = -1;
    for (int slot = 10; slot < mgr.vfunc_count && slot < 20; ++slot) {
        const auto* const fn =
            reinterpret_cast<const std::uint8_t*>(mgr.vfuncs[slot]);
        if (fn == nullptr) continue;
        for (int i = 0; i < 0x80; ++i) {
            if (copySlot < 0 &&
                std::memcmp(fn + i, kCallSlot9, sizeof(kCallSlot9)) == 0) {
                copySlot = slot;
                break;
            }
        }
        if (weaponSlot < 0 &&
            std::memcmp(fn, kLeaRcx, sizeof(kLeaRcx)) == 0 && fn[7] == 0xC3) {
            weaponSlot = slot;
        }
    }
    if (copySlot < 0) {
        HT_LOG("Resolver: no cGcCameraManager slot calls the live-rows accessor.");
        return false;
    }

    p.copyTransformVfunc = copySlot;
    p.cameraTransformRva =
        static_cast<std::uint32_t>(mgr.vfuncs[copySlot] -
                                   reinterpret_cast<std::uintptr_t>(moduleBase));
    p.weaponCameraRva =
        weaponSlot < 0 ? 0u
                       : static_cast<std::uint32_t>(
                             mgr.vfuncs[weaponSlot] -
                             reinterpret_cast<std::uintptr_t>(moduleBase));
    HT_LOG("Resolver: cGcCameraManager copy accessor at slot %d (RVA 0x%08X), "
           "weapon accessor slot %d.", copySlot, p.cameraTransformRva, weaponSlot);
    return true;
}

// The engine's own camera world-to-screen helper, which is where four pins come
// from at once. Its shape:
//
//     movss xmm6, [rip + X]       ; the global camera FOV scale
//     mulss xmm6, [rax + D1]      ; that camera's own FOV field
//     ...
//     lea   r8,   [rax + D2]      ; the rows it hands the projection
//
// D1 - D2 has been 0xA0 on every build: June is 0xB0/0x10, every September
// build and the Game Pass one is 0xF0/0x50. That difference is the self-check -
// it is what says the two displacements came from the same camera object rather
// than from an unrelated pair of instructions that happen to match.
bool ResolveFovHelper(const Image& img, BuildProfile& p) {
    const std::uint8_t kMovssRip[] = {0xF3, 0x0F, 0x10, 0x35};  // movss xmm6,[rip+d]
    const std::uint8_t kMulssRax[] = {0xF3, 0x0F, 0x59, 0xB0};  // mulss xmm6,[rax+d32]
    const std::uint8_t kLeaR8[] = {0x4C, 0x8D, 0x80};           // lea r8,[rax+d32]
    const std::uint8_t kLeaR8Short[] = {0x4C, 0x8D, 0x40};      // lea r8,[rax+d8]

    std::uint32_t at = img.text.rva;
    while (true) {
        at = FindBytes(img.text, kMovssRip, sizeof(kMovssRip), at);
        if (at == kNotFound) break;
        const std::uint32_t movss = at;
        at = movss + 1;

        // The mulss does not follow the movss immediately - the helper loads
        // the camera object in between - so this is a window, not an offset.
        std::uint32_t mulss = kNotFound;
        for (std::uint32_t i = movss + 8; i < movss + 0x30; ++i) {
            if (std::memcmp(img.base + i, kMulssRax, sizeof(kMulssRax)) == 0) {
                mulss = i;
                break;
            }
        }
        if (mulss == kNotFound) continue;
        const std::uint32_t afterMovss = mulss;
        const std::uint32_t d1 = img.Read<std::uint32_t>(mulss + 4);

        // The lea follows within a short window; accept either encoding.
        std::uint32_t d2 = kNotFound;
        for (std::uint32_t i = afterMovss + 8; i < afterMovss + 0x50; ++i) {
            if (std::memcmp(img.base + i, kLeaR8, sizeof(kLeaR8)) == 0) {
                d2 = img.Read<std::uint32_t>(i + 3);
                break;
            }
            if (std::memcmp(img.base + i, kLeaR8Short, sizeof(kLeaR8Short)) == 0) {
                d2 = img.Read<std::uint8_t>(i + 3);
                break;
            }
        }
        if (d2 == kNotFound || d1 < d2 || (d1 - d2) != 0xA0u) continue;

        const std::int32_t rel = img.Read<std::int32_t>(movss + 4);
        p.fovScaleRva = static_cast<std::uint32_t>(movss + 8 + rel);
        p.cameraFovFromTransform = d1;
        p.renderRowsFromTransform = d2;

        // Both globals this helper touches sit just above it as RIP loads.
        // The graphics manager is the one it passes to the projection; the game
        // globals block is the other. Taking the two most recent distinct RIP
        // targets before the FOV read is what the offline scan does.
        // Which registers the helper loads them into differs between the
        // Steam and Game Pass compilers, so this matches any `mov r64,[rip+d]`
        // rather than the two registers the Steam build happens to use: the
        // modrm's r/m field is 5 with mod 0, whatever the destination.
        std::uint32_t seenTargets[2] = {0, 0};
        int seenCount = 0;
        for (std::uint32_t i = movss > 0x80 ? movss - 0x80 : 0; i < movss; ++i) {
            if (img.Read<std::uint8_t>(i) != 0x48) continue;
            if (img.Read<std::uint8_t>(i + 1) != 0x8B) continue;
            if ((img.Read<std::uint8_t>(i + 2) & 0xC7) != 0x05) continue;
            const std::uint32_t target = static_cast<std::uint32_t>(
                i + 7 + img.Read<std::int32_t>(i + 3));
            bool already = false;
            for (int k = 0; k < seenCount; ++k) already |= seenTargets[k] == target;
            if (already) continue;
            if (seenCount < 2) seenTargets[seenCount++] = target;
        }
        // Order, not register: the globals block is loaded first and the
        // graphics manager second, which is what the Steam images show and what
        // the verified Steam profiles carry. Both are optional - neither is
        // needed to move the camera - so a build that does not match this shape
        // leaves them zero rather than refusing to run.
        if (seenCount == 2) {
            p.gameGlobalsPtrRva = seenTargets[0];
            p.gfxManagerPtrRva = seenTargets[1];
        }
        HT_LOG("Resolver: %d RIP-relative globals beside the projection helper "
               "(globals 0x%08X, graphics manager 0x%08X).",
               seenCount, p.gameGlobalsPtrRva, p.gfxManagerPtrRva);

        HT_LOG("Resolver: camera projection helper at RVA 0x%08X - FOV at "
               "+0x%X, render rows at +0x%X, fovScale RVA 0x%08X.",
               movss, p.cameraFovFromTransform, p.renderRowsFromTransform,
               p.fovScaleRva);
        return true;
    }
    HT_LOG("Resolver: no camera projection helper matched the FOV read shape.");
    return false;
}

// The camera commit, which is the one address this mod writes through.
//
// The engine fills three row blocks and gates the last on `byte [rax+0x4FF]`;
// the jump that skips it lands on the instruction after the whole sequence,
// which is reached whether or not the gated write happened. Fifteen camera
// behaviours share that shape, so the gate alone does not identify it. The one
// that matters is the FIRST-PERSON behaviour's, and the way to say so across
// builds is to resolve cGcCameraBehaviourFirstPerson from RTTI, take its slot
// 4, and keep the gate whose .pdata primary entry is that function.
// The gate inside `behaviour`'s slot 4, or kNotFound. Same rule the
// first-person search below uses, factored out because the third-person
// cameras need their own site on a build that inlines the write per behaviour.
std::uint32_t GateInsideBehaviour(const Image& img, void* moduleBase,
                                  const char* behaviour) {
    VtableInfo vt{};
    if (!FindVtableFromRTTI(moduleBase, behaviour, vt, 8) || vt.vfunc_count <= 4) {
        return kNotFound;
    }
    const auto moduleAddr = reinterpret_cast<std::uintptr_t>(moduleBase);
    const std::uint32_t wanted =
        static_cast<std::uint32_t>(vt.vfuncs[4] - moduleAddr);

    // cmp byte ptr [rax+0x4FF], 0
    const std::uint8_t kGate[] = {0x80, 0xB8, 0xFF, 0x04, 0x00, 0x00, 0x00};
    std::uint32_t at = img.text.rva;
    while (true) {
        at = FindBytes(img.text, kGate, sizeof(kGate), at);
        if (at == kNotFound) break;
        const std::uint32_t gate = at;
        at = gate + 1;
        const std::uint32_t jne = gate + sizeof(kGate);
        if (img.Read<std::uint8_t>(jne) != 0x75) continue;  // jne rel8
        const std::int8_t rel = img.Read<std::int8_t>(jne + 1);
        const std::uint32_t target = static_cast<std::uint32_t>(jne + 2 + rel);
        if (PrimaryEntry(img, target) != wanted) continue;
        return target;
    }
    return kNotFound;
}

// The third-person cameras, on a build that inlines the commit into each
// behaviour. cGcCameraBehaviourThirdPerson, PlayerThirdPerson and
// SpacewalkThirdPerson share one slot-4 function on the Steam image, so the
// first name that resolves carries all three. Absent on a build whose
// behaviours call one shared writer: there cameraCommitRva already covers every
// camera, and this stays zero.
void ResolveThirdPersonCommitSite(const Image& img, void* moduleBase, BuildProfile& p) {
    for (const char* behaviour : {"cGcCameraBehaviourThirdPerson",
                                  "cGcCameraBehaviourPlayerThirdPerson",
                                  "cGcCameraBehaviourSpacewalkThirdPerson"}) {
        const std::uint32_t target = GateInsideBehaviour(img, moduleBase, behaviour);
        if (target == kNotFound || target == p.cameraCommitRva) continue;
        p.cameraCommitThirdPersonRva = target;
        HT_LOG("Resolver: third-person camera commit at RVA 0x%08X, inside %s "
               "slot 4.", target, behaviour);
        return;
    }
    HT_LOG("Resolver: no third-person commit gate on this build, so the view "
           "follows your head in first person only.");
}

bool ResolveCommitSite(const Image& img, void* moduleBase, BuildProfile& p) {
    VtableInfo fp{};
    if (!FindVtableFromRTTI(moduleBase, "cGcCameraBehaviourFirstPerson", fp, 8)) {
        HT_LOG("Resolver: no cGcCameraBehaviourFirstPerson RTTI vtable.");
        return false;
    }
    if (fp.vfunc_count <= 4) {
        HT_LOG("Resolver: cGcCameraBehaviourFirstPerson vtable too short (%d).",
               fp.vfunc_count);
        return false;
    }
    const auto moduleAddr = reinterpret_cast<std::uintptr_t>(moduleBase);
    const std::uint32_t wanted =
        static_cast<std::uint32_t>(fp.vfuncs[4] - moduleAddr);

    // cmp byte ptr [rax+0x4FF], 0
    const std::uint8_t kGate[] = {0x80, 0xB8, 0xFF, 0x04, 0x00, 0x00, 0x00};

    std::uint32_t at = img.text.rva;
    int seen = 0;
    // The Game Pass binary inlines far less than the Steam one: instead of the
    // commit being folded into every camera behaviour, it is ONE shared routine
    // the behaviours call, so no gate belongs to a behaviour's slot 4 and the
    // rule above finds nothing. That routine is recognisable on its own terms -
    // its gated block is the last thing it does, so the jump that skips the
    // block lands on the function's own `ret` rather than on more code.
    //
    // Measured across the images this mod has: exactly one gate per binary has
    // a `ret` at its target, and on the Game Pass build that gate resolves to
    // 0x00808770, which is the address that profile already pins by hand. It is
    // the fallback and never the first answer, because Steam images contain a
    // ret-target gate too and it is NOT the Steam commit - there the
    // first-person behaviour always matches first.
    std::uint32_t retTarget = kNotFound;
    int retTargets = 0;

    while (true) {
        at = FindBytes(img.text, kGate, sizeof(kGate), at);
        if (at == kNotFound) break;
        const std::uint32_t gate = at;
        at = gate + 1;

        const std::uint32_t jne = gate + sizeof(kGate);
        if (img.Read<std::uint8_t>(jne) != 0x75) continue;  // jne rel8
        ++seen;
        const std::int8_t rel = img.Read<std::int8_t>(jne + 1);
        const std::uint32_t target = static_cast<std::uint32_t>(jne + 2 + rel);

        if (img.Read<std::uint8_t>(target) == 0xC3) {  // ret
            ++retTargets;
            retTarget = target;
        }

        if (PrimaryEntry(img, target) != wanted) continue;

        p.cameraCommitRva = target;
        HT_LOG("Resolver: camera commit at RVA 0x%08X, inside "
               "cGcCameraBehaviourFirstPerson slot 4 (RVA 0x%08X); %d gates "
               "carried that shape and this is the one that belongs to it.",
               target, wanted, seen);
        return true;
    }

    if (retTargets == 1) {
        p.cameraCommitRva = retTarget;
        HT_LOG("Resolver: camera commit at RVA 0x%08X. None of the %d gates "
               "belong to a camera behaviour on this build, which is the shape "
               "the Game Pass binary has - one shared commit routine rather "
               "than one inlined into each behaviour - and exactly one gate "
               "skips to its own ret.", retTarget, seen);
        return true;
    }

    HT_LOG("Resolver: none of the %d camera-commit gates belong to "
           "cGcCameraBehaviourFirstPerson slot 4, and %d skip to a ret, so the "
           "shared-routine rule cannot pick one either.", seen, retTargets);
    return false;
}

// The game's own answer to "is a menu up", which is where the menu gate and a
// second, independent reading of the globals pointer come from:
//
//     mov rcx, [rip + X]      ; the globals block
//     add rcx, BASE
//     mov eax, [rcx + FIELD]  ; the page currently up
//
// menuPageModeOffset is BASE + FIELD. The globals pointer it loads must agree
// with the one the projection helper produced, and that agreement is the check.
bool ResolveMenuPage(void* moduleBase, BuildProfile& p) {
    VtableInfo cond{};
    if (!FindVtableFromRTTI(moduleBase, "cGcGenericSectionConditionInventoryOpen",
                            cond, 4)) {
        HT_LOG("Resolver: no inventory-open condition RTTI vtable.");
        return false;
    }
    if (cond.vfunc_count <= 1) return false;

    const auto* const fn = reinterpret_cast<const std::uint8_t*>(cond.vfuncs[1]);
    const std::uint32_t fnRva = static_cast<std::uint32_t>(
        cond.vfuncs[1] - reinterpret_cast<std::uintptr_t>(moduleBase));

    for (std::uint32_t i = 0; i < 0x40; ++i) {
        // mov rcx, [rip+d]
        if (std::memcmp(fn + i, "\x48\x8B\x0D", 3) != 0) continue;
        std::int32_t rel = 0;
        std::memcpy(&rel, fn + i + 3, 4);
        const std::uint32_t globals = fnRva + i + 7 + rel;

        // add rcx, imm32
        if (std::memcmp(fn + i + 7, "\x48\x81\xC1", 3) != 0) continue;
        std::uint32_t blockBase = 0;
        std::memcpy(&blockBase, fn + i + 10, 4);

        // mov eax, [rcx+disp32]
        if (std::memcmp(fn + i + 14, "\x8B\x81", 2) != 0) continue;
        std::uint32_t field = 0;
        std::memcpy(&field, fn + i + 16, 4);

        if (p.gameGlobalsPtrRva != 0 && globals != p.gameGlobalsPtrRva) {
            HT_LOG("Resolver: the menu condition reads globals at RVA 0x%08X "
                   "but the projection helper found 0x%08X. Not pinning either.",
                   globals, p.gameGlobalsPtrRva);
            return false;
        }
        p.gameGlobalsPtrRva = globals;
        p.menuPageModeOffset = blockBase + field;
        HT_LOG("Resolver: menu page at globals+0x%X+0x%X = 0x%08X, globals "
               "pointer RVA 0x%08X confirmed by two independent reads.",
               blockBase, field, p.menuPageModeOffset, globals);
        return true;
    }
    HT_LOG("Resolver: the inventory-open condition no longer reads the menu "
           "page through a globals base and field.");
    return false;
}

}  // namespace

const BuildProfile* ResolveProfileFromImage(void* moduleBase, BuildProfile& out) {
    Image img{};
    if (!MapImage(moduleBase, img)) {
        HT_LOG("Resolver: the running image has no .text/.pdata to read.");
        return nullptr;
    }

    out = BuildProfile{};
    out.name = "resolved-at-runtime";
    cameraunlock::memory::ReadPeFingerprint(moduleBase, out.fingerprint);

    // These three are what head tracking cannot run without: where the camera
    // is, where its rows are, and where to write them.
    if (!ResolveCameraManager(moduleBase, out)) return nullptr;
    if (!ResolveFovHelper(img, out)) return nullptr;
    if (!ResolveCommitSite(img, moduleBase, out)) return nullptr;
    ResolveThirdPersonCommitSite(img, moduleBase, out);

    // The menu gate is not. Failing it costs the suppression that keeps the
    // head from swinging the view behind an open menu, which is worth saying
    // out loud and is not worth refusing to run over - and the globals pointer
    // it would confirm has already been read out of the projection helper.
    if (!ResolveMenuPage(moduleBase, out)) {
        out.menuPageModeOffset = 0;
        HT_LOG("Resolver: continuing without the menu gate. Head tracking will "
               "keep moving the view while an in-game menu is open; everything "
               "else is unaffected.");
    }
    if (out.gameGlobalsPtrRva == 0) {
        HT_LOG("Resolver: no globals pointer from either route, so the menu and "
               "multiplayer gates are inactive on this build. Gameplay is still "
               "detected - the application-state machine is found by scan when "
               "no slot is pinned, which is the path this build now takes.");
    }

    // The application-state slot sits a fixed 0x28 below the globals pointer,
    // which is the relationship every pinned September profile carries. It is
    // the one value here taken from a relationship rather than read directly,
    // so game_state re-validates it by scan and falls back if it does not walk
    // named states.
    // Only when there IS a globals pointer: 0 - 0x28 wraps to 0xFFFFFFD8, and
    // that is not a harmless bad value, it is a wild read the state machine
    // then treats as a pinned slot.
    out.appStateSlotRva =
        out.gameGlobalsPtrRva != 0 ? out.gameGlobalsPtrRva - 0x28u : 0u;

    // Left at zero on purpose, each because zero is a behaviour this mod
    // already ships somewhere rather than a broken state: no accessor
    // substitution (aim follows the view), no reticle correction, no ADS state,
    // no multiplayer gate, no debug camera-global probe.
    return &out;
}

}  // namespace NMSHT
