#pragma once

#include <cstdint>

namespace NMSHT {

// The two cGcCameraManager vtable slots this mod hooks are per build - see
// BuildProfile::copyTransformVfunc / activeTransformVfunc. They used to be
// constants here, which was right while every shipped build declared the class
// the same way; the September 2026 builds re-declare it and move the copy
// accessor from slot 10 to slot 11, so a constant would hook the wrong function
// on a build whose fingerprint the mod recognises.

// Engine camera hook for Hello Games' proprietary Vulkan engine (NMS.exe).
//
// The renderer does NOT read the copy cGcCameraManager::vtable[10] hands its
// callers - rotating that moves nothing on screen, which cost this mod a lot of
// time to establish. It reads the LIVE transform that vtable[9] returns a
// pointer into, and so does everything else: aim, raycasts, audio, culling.
//
// So the rotation is written into that live transform directly, at the engine's
// own camera commit (OnRenderPhaseBegin, reached from the execute breakpoint in
// commit_hook.cpp). The renderer picks it up because it reads that memory.
//
// The decoupling comes from the other side: vtable[9] is hooked so that every
// caller that goes through the ACCESSOR is handed a mirror of the clean rows
// instead of the live ones. Aim, raycasts and weapon fire read through that
// pointer, so they keep the camera the player is really pointing, while the
// renderer reads the rotated memory. Selecting per return address was tried and
// retired: it moved no pixels, because no renderer is among the accessor's
// callers.
//
// vtable[10]'s address is resolved from RTTI and survives code motion. The
// commit site cannot be - it is an address inside a chunked function with no
// direct callers - so it is pinned per build in build_profile.cpp, and a build
// with no profile gets no head tracking rather than a wrong camera.
class CameraHook {
public:
    static CameraHook& Instance();

    // Installs and never uninstalls. The image is pinned (see
    // StartFramePhaseInstaller) because the engine caches Vulkan detour
    // pointers it can never be handed back, and the hardware breakpoints are
    // armed from detached threads, so there is no point at which this could be
    // taken out of a running process safely.
    void Install();

    CameraHook(const CameraHook&) = delete;
    CameraHook& operator=(const CameraHook&) = delete;

private:
    CameraHook()  = default;
    ~CameraHook() = default;

    bool m_installed = false;
};

// Called from the commit hook, immediately after the engine has written the
// frame's camera into its live transform: this captures that clean camera and
// writes the head-tracked one over it. Nothing puts the clean rows back - the
// engine's own next commit is the restore, which is why the game's update, which
// runs against that commit, reads the camera the player is really pointing.
// `committedCamera` is the camera the engine has just written, read out of the
// register BuildProfile::commitCameraReg names, or nullptr on a build that
// names none. When it is given and is not the live camera, the frame is not
// ours to act on - see that field for why Game Pass needs this and Steam does
// not.
void OnRenderPhaseBegin(void* committedCamera);

// How many frames have reached the injection point. Zero after the game has
// been drawing for a while means the mod is hooked to something the renderer
// never calls, which is indistinguishable from a working mod without it.
uint64_t RenderPhaseBeginCount();

// Called from the present hook purely so the frame-phase line can show how many
// frames were drawn per engine camera commit.
void CountPresent();


// The engine's untouched camera basis for the most recent commit, five rows of
// four floats. Null until the first commit has been captured. The weapon probe
// scores candidate bases against it; nothing else should write through it.
const float* CleanCameraRows();

// The rotated rows last written into the engine's transform, five rows of four.
const float* AppliedCameraRows();

// The clean rows and the rows written from them at ONE commit, published as one
// object. Anything that projects one through the other must use this: reading
// CleanCameraRows and AppliedCameraRows separately can pair this commit's clean
// camera with the previous commit's applied one, which is off by a commit's
// worth of mouse movement. `commit` counts commits; `qpc` is when it landed.
struct CameraPair {
    float clean[20];
    float applied[20];
    uint64_t commit;
    int64_t qpc;
};
// Null whenever no pose is applied.
const CameraPair* CurrentCameraPair();

// Called at the instruction where the frame is set up from the render rows:
// records which published pair those rows belong to.
void NoteSceneSample();

// The pair the reticle is placed from: the one the frame was set up with where
// the build pins a sample site, the newest otherwise. Null when no pose applies.
const CameraPair* ReticleCameraPair();

// Diagnostic totals: samples whose rows matched no published pair, and reticle
// reads that fell back to the newest pair because the sampled one was stale.
void SceneSampleStats(uint64_t& unmatched, uint64_t& stale);

// Pins the injection to the rows currently applied, so every copy the engine
// makes of them holds one exact bit pattern for as long as a memory scan needs
// to find it. Without this the camera has moved on long before the scan reaches
// the region holding a given copy, and the search matches nothing. The view
// stops following the head while it is frozen. False if nothing is applied yet.
// Freezes on the clean camera composed with a yaw-only head rotation of the
// given angle, so a scan can be run twice at two known head angles and keep
// only what actually moved between them. Clean is not re-captured while frozen,
// so both passes share one reference pose.
bool FreezeWithYaw(float yawDegrees);
void UnfreezeAppliedRows();

float* CleanCameraForTransform(float* transform);

// The camera the weapon is placed from: the clean basis, so the weapon stays on
// the aim, at the LEANED eye. The weapon hangs about half a metre in front of
// the eye, so placing it at the clean eye would let a lean throw it most of the
// way across the frame and take its sights off the eye; riding with the eye it
// keeps its place in the frame while the lean parallaxes the world past it.
float* WeaponCameraForTransform(float* transform);

// The vertical field of view, in degrees, that the engine is rendering with
// right now, or 0 before the live camera has been located or on a build whose
// profile pins no FOV scale.
//
// Read live, and from BOTH of its terms. The game owns this number - Options >
// Camera has On-Foot Field of View and Flight Field of View, each 60 to 120
// degrees, default 75 - and it does not write the player's choice into the
// camera: the camera's own field stays at 75 and the setting arrives as a
// global multiplier. Measured in world: at 75 degrees the scale is 1.0 and the
// field 75; at 100 degrees the scale is 1.3333 and the field is still 75. Read
// only the camera and the answer is 75 whatever the player picked.
float LiveCameraFovDegrees();

// The two terms that product is built from: the live camera's own field and the
// global scale the player's Options > Camera setting arrives as. False when
// neither is readable.
//
// Worth having separately because the two fail differently and the sum of them
// looks reasonable either way. A field that is not 75 says the live camera is
// not the one this mod was measured against; a scale that is not the player's
// setting divided by 75 says the wrong global is pinned. Either makes every
// screen-space correction wrong by a constant ratio, which reads as the reticle
// travelling too far rather than as anything being broken.
bool LiveCameraFovTerms(float& field, float& scale);

// Which of this mod's two cameras `rows` carries, compared bit for bit:
// 0 = neither, 1 = the clean camera the accessor hands out, 2 = the tracked
// camera the commit hook wrote into the engine's live transform. A consumer
// that read the live transform straight from memory gets 2; one that went
// through the accessor gets 1. Which of the two a HUD projection was handed is
// what decides where the reticle correction belongs.
int ClassifyCameraBasis(const float* rows);

} // namespace NMSHT
