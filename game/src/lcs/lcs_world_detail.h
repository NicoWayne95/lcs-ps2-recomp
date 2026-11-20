// [lcs] Draw distance (the game's LOD scale 0x3D7578), src/lcs/lcs_world_detail.cpp.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs_world
{
    void install(PS2Runtime &runtime);
    bool installed();
    // 1.0 = the original game, 0.5 .. 3.0; any thread, applied at the next CCamera::Process
    void setDrawDistance(float scale);
    float drawDistance();
    // the far plane and fog start from the timecycle times this (1.0 = the original game), buildings / roads / props
    // LCS_WORLD_DUMP=1 only: the guest RAM to logs\world_<n>.bin at the next frame (F10)
    void requestWorldDump();
    void setWorldDistance(float scale);
    float worldDistance();
    bool worldDistanceInstalled();
    float timecycleFar();
    float appliedFar();
    float timecycleFog();
    float lodMultiplier();
    float generationMultiplier();
    // which 0 = main heap 0x406D50, 1 = streaming heap 0x634140
    bool heapUse(int which, uint32_t &used, uint32_t &size);
}
