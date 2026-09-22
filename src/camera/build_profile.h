#pragma once

#include <cstdint>
#include <initializer_list>

#include <cameraunlock/memory/pe_fingerprint.h>

namespace NMSHT {

// One shipped NMS build: a PE fingerprint plus the RVAs this mod pins to it.
// Append-only registry (see doctrine "Maintain compatibility across new
// patches"): when a patch breaks RVAs, ADD a new profile, never edit an
// existing one - users on the old build keep matching their old profile.
struct BuildProfile {
    const char* name;                          // "steam-win64-YYYYMMDD"
    cameraunlock::memory::PeFingerprint fingerprint;

    // Slots in cGcCameraManager's vtable, per build. They are class-layout
    // properties rather than code addresses, so they survive ordinary code
    // motion - but not a re-declaration of the class, and the September 2026
    // builds are exactly that: cTkCamera's five-row transform moved to the
    // start of the object, the object grew from 0xC0 to 0x100 bytes, the
    // selector byte moved from manager+0x2B0 to +0x330, and a copy accessor was
    // inserted, pushing the one this mod hooks from slot 10 to slot 11.
    //
    // `copyTransform` copies the active camera's five rows into the caller's
    // buffer. `activeTransform` hands back a pointer INTO the live rows, which
    // is the same data one level up, and is the accessor whose callers get the
    // clean mirror.
    int copyTransformVfunc;
    int activeTransformVfunc;

    // RVA of cGcCameraManager vtable[copyTransformVfunc]. The address is
    // RESOLVED from RTTI at runtime; this copy exists so a known build can
    // cross-check the resolution and refuse to hook if the class layout moved.
    std::uint32_t cameraTransformRva;

    // RVA of the instruction immediately after the engine commits the frame's
    // camera into the live transform. The head rotation is injected there: the
    // renderer samples the camera later in the frame, so this is the one moment
    // that is both after the engine's own write and before anything draws.
    // Found with a write watch on the live transform, which reported exactly one
    // writing instruction. Zero means the build has no pinned site and the mod
    // stays dormant rather than injecting at a guessed one.
    std::uint32_t cameraCommitRva;

    // Legacy whole-accessor substitution. Zero disables it. On the September
    // layout this accessor also returns the camera object to projection and
    // culling callers, which must keep the live object and tracked transform.
    // Use the caller lists below to decouple aim on that layout.
    std::uint32_t accessorMirrorBytes;

    // Where the camera's field-of-view float sits, measured from the pointer
    // the activeTransform accessor returns. Per build because the re-declared
    // class moved BOTH terms: June holds the transform at object+0x10 and the
    // FOV at object+0xB0, so +0xA0 apart; September and GDK hold the transform
    // at the object start and the FOV at object+0xF0, so +0xF0 apart. Reading
    // the September builds at June's +0xA0 lands on a neighbouring float and
    // reports a field of view around one degree.
    //
    // The engine's own projection helper is what settles it: it multiplies
    // `fovScale` by this field, which is how find_fov_helper.py reads the
    // displacement straight out of the `mulss`. Zero leaves the FOV unreported,
    // as does a zero fovScaleRva.
    std::uint32_t cameraFovFromTransform;

    // Where the rows the RENDERER projects from sit, measured from the pointer
    // the activeTransform accessor returns. Zero means they are the same block.
    //
    // The September re-declaration split the two. cTkCamera now holds three row
    // blocks and the camera commit fills them in this order: object+0xA0 gets
    // the previous frame archived, object+0x50 gets this frame unconditionally,
    // and object+0x00 gets this frame only when `byte [rax+0x4FF]` is zero. The
    // accessor hands back object+0x00, so a mod that rotates what the accessor
    // returns rotates the GATED copy - which the frustum reads and the renderer
    // does not. The symptom is unmistakable once seen: looking around moves what
    // is culled and never moves the picture.
    //
    // The engine's own projection is the proof and is not open to argument:
    // `call [rax+0x70]` for the object, `mulss xmm6, [rax+0xF0]` for the FOV,
    // then `lea r8, [rax+0x50]` for the rows it hands world-to-screen. June does
    // the same thing with +0xB0 and +0x10, and +0x10 is what its accessor
    // returns, which is why June needed none of this.
    std::uint32_t renderRowsFromTransform;

    // Non-zero decouples aim through the camera's OWN layout instead of through
    // the caller lists below, by writing the tracked rows to the render block
    // and the engine's clean rows to the accessor block on the same commit.
    //
    // It works because those two blocks already have different readers: the
    // engine's projection takes `lea r8,[rax+0x50]`, while everything that asks
    // the manager for "the camera" gets the object base. Serving each what it
    // needs costs no substitution, no snapshot and no per-caller knowledge -
    // which is what makes it the answer for the Game Pass binary, where the
    // whole-accessor mirror breaks the frame and no caller list is derived.
    //
    // The accessor block gets the CLEAN rows rather than being left alone,
    // because the engine writes it behind a gate (`byte [rax+0x4FF]`) and skips
    // it on some frames. Left alone it would go stale and gameplay would aim
    // through an older camera; written every commit it is always this frame's,
    // just without the head rotation.
    //
    // The cost is that anything reading the accessor block for a RENDERING
    // decision - culling is the known one - works from the un-turned camera, so
    // geometry can pop at the edges of a hard head turn.
    std::uint32_t decoupleByRowBlock;

    // Which register holds the camera being committed, at cameraCommitRva.
    // Zero means do not filter, which is what every Steam profile wants.
    //
    // It matters because the two binaries reach the commit differently. Steam
    // inlines it into cGcCameraBehaviourFirstPerson, so the breakpoint only
    // ever fires for the camera this mod cares about. Game Pass calls ONE
    // shared routine from every camera behaviour, so the breakpoint fires for
    // the fly camera, the cockpit transition and the building-mode camera too -
    // and each of those made this mod capture a clean basis from the LIVE
    // camera, which had not just been written. The frames that followed were
    // refused for still holding our own last write, so the pose reached the
    // renderer in bursts and the reticle stepped rather than moved.
    //
    // With a register named here the injection runs only when the camera that
    // just committed IS the live one, which is the behaviour Steam gets for
    // free from where its commit sits.
    enum class CommitCameraReg : std::uint32_t { None = 0, Rcx = 1, Rsi = 2 };
    CommitCameraReg commitCameraReg;

    // RVA of the cGcCameraManager accessor that returns the PRIMARY camera's
    // transform without consulting the selector byte - manager+0x140 on the
    // June builds, manager+0x130 on the September ones. First-person weapon
    // placement reads it, which is how the multitool ends up following the head
    // unless the rows it is handed are the clean ones.
    std::uint32_t weaponCameraRva;

    // RVA of cTkGraphicsManager's world-to-screen projection, the function the
    // engine's own helper at 0x0065CCD0 calls as
    // Project(mgr, rows, fov, points, out, count, flags, screenSize).
    // Every world-anchored HUD marker goes through it; the crosshair does not.
    // Diagnostics only - it is what proved the HUD already projects through the
    // head-tracked camera and that the reticle is not projected at all.
    std::uint32_t worldToScreenRva;

    // RVA of the pointer to the game's global block. Every gate below is an
    // offset from the block it points at, and all of them are measured rather
    // than guessed: they are where cGcGenericSectionConditionInMultiplayer and
    // cGcGenericSectionConditionInventoryOpen were observed to read from, those
    // being the game's own answers to "are other players here" and "is a menu
    // up".
    std::uint32_t gameGlobalsPtrRva;

    // Half-open range of network player slots inside that block, and the byte
    // in each slot whose value above 1 means a connected player. Measured from
    // the same condition, and confirmed in solo play where every slot is empty.
    std::uint32_t netPlayerSlotsBegin;
    std::uint32_t netPlayerSlotsEnd;
    std::uint32_t netPlayerConnectedByte;

    // Int in that block holding which in-game menu page is up. Any value other
    // than 0 or 2 means a page is open, which the in-game PDA confirmed: it
    // walked the value 0 -> 3 -> 4 -> 5 -> 0 across an open and close.
    std::uint32_t menuPageModeOffset;

    // Byte in that block that is non-zero while the multi-tool's weapon zoom is
    // up - the game's own aim-down-sights state. Zero leaves the ADS cycle
    // reporting "not aiming" on every frame, which is stock behaviour: failing
    // toward the unmodded game is the safe direction for a build whose aim
    // state has not been located.
    std::uint32_t weaponZoomOffset;

    // RVA of cTkNGui's by-name element lookup, and the return address of the
    // one call site in the HUD that passes it the RETICLE layer. The crosshair
    // element is never stored anywhere reachable - the HUD looks it up afresh
    // every frame, because the name changes with the equipped weapon - so the
    // lookup is hooked and that one caller claimed by its return address.
    //
    // The site is `mov rcx, [rsi + 0xe9db0]` (the RETICLE layer, loaded out of
    // UI\HUD\HUDCROSSHAIR.MXML) followed by the call; the return address is the
    // instruction that stores the result. Either being zero leaves NMS's own
    // centred crosshair alone.
    std::uint32_t nguiFindElementRva;
    std::uint32_t reticleLookupReturnRva;

    // RVA of the global pointer to cTkGraphicsManager. The engine's own
    // projection reads the frame's render size from it - width at +0x34 and
    // height at +0x38, both ints - and divides one by the other to widen the
    // vertical field of view into the horizontal one. That ratio is the scale
    // on every horizontal correction this mod makes, so it is read from the
    // engine rather than measured off the game window, which is a different
    // rectangle whenever the window has a caption or the game upscales.
    //
    // Found in cGcCameraManager's own world-to-screen helper at 0x0065CCD0,
    // which passes this global straight to the projection. Zero leaves the
    // aspect unavailable and the crosshair where the engine draws it.
    std::uint32_t gfxManagerPtrRva;

    // RVA of the engine's global camera FOV scale, the float the projection
    // helpers multiply the camera's own FOV field by before handing it to
    // cTkGraphicsManager's world-to-screen. Zero leaves the FOV unreported.
    std::uint32_t fovScaleRva;

    // RVA of a .data copy of the camera basis that the differential scan
    // proved turns with the head. [Debug] CleanGlobalCycle writes the clean
    // basis through it, so it is pinned per build like everything else this mod
    // writes to. Zero leaves that probe off.
    std::uint32_t cameraGlobalRva;

    // RVA of the cGcApplication FSM slot holding the current application state
    // object. Resolved by scan when zero, which is what a build the mod has
    // never seen falls back to; pinning it removes the dependency on the scan
    // finding exactly one moving candidate.
    std::uint32_t appStateSlotRva;

    // Return addresses that must get the TRACKED camera while the rest of the
    // build keeps the clean one. The inverse of the two lists below, and only
    // meaningful with decoupleByRowBlock, where the accessor's block is kept
    // clean so that gameplay aims straight.
    //
    // Culling is what this exists for. It asks the manager for the camera like
    // everything else, so on a row-block build it gets the clean block and
    // decides what is on screen from a camera that is not turning with the
    // player's head - scenery outside the un-turned view is culled and the edges
    // of a hard head turn go empty.
    //
    // Found by putting a read watch on the clean block in gameplay rather than
    // by guessing from the accessor's callers: the watch names the instructions
    // that actually read it, and the one that matters reads the rows AND the
    // field of view at +0xF0 and then loops over an object list, which is a
    // frustum test and nothing else.
    //
    // Serving it the tracked rows is additive and safe to get wrong: the worst
    // case is that one consumer sees the camera the frame is drawn from, which
    // is what every consumer sees on a build that does not split the blocks.
    std::initializer_list<std::uint32_t> trackedTransformCallers;

    // Return addresses of gameplay readers that only consume the five-row
    // transform. Whole-camera and culling readers must not enter either list.
    std::initializer_list<std::uint32_t> aimCopyCallers;
    std::initializer_list<std::uint32_t> aimTransformCallers;
    // NGui render entry and the caller that draws HUDCROSSHAIR.
    std::uint32_t nguiRenderRva = 0;
    std::uint32_t reticleRenderReturnRva = 0;
    std::uint32_t playerFromGlobals = 0;
    std::uint32_t playerShipRva = 0;
    // The instruction just after the main thread reads the render rows to set
    // up the frame. The reticle is placed from the commit those rows came from,
    // which is only the newest commit when the camera is committed on the same
    // thread at a fixed point in the frame. Zero: the newest commit is used.
    std::uint32_t sceneSampleRva = 0;

    // Second commit site, for the third-person cameras. The Steam image inlines
    // the row write into each behaviour, so cameraCommitRva above is the
    // FIRST-PERSON behaviour's copy and fires only while you are looking
    // through your own eyes. cGcCameraBehaviourThirdPerson, PlayerThirdPerson
    // and SpacewalkThirdPerson share one slot-4 function, so one more pin
    // covers all three. Zero where it is not needed or not known: the GDK build
    // routes every behaviour through one shared writer, which cameraCommitRva
    // already pins, and an unpinned Steam build keeps the first-person-only
    // behaviour it had.
    std::uint32_t cameraCommitThirdPersonRva = 0;
};

// Selects the profile matching the running EXE, or nullptr if none match.
// On no match, *outRunning / *outPrimary are filled so the caller can log
// which direction the running build differs from the newest known one.
const BuildProfile* SelectProfile(void* moduleBase,
                                   cameraunlock::memory::PeFingerprint* outRunning,
                                   const BuildProfile** outPrimary);

}  // namespace NMSHT
