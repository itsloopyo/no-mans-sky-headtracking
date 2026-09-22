#pragma once

#include "build_profile.h"

namespace NMSHT {

// Derives a profile for a build the registry has never seen, by recognising
// engine structure in the already-mapped image rather than by pinned address.
//
// This runs ONLY after SelectProfile has returned nothing. A pinned profile is
// always authoritative: it was verified in a running game, and a resolver that
// merely agrees with it adds nothing while a resolver that disagrees with it is
// the one thing that must never decide.
//
// Every resolver below self-checks, and any failure returns nullptr and leaves
// the mod dormant. That is the same failsafe as before; what changes is that a
// routine patch now has a chance of landing on "works, unverified" rather than
// certainly landing on "dead until someone derives the addresses by hand".
//
// What it deliberately does NOT resolve: the aim caller lists and playerShipRva
// needed call-site counting and judgement to pin, and a build where those come
// back zero still runs - the aim follows the view, which is the same compromise
// several shipped profiles already carry. Guessing them would be worse than
// leaving them out.
//
// `out` is filled and returned on success. Its storage is the caller's.
const BuildProfile* ResolveProfileFromImage(void* moduleBase, BuildProfile& out);

}  // namespace NMSHT
