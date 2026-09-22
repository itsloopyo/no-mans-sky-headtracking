#pragma once

#include <cstdint>

namespace NMSHT {

// Records the live camera transform's address, taken from the accessor's own
// return value so the watch lands on whichever of the manager's two transforms
// is currently active.
void NoteTransformAddress(const float* transform);

// Marks that gameplay has started. The copy accessor is never called at the
// frontend, so its first call is the signal that a scene is actually being
// rendered - without it the bursts fire during the load screen and find only
// whatever housekeeping runs there.
void NoteInWorld();

// Whether NoteInWorld has fired yet. The frame-phase watchdog times itself from
// this rather than from process start: the engine commits no camera at the
// frontend, and a cold start spends minutes there.
bool SawInWorld();

// One-shot background probe: puts a CPU data breakpoint on the first four bytes
// of that transform and logs which instructions touch it.
//
// This exists because no amount of hooking the accessor can say who the
// renderer is. Handing the accessor's callers a private rotated copy moves
// nothing on screen while writing the same rotation into the engine's own
// transform does, so the renderer reads that memory directly rather than
// through the pointer it was handed. Screenshot sweeps can only score how long
// an in-place write survived, never who consumed it; a data breakpoint names
// the reading instruction outright.
//
// Armed in short bursts. A read watch on a structure the engine touches
// thousands of times a second makes the game unplayable while it is on.
void StartReadWatch(uintptr_t offsetBytes);

// The same probe armed for WRITES ONLY. The read watch answers "who consumes the
// camera"; this answers "who produces it". Today's frame-window measurements
// showed the renderer samples the transform in the present -> acquire span, so
// the instruction that writes it each frame is the boundary the head rotation
// has to be injected after. A write watch is also far quieter than the read
// watch - a handful of hits a frame rather than 167 - so it does not bring the
// game down the way two 600ms read bursts did.
void StartWriteWatch();

// The same write watch pointed at a FIXED address instead of the camera
// transform. The weapon probe uses it on a node it has proved follows the head:
// the instruction that writes that node's basis is the one to sandwich, and
// there is no other way to name it - the node is reached through a pointer the
// engine holds, not through any function this mod hooks.
void StartWriteWatchAt(uintptr_t address, const char* label);

} // namespace NMSHT
