#pragma once

#include <cstdint>

namespace NMSHT {

// Profiles with a render site correct the currently visible reticle groups
// for the duration of the crosshair draw. Older profiles use the lookup hook.
void InstallReticle(std::uint32_t nguiFindElementRva,
                    std::uint32_t reticleLookupReturnRva,
                    std::uint32_t gfxManagerPtrRva,
                    bool sweep,
                    std::uint32_t nguiRenderRva = 0,
                    std::uint32_t reticleRenderReturnRva = 0);

std::uint64_t ReticlePlacementCount();
void LastReticleOffset(float& x, float& y);

}  // namespace NMSHT
