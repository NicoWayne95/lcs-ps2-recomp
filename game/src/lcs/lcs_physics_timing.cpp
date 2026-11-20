// [lcs] Vehicle physics that ran once a frame instead of per unit of time (a port of the reference port's
// src/game/ref_physics_timing.cpp, part 3, to LCS).
//
// A bike's brake wobble. CBike::ProcessControl (LCS sub_00159000, the block at 0x15BDC4..0x15BF0C - the same code as
// the reference port 0x33BD18..0x33BE54, its constants 0.03 / 0.007 at 0x15BE8C / 0x15BEB0): while the brake leads the gas by more
// than 0.9 (+0x250 / +0x24C), the bike rolls forwards and a wheel is on the ground, a phase grows by random * 0.2 *
// step and the bike gets ApplyTurnForce (LCS 0x2BC980: f12-f14 the force, f15-f17 the point, a0 the vehicle) once a
// frame - the step only in the phase. At 60 fps that is twice the PS2's shoves per second (LCS ran at 30), more at
// higher rates: braking from speed threw the bike sideways above 60 fps (user report 2026-11-16). The hook scales that
// one call's force (return address 0x15BF0C) by the frame's step over a PS2 frame's (50/30), as the reference port does.
//
// LCS_PHYSICS_TIMESTEP=0: as the game does it (once a frame).
//
// Not ported yet (see HANDOFF_CLAUDE_2026-11-16.md): the reference port's rolling resistance in CVehicle::ProcessWheel (a brake per
// frame without the step: cars coast down and reach top speed slower at high fps), the push from a body under a wheel,
// the rotor blades' damage, the water cannon, vehicles in water.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kApplyTurnForce = 0x002BC980u;    // CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
    constexpr uint32_t kBrakeWobbleReturn = 0x0015BF0Cu; // CBike::ProcessControl's call: the brake wobble
    constexpr uint32_t kTimeStep = 0x003D9B8Cu;          // CTimer::ms_fTimeStep (1.0 = 1/50 s)
    constexpr float kPs2Step = 50.0f / 30.0f;            // a PS2 frame (LCS: 30 fps)

    PS2Runtime::RecompiledFunction g_origTurnForce = nullptr;

    // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
    float frameScale(const uint8_t *rdram)
    {
        float step;
        std::memcpy(&step, rdram + kTimeStep, 4);
        return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
    }

    PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 31) == kBrakeWobbleReturn)
        {
            const float scale = frameScale(rdram);
            ctx->f[12] *= scale;
            ctx->f[13] *= scale;
            ctx->f[14] *= scale;
        }
        g_origTurnForce(rdram, ctx, runtime);
    }
}

namespace lcs_physics_timing
{
    bool install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("LCS_PHYSICS_TIMESTEP");
        if (v && *v == '0')
            return false;
        g_origTurnForce = runtime.lookupFunction(kApplyTurnForce);
        const bool ok = g_origTurnForce && runtime.replaceFunction(kApplyTurnForce, &applyTurnForce);
        std::fprintf(stderr, "[physics-timing] bike brake wobble on the time step: %s\n", ok ? "on" : "ApplyTurnForce NOT found");
        return ok;
    }
}
