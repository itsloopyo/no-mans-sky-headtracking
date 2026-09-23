#pragma once

#include <cstdint>

namespace NMSHT {

// Tracks which cGcApplication FSM state the game is in, so head tracking runs
// only during actual play.
//
// NMS drives its whole life cycle from one finite state machine whose states
// are real classes with intact RTTI - cGcApplicationTitleScreenState,
// GameModeSelectorState, GlobalLoadState, LocalLoadState, BaseLoadingState,
// DeathState, GalacticMapState, SimulationState and a few more. So the state is
// not something to infer from a paused clock or a locked cursor: the engine
// names it, and the only work is finding the pointer the FSM keeps it in.
//
// That pointer is found by scanning the module's writable data for a qword that
// points at an object whose vtable is one of the resolved state vtables. The
// scan is what makes this survive a patch; `appStateSlotRva` pins the answer
// for a known build so a session does not depend on the scan finding exactly
// one candidate.
struct GameStateOffsets {
    std::uint32_t appStateSlotRva;
    std::uint32_t gameGlobalsPtrRva;
    std::uint32_t netPlayerSlotsBegin;
    std::uint32_t netPlayerSlotsEnd;
    std::uint32_t netPlayerConnectedByte;
    std::uint32_t menuPageModeOffset;
    std::uint32_t playerFromGlobals;
    std::uint32_t playerShipRva;
};

// Installs and never uninstalls: the poll thread outlives every caller, and
// the mod has no unload path (see Mod and CameraHook::Install).
void InstallGameStateProbe(const GameStateOffsets& offsets);

// True while the game is in the simulation state with no in-game menu page up -
// the only condition in which the player is looking through the first-person
// camera at a live world.
bool IsInGameplay();

// The gate and the reason for it, from ONE sample, and the only way to read
// them. Two separate accessors were two loads, and a publish between them paired
// one verdict's gate with another's reason - which is how the log came to name a
// cause that never happened.
//
// `reason` is non-null whenever the pose is standing down.
bool TrackingApplies(const char*& reason);

bool GetWalkingUp(float up[3]);
bool IsInShip();

// Name of the state class the FSM currently holds, or "unresolved".
const char* CurrentStateName();

}  // namespace NMSHT
