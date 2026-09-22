#include "pch.h"
#include "build_profile.h"

namespace NMSHT {

using cameraunlock::memory::PeFingerprint;
using cameraunlock::memory::ReadPeFingerprint;

// Steam Win64 build, TimeDateStamp 0x6A205F37 (~2026-06-05). Patch shifted
// SizeOfImage 0x07202000 -> 0x07288000; the view-matrix builder relocated
// from RVA 0x65bcf0 to 0x65c060 (prologue byte-identical, found by signature
// scan - see .lab/scripts/rva_reloc.py).
static const BuildProfile kSteamProfile_20260605 = {
    "steam-win64-20260605",
    { 0x6A205F37u, 0x07288000u, 0x00000000u },
    10,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0065c060u,   // cameraTransformRva
    0u,            // cameraCommitRva
    0x50u,         // accessorMirrorBytes
};

// Steam Win64 build, TimeDateStamp 0x6A19D791 (~2026-05-30).
// view-matrix builder discovered via Ghidra: FUN_14065bcf0, imageBase
// 0x140000000 -> RVA 0x0065bcf0.
static const BuildProfile kSteamProfile_20260530 = {
    "steam-win64-20260530",
    { 0x6A19D791u, 0x07202000u, 0x00000000u },
    10,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0065bcf0u,   // cameraTransformRva
    0u,            // cameraCommitRva
    0x50u,         // accessorMirrorBytes
};

// Steam Win64 build, TimeDateStamp 0x6A32CB34 (~2026-06-18).
//
// The renderer does NOT read the copy vtable[10] hands out, and it does not
// read it during the swapchain's acquire -> present window either. It reads the
// live transform straight from memory, later in the frame than acquire. So the
// injection point is the engine's own camera commit: RVA 0x00621DFF stores the
// first row, and 0x00621E2B is the next instruction after the whole five-row
// block lands. A write watch on the live transform found that one site and no
// other (2026-08-31).
static const BuildProfile kSteamProfile_20260618 = {
    "steam-win64-20260618",
    { 0x6A32CB34u, 0x0728D000u, 0x00000000u },
    10,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0065C0D0u,   // cameraTransformRva
    0x00621E2Bu,   // cameraCommitRva
    0x00000050u,   // accessorMirrorBytes
    0x000000A0u,   // cameraFovFromTransform
    0x00000000u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::None,  // commitCameraReg
    0x00233380u,   // weaponCameraRva
    0x02B33900u,   // worldToScreenRva
    0x06B44DD8u,   // gameGlobalsPtrRva
    0x00904628u,   // netPlayerSlotsBegin
    0x00904728u,   // netPlayerSlotsEnd
    0x00009290u,   // netPlayerConnectedByte
    0x008467A8u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x001C1E10u,   // nguiFindElementRva
    0x00905FD2u,   // reticleLookupReturnRva
    0x05637610u,   // gfxManagerPtrRva
    0x06B46DACu,   // fovScaleRva
    0x06B46BC0u,   // cameraGlobalRva
    0x06B44DB0u,   // appStateSlotRva
    {},            // trackedTransformCallers
};

// Steam Win64 build, TimeDateStamp 0x6AA2C421 (2026-09-10).
//
// cGcCameraManager was re-declared in this update, so the June values do not
// carry across as a block of shifted addresses - the class itself is different.
// What moved, read out of the vtable and the engine's own projection helper:
//
//   * cTkCamera's five-row transform sits at the START of the object now
//     (manager+0x130 / +0x230), where it used to be 0x10 bytes in.
//   * The object grew from 0xC0 to 0x100 bytes, so the second camera moved from
//     manager+0x1F0 to +0x230, and the selector byte from +0x2B0 to +0x330.
//   * Its field of view moved from object+0xB0 to object+0xF0. Both terms moved,
//     so the distance from the transform changed too - 0xA0 in June, 0xF0 here -
//     and that distance is what the FOV read uses (cameraFovFromTransform).
//   * A second selector accessor and a second copy accessor were inserted, so
//     the accessor that copies the LIVE rows - the one calling vtable[9], which
//     is what identifies it - sits at slot 11 rather than slot 10.
//
// Derived from the shipped EXE with .lab/scripts/find_fov_helper.py, which
// finds the engine's own world-to-screen helper by the shape of its FOV read
// rather than by any byte of the surrounding code.
//
// The commit site is the same shape as the June one and was found the same way:
// a write watch on the live transform in world named exactly one writing
// instruction, 0x006520F3, and 0x00652118 is the next instruction after the
// whole five-row block it starts. The engine writes the frame's rows TWICE
// here - unconditionally to camera+0x50, then to the live rows at camera+0x00
// behind a `cmp byte [rax+0x4FF]` gate - and 0x00652118 is reached whether or
// not that gate let the second write through. That is safe because
// OnRenderPhaseBegin already refuses any frame whose rows still hold this mod's
// own last write; it stands down rather than composing a second rotation onto
// an already rotated basis.
//
// worldToScreenRva is deliberately 0: the projection this build's camera helper
// calls is a viewport wrapper with a different signature, not the function the
// June profile pins, and the screen-projection probe is diagnostics only.
// netPlayerSlots* are 0 for the same kind of reason - the engine's multiplayer
// predicate no longer walks a fixed slot array, it asks an object through a
// virtual - so the "another explorer is here" gate is inactive on this build.
static const BuildProfile kSteamProfile_20260910 = {
    "steam-win64-20260910",
    { 0x6AA2C421u, 0x07612000u, 0x00000000u },
    11,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0068D870u,   // cameraTransformRva
    0x00652118u,   // cameraCommitRva
    0x00000050u,   // accessorMirrorBytes
    0x000000F0u,   // cameraFovFromTransform
    0x00000050u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::None,  // commitCameraReg
    0x00317A60u,   // weaponCameraRva
    0x00000000u,   // worldToScreenRva
    0x06E81828u,   // gameGlobalsPtrRva
    0x00000000u,   // netPlayerSlotsBegin
    0x00000000u,   // netPlayerSlotsEnd
    0x00000000u,   // netPlayerConnectedByte
    0x008653F8u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x00000000u,   // nguiFindElementRva
    0x00000000u,   // reticleLookupReturnRva
    0x05918850u,   // gfxManagerPtrRva
    0x06E839ACu,   // fovScaleRva
    0x00000000u,   // cameraGlobalRva
    0x06E81800u,   // appStateSlotRva
    {},            // trackedTransformCallers
};

// Steam Win64 build, TimeDateStamp 0x6AB0FFC9 (2026-09-21).
//
// Another ordinary patch on the September layout. Every structural constant the
// mod depends on came back identical - cTkCamera still starts at the object
// top, its field of view is still object+0xF0, the rows the renderer projects
// from are still object+0x50, the vtable slots are still 9 / 11 / 13, and the
// menu page is still globals+0x849020+0x1C3F8 - so only addresses moved.
//
// Derived from the shipped image, every scan run against NMS_20260917.exe first
// so it had to reproduce a value already verified in game before its answer
// here was taken:
//
//   * cameraTransformRva / weaponCameraRva - cGcCameraManager's RTTI vtable,
//     slots 11 and 13.
//   * cameraCommitRva - the gate the commit writes the accessor's row block
//     behind, `cmp byte [rax+0x4FF], 0` followed by `jne`, whose jump target is
//     reached whether or not the gated write happened. Fifteen camera
//     behaviours share that shape, so the right one is picked by walking the
//     .pdata unwind chain to its primary entry and naming the vtable that
//     points at it: cGcCameraBehaviourFirstPerson slot 4, as on every Steam
//     build since June. Position in the scan's output is not evidence and was
//     not used.
//   * gfxManagerPtrRva / fovScaleRva / gameGlobalsPtrRva - find_fov_helper.py.
//     The globals pointer is confirmed a second time by the RIP target in
//     cGcGenericSectionConditionInventoryOpen slot 1, which agrees.
//   * appStateSlotRva - the cGcApplication member 0x28 below the globals
//     pointer, the same relationship the 2026-09-17 profile carries.
//   * nguiFindElementRva / reticleLookupReturnRva / nguiRenderRva /
//     reticleRenderReturnRva and the aim caller lists - port_rva.py masked
//     signatures, each unique.
//   * playerShipRva - the masked signature matched TWO functions, so it was
//     settled by call sites instead: the shipped 2026-09-17 function has ~600
//     and so does 0x01479930, many at byte-identical addresses, while the other
//     candidate has 36.
//   * playerFromGlobals and menuPageModeOffset are offsets into the globals
//     block rather than code, so neither can be ported by signature.
//     menuPageModeOffset is read straight out of the condition above, and
//     0x0071C690 still appears as an immediate in ~1100 sites in both images,
//     in the same instruction shapes - it is a fixed constant of that block's
//     layout and did not move.
static const BuildProfile kSteamProfile_20260921 = {
    "steam-win64-20260921",
    { 0x6AB0FFC9u, 0x0760A000u, 0x00000000u },
    11,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0068D880u,   // cameraTransformRva
    0x00651FD8u,   // cameraCommitRva
    0x00000000u,   // accessorMirrorBytes
    0x000000F0u,   // cameraFovFromTransform
    0x00000050u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::None,  // commitCameraReg
    0x00317560u,   // weaponCameraRva
    0x00000000u,   // worldToScreenRva
    0x06E7AAE8u,   // gameGlobalsPtrRva
    0x00000000u,   // netPlayerSlotsBegin
    0x00000000u,   // netPlayerSlotsEnd
    0x00000000u,   // netPlayerConnectedByte
    0x00865418u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x001CB2A0u,   // nguiFindElementRva
    0x0098F9A1u,   // reticleLookupReturnRva
    0x05911B70u,   // gfxManagerPtrRva
    0x06E7CC6Cu,   // fovScaleRva
    0x00000000u,   // cameraGlobalRva
    0x06E7AAC0u,   // appStateSlotRva
    {},            // trackedTransformCallers
    {0x013E7D75u}, // aim ray origin
    {0x013EAA91u, 0x01459C1Cu}, // aim target and camera-relative movement input
    0x001DA030u,
    0x009910D7u,
    0x0071C690u,
    0x01479930u,
};

// Steam Win64 build, TimeDateStamp 0x6AAA83EE (2026-09-17).
//
// An ordinary patch on top of the September layout rather than another
// re-declaration: cTkCamera still starts at the top of the object, the selector
// byte is still manager+0x330, the two cameras are still +0x130 and +0x230, and
// the camera's field of view is still object+0xF0. Every accessor below
// disassembles byte for byte the same as its 2026-09-10 counterpart, so the
// vtable slots carry over unchanged and only the addresses moved.
//
// Derived entirely from the shipped image with the structural scans in .lab,
// each one run against NMS_20260910.exe first so the tool had to reproduce a
// value already verified in game before its answer for this build was taken:
//
//   * cameraTransformRva / weaponCameraRva - the cGcCameraManager RTTI vtable,
//     slots 11 and 13. Slot 9 still tests the selector at +0x330 and picks
//     between +0x130 and +0x230, slot 11 still calls it through [rax+0x48] and
//     copies five rows out, slot 13 is still the bare `lea rax,[rcx+0x130]`.
//   * gfxManagerPtrRva / fovScaleRva - find_fov_helper.py, which anchors on the
//     shape of the FOV read rather than on any byte around it.
//   * gameGlobalsPtrRva / menuPageModeOffset - cGcGenericSectionConditionInventoryOpen
//     slot 1, which is the same function shape as before with the block split
//     moved from 0x849000 + 0x1C3F8 to 0x849020 + 0x1C3F8.
//   * appStateSlotRva - the cGcApplication member 0x28 below the globals
//     pointer, as on every build so far, and carrying the same 115 `mov`
//     references here as 0x06E81800 does there.
//   * cameraCommitRva - find_commit.py paired with func_owner.py, which resolves
//     the owning function through the unwind chain to a vtable slot. Exactly one
//     save-then-commit pair in the image belongs to cGcCameraBehaviourFirstPerson
//     slot 4 on either build, and the twenty-five instructions around the pin are
//     identical between them, `jne` target included - which is what makes the pin
//     safe: it is reached whether or not the `cmp byte [rax+0x4ff]` gate let the
//     write to the live rows through.
//
// worldToScreenRva and netPlayerSlots* remain unavailable on this build.
static const BuildProfile kSteamProfile_20260917 = {
    "steam-win64-20260917",
    { 0x6AAA83EEu, 0x07601000u, 0x00000000u },
    11,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x0068DB00u,   // cameraTransformRva
    0x006522A8u,   // cameraCommitRva
    0x00000000u,   // accessorMirrorBytes
    0x000000F0u,   // cameraFovFromTransform
    0x00000050u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::None,  // commitCameraReg
    0x00317590u,   // weaponCameraRva
    0x00000000u,   // worldToScreenRva
    0x06E723F8u,   // gameGlobalsPtrRva
    0x00000000u,   // netPlayerSlotsBegin
    0x00000000u,   // netPlayerSlotsEnd
    0x00000000u,   // netPlayerConnectedByte
    0x00865418u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x001CB290u,   // nguiFindElementRva
    0x0098FF61u,   // reticleLookupReturnRva
    0x05909440u,   // gfxManagerPtrRva
    0x06E7457Cu,   // fovScaleRva
    0x00000000u,   // cameraGlobalRva
    0x06E723D0u,   // appStateSlotRva
    {},            // trackedTransformCallers
    {0x013E4825u}, // aim ray origin
    {0x013E7541u, 0x014566ACu}, // aim target and camera-relative movement input
    0x001DA020u,
    0x00991697u,
    0x0071C690u,
    0x01476360u,
};

// Xbox Game Pass (GDK) Win64 build, TimeDateStamp 0x6AA8CC1C, package 7.3.0.0
// (2026-09-21). The Game Pass copy cannot be read from disk - the licensing
// filter refuses every process but the launcher - so this was derived from the
// mapped image dumped out of the running game by .lab/scripts/dump_module.py,
// with every scan run against the 2026-09-09 dump first so it had to reproduce
// a value that profile already carries:
//
//   * cameraTransformRva / weaponCameraRva - cGcCameraManager RTTI vtable slots
//     11 and 13; the control reproduced 0x00835670 and 0x004EC460.
//   * cameraCommitRva - this binary inlines far less than the Steam one, so the
//     commit is ONE shared routine the behaviours call rather than one folded
//     into each, and no gate belongs to a behaviour's slot 4. That routine ends
//     with its gated block, so the `jne` past it lands on the function's own
//     `ret`. Exactly one gate per image has that, and on the control it is
//     0x00808770 - the address that profile pins.
//   * fovScaleRva / gfxManagerPtrRva - the projection helper's FOV read; the
//     control reproduced 0x0532314C and 0x05CAA4D8.
//   * appStateSlotRva / gameGlobalsPtrRva - this binary does not inline the
//     globals read, so they cannot be taken from the menu condition the Steam
//     builds use. They sit at fixed distances from the singleton pointer that
//     condition DOES load: +0x10 and +0x38, which reproduces the control's
//     0x05320FA0 / 0x05320FC8. The FSM slot is confirmed a second time by the
//     live scan, which found 0x0531D720 in the running game.
//
// decoupleByRowBlock is what makes aim work here. The whole-accessor mirror
// breaks the frame on this binary and no caller list has been derived for it,
// so the two row blocks do the job instead - tracked rows to the block the
// renderer projects from, clean rows to the block every camera query lands on.
//
// menuPageModeOffset is derived, not inherited. The 2026-09-09 value was
// carried across untested and read "menu open" on every frame of gameplay -
// suppressed=1800, applied=0, head tracking dead in world with every hook
// correctly installed.
//
// This binary does not inline the read, so it cannot be taken from the
// condition the Steam builds use; that function just calls two helpers. They
// hold it between them: one is `mov rax,[rcx+0x38]; add rax,0x849020; ret`, and
// the other opens `mov eax,[rcx+0x1C438]` then returns false on 0 and on 2,
// which is the same "a page other than 0 or 2 means a menu is up" rule Steam
// uses. 0x849020 + 0x1C438 = 0x865458.
//
// The base is what moved: 0x849050 on 2026-09-09 against 0x849020 here, so the
// old value was 0x30 too high. The field itself sits at 0x1C438 on both GDK
// builds and at 0x1C3F8 on Steam, so it does not carry between the two
// compilers either. Re-derive it per build; do not inherit it.
// The reticle pins are the NGui by-name element lookup and the one HUD call
// site that asks it for the crosshair. Neither ports by signature, so both came
// from structure:
//
//   * nguiFindElementRva - the lookup reads its element table from [rcx+0x150]
//     exactly as the Steam one does. It is NOT found by the table-walk shape
//     the Steam function has, because this binary inlines the name hash (the
//     `movabs r10, 0x9DDFEA08EB382D69`) rather than calling out to it.
//   * reticleLookupReturnRva - of that lookup's 1122 call sites, exactly one
//     reads the element NAME and the NGui object out of the HUD at +0xEC7D0 and
//     +0xEC5B0. Those are the same two offsets the Steam site uses, to the byte:
//     the class layout is shared between the two binaries even though none of
//     the code addresses are. The crosshair's name is not a constant - the HUD
//     looks it up afresh each frame because it changes with the equipped weapon
//     - which is what makes "name read from an object field" the thing to
//     search for rather than a string reference.
//
//   * nguiRenderRva - the GUI render entry, found by the three class offsets
//     its Steam counterpart reads (+0x168, +0x468, +0x46C). That triple is not
//     unique on its own - 24 functions carry it on Steam and 27 here - so it
//     only narrows the field.
//   * playerShipRva - the handle resolver the Steam profile pins, found by its
//     shape: read a handle at player+0x168, bounds-check against 0xFEFF, index
//     a table with stride 0x58 and validate at +0x50. Five functions match on
//     each image; on Steam the pinned one has 566 call sites against 36 for the
//     runner-up and 0 for the rest, and here one has 575 and the other four
//     have none.
// The two clean-served aim callers were found by different means and neither
// ports from Steam:
//
//   * 0x01596961 - camera-relative movement input. Matches Steam's 0x01459C1C
//     instruction for instruction, including `mov rdx,[rsi+0x2a8]` before the
//     call and `mov r8,rbx; lea rdx,[rsp+0x30]` after it. It reaches the camera
//     manager through getter 0x4EC4D0 (base 0x8F5B60) where Steam adds
//     0x8F5A90 inline, which is also how that offset was pinned.
//   * 0x016CCB2D - the weapon ray. Found by READ WATCH DIFF rather than by
//     matching Steam at all: one capture while the player never fired, one
//     while they held the mining beam, and this is the reader that appears only
//     in the second, at 392 hits against 0 - roughly twice a frame of sustained
//     fire. Its function takes the camera and immediately reads all five rows.
//     Steam's remaining aim caller still has no twin here and did not need one.
//
// The diff is the method to reuse. Asking which code reads the camera gives a
// hundred answers; asking which code reads it ONLY WHILE DOING THE THING gives
// one.
//
//   * playerFromGlobals - the SAME 0x71C690 Steam uses, which is not obvious
//     from the code: Steam does `globals + 0x71C690` in one instruction, while
//     this binary calls a getter that returns `globals + 0x4CCF90` and then
//     adds 0x24F700. 0x4CCF90 + 0x24F700 = 0x71C690. The player's place in the
//     globals block is shared between the two compilers even though the menu
//     page's is not.
//   * reticleRenderReturnRva - what settles it. On Steam the reticle's lookup
//     and its render call sit in the SAME function (primary 0x98F720), so the
//     render call had to be inside the function that owns the lookup here too.
//     Function 0xB18240 makes exactly one call into any of the 27 candidates.
//     The function it lands on has the same prologue as the Steam one, byte for
//     byte through the first four instructions, and reads all three offsets.
static const BuildProfile kGdkProfile_20260921 = {
    "gdk-win64-20260921",
    { 0x6AA8CC1Cu, 0x075B6000u, 0x00000000u },
    11,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x00836760u,   // cameraTransformRva
    0x008098E0u,   // cameraCommitRva
    0x00000000u,   // accessorMirrorBytes
    0x000000F0u,   // cameraFovFromTransform
    0x00000050u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::Rcx,   // commitCameraReg - shared commit routine
    0x004EC4C0u,   // weaponCameraRva
    0x00000000u,   // worldToScreenRva
    0x0531D748u,   // gameGlobalsPtrRva
    0x00000000u,   // netPlayerSlotsBegin
    0x00000000u,   // netPlayerSlotsEnd
    0x00000000u,   // netPlayerConnectedByte
    0x00865458u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x003B9770u,   // nguiFindElementRva
    0x00B1840Fu,   // reticleLookupReturnRva
    0x05CA76B8u,   // gfxManagerPtrRva
    0x0531F8CCu,   // fovScaleRva
    0x00000000u,   // cameraGlobalRva
    0x0531D720u,   // appStateSlotRva
    {},            // trackedTransformCallers
    {},            // aimCopyCallers - all 16 copy callers served clean moved nothing
    // The aim ray and the camera-relative movement input, both found by what
    // they do rather than by matching Steam. Served the clean camera on its own,
    // 0x01554253 keeps the mining beam on the mouse direction under a 15 degree
    // head turn, in first and third person, with culling unchanged from the
    // fully tracked camera. Without 0x01596961, W walks 13-15 degrees toward
    // where the head points; with it, within a degree of the mouse. It is
    // Steam's movement caller 0x01459C1C instruction for instruction.
    //
    // Every earlier search here concluded the aim ray read camera memory rather
    // than this accessor. It was wrong because the clean camera handed out was
    // 20 floats of rows, where this build's callers read the whole object.
    {0x01554253u, 0x01596961u}, // aimTransformCallers
    0x003E6580u,   // nguiRenderRva
    0x00B19C05u,   // reticleRenderReturnRva
    0x0071C690u,   // playerFromGlobals
    0x015A3820u,   // playerShipRva
    // sceneSampleRva - the main thread's once-a-frame read of the render rows,
    // found by a read watch on camera+0x50. This build commits the camera from
    // whichever thread the job system picks, so about one HUD frame in five saw
    // a commit the picture was not drawn from, and the reticle froze for a frame
    // then took a double step. Placed from the commit read here, a 6 degree
    // head sweep measures the reticle within 1 px of the scenery around it,
    // against 4 px RMS and 15 px peaks from the newest commit. Hit in first and
    // third person alike.
    0x00516884u,
    // cameraCommitThirdPersonRva - the same gate shape inside
    // cGcCameraBehaviourThirdPerson slot 4 (0x0067D710), whose `jne` lands on
    // 0x0067FBC6. PlayerThirdPerson and SpacewalkThirdPerson share that exact
    // function, so this one address carries the on-foot third-person view, the
    // spacewalk and the player third-person camera together. The camera is in
    // RSI there, the same register the first-person site uses: both targets end
    // `mov rcx, rsi` into the same call.
    0x0067FBC6u,
};

// Xbox Game Pass (GDK) Win64 build, TimeDateStamp 0x6AA13600 (2026-09-09).
//
// Same game version as the Steam build above and the same class layout, but a
// separate binary with its own addresses: it is built with far less inlining,
// so whole functions the Steam image folds into their callers survive as calls
// here and a byte signature carries nothing between the two. Every value below
// was derived against this build's own image, by the same structural scans.
//
// The image is ACL-protected on disk (only the licensed launcher may read
// Content\Binaries\NMS.exe), so it was read out of the running process instead;
// RVAs are base-relative, so a mapped image gives the same numbers a file would.
//
// The commit site is the same moment as the Steam one, reached differently.
// Steam inlines the five-row write into each camera behaviour, so its pin sits
// inside cGcCameraBehaviourFirstPerson. This build does not inline it: all
// thirty behaviours call one shared writer at 0x008086D0, which does exactly
// what the Steam inline code does - save camera+0x50..+0x90 up to +0xA0..+0xE0,
// write the frame's rows to +0x50..+0x90, then to the live rows at +0x00..+0x40
// behind a `cmp byte [globals+0x4FF]` gate. The pin is that writer's own `ret`.
//
// So this profile injects for whichever behaviour owns the camera, where the
// Steam one injects only for the first-person behaviour. That is not a choice:
// a write watch in world named 0x00808750 - the writer's first row store - and
// nothing else, while an execute breakpoint on the first-person behaviour's own
// call to it never fired once across two sessions, armed on 127 of 128 threads.
// What holds the two builds to the same behaviour is the guard in
// OnRenderPhaseBegin, which refuses any frame whose live rows still carry this
// mod's own last write, so a behaviour that is not driving the view is left
// alone rather than composed onto twice.
//
// The menu gate reads the same int the Steam build does, reached the same way:
// cGcGenericSectionConditionInventoryOpen takes the block from
// cGcApplication+0x38 and tests the int at block+0x865488 against 0 and 2. This
// build reaches it through two calls where Steam inlines the arithmetic, which
// is the only difference. worldToScreenRva and netPlayerSlots* are 0 for the
// same reasons as the Steam September profile.
static const BuildProfile kGdkProfile_20260909 = {
    "gdk-win64-20260909",
    { 0x6AA13600u, 0x075B9000u, 0x00000000u },
    11,            // copyTransformVfunc
    9,             // activeTransformVfunc
    0x00835670u,   // cameraTransformRva
    0x00808770u,   // cameraCommitRva
    0x00000000u,   // accessorMirrorBytes
    0x000000F0u,   // cameraFovFromTransform
    0x00000050u,   // renderRowsFromTransform
    0x00000000u,   // decoupleByRowBlock
    BuildProfile::CommitCameraReg::None,  // commitCameraReg
    0x004EC460u,   // weaponCameraRva
    0x00000000u,   // worldToScreenRva
    0x05320FC8u,   // gameGlobalsPtrRva
    0x00000000u,   // netPlayerSlotsBegin
    0x00000000u,   // netPlayerSlotsEnd
    0x00000000u,   // netPlayerConnectedByte
    0x00865488u,   // menuPageModeOffset
    0x00000000u,   // weaponZoomOffset
    0x00000000u,   // nguiFindElementRva
    0x00000000u,   // reticleLookupReturnRva
    0x05CAA4D8u,   // gfxManagerPtrRva
    0x0532314Cu,   // fovScaleRva
    0x00000000u,   // cameraGlobalRva
    0x05320FA0u,   // appStateSlotRva
    {},            // trackedTransformCallers
};

// Newest-first (top entry is the diagnostic primary for mismatch wording).
static const BuildProfile* const kKnownProfiles[] = {
    &kSteamProfile_20260921,
    &kGdkProfile_20260921,
    &kSteamProfile_20260917,
    &kSteamProfile_20260910,
    &kGdkProfile_20260909,
    &kSteamProfile_20260618,
    &kSteamProfile_20260605,
    &kSteamProfile_20260530,
};

const BuildProfile* SelectProfile(void* moduleBase,
                                  PeFingerprint* outRunning,
                                  const BuildProfile** outPrimary) {
    if (outPrimary) *outPrimary = kKnownProfiles[0];

    PeFingerprint running{};
    if (!ReadPeFingerprint(moduleBase, running)) {
        if (outRunning) *outRunning = PeFingerprint{};
        return nullptr;
    }
    if (outRunning) *outRunning = running;

    for (const BuildProfile* p : kKnownProfiles) {
        if (p->fingerprint.Matches(running)) return p;
    }
    return nullptr;
}

}  // namespace NMSHT
