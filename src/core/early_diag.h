#pragma once

#include <windows.h>

namespace NMSHT {

// Proof-of-load diagnostic, written next to this DLL as <module>_diag.txt.
//
// This is the only reporting channel that works before Mod::Initialize opens
// HeadTracking.log: DllMain runs first, and the game starts polling the pad
// inside the init thread's settling delay. HT_LOG lines emitted before the
// open are dropped, so anything that has to survive a failure that early goes
// here as well.
//
// Written with CreateFile/WriteFile rather than a CRT FILE* because DllMain
// calls it under the loader lock, and opening a FILE* there takes the CRT's
// stdio locks, which inverts against any thread that holds them and then loads
// a library.
void WriteEarlyDiag(const char* message);

} // namespace NMSHT
