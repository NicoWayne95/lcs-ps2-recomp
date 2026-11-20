// [lcs-recomp] A copy of every VU1 XGKICK packet for checkers (game thread). Set g_kicks to a vector to collect the
// packets the VU1 sends to the GS (they still go to the GS); null (the default) costs one branch per kick.
// Used by src/game/lcs_hle_world.cpp (PS2X_HLE_VERIFY) to compare its clipper with the VU1's.
#pragma once

#include <cstdint>
#include <vector>

namespace ps2vu1tap
{
    extern std::vector<std::vector<uint8_t>> *g_kicks;
}

struct VU1State;

namespace ps2vu1tap
{
    // [lcs] diagnostics (LCS_LIT_TRACE, src/game/lcs_hle_world.cpp): g_pairHook is called before every pair the VU1
    // interpreter runs, with the state as it is then (a diagnostic may change it); a run that starts with it set does not enter compiled VU1 code.
    // g_kickHook sees every XGKICK packet (also when g_kicks is set). Null (the default) costs one branch each.
    extern void (*g_pairHook)(uint32_t pc, VU1State &state);
    extern void (*g_kickHook)(const uint8_t *packet, uint32_t bytes);
}

namespace ps2vu1tap
{
    // The MSCAL that just ended was handled by a game hook (the VU1 did not run): set by the VIF1 interpreter before
    // ps2hle::afterMscal, read by the survey to report native and VU1 work apart.
    extern bool g_mscalNative;
}
