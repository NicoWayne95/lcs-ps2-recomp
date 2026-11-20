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
// The wheel routines scale the passive brake after dividing by handling mass (see lcs_codegen_edits.json).
// The active braking and traction paths bypass that calculation.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kApplyMoveForce = 0x002BC870u;
    constexpr uint32_t kApplyTurnForce = 0x002BC980u;    // CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
    constexpr uint32_t kBrakeWobbleReturn = 0x0015BF0Cu; // CBike::ProcessControl's call: the brake wobble
    constexpr uint32_t kTimeStep = 0x003D9B8Cu;          // CTimer::ms_fTimeStep (1.0 = 1/50 s)
    constexpr float kPs2Step = 50.0f / 30.0f;            // a PS2 frame (LCS: 30 fps)

    PS2Runtime::RecompiledFunction g_origMoveForce = nullptr;
    PS2Runtime::RecompiledFunction g_origTurnForce = nullptr;

    // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
    float frameScale(const uint8_t *rdram)
    {
        float step;
        std::memcpy(&step, rdram + kTimeStep, 4);
        return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
    }

    // CPhysical::ApplyCollision keeps its two participants in s5 and s4.
    // Only its impulse calls can use those registers as entity pointers here.
    bool playerPushesVehicle(const uint8_t *rdram, R5900Context *ctx)
    {
        switch (GPR_U32(ctx, 31))
        {
        case 0x2C0024u: case 0x2C0044u: case 0x2C046Cu: case 0x2C06C0u:
        case 0x2C076Cu: case 0x2C0AE8u: case 0x2C0E74u: case 0x2C0E94u:
        case 0x2C0F3Cu: case 0x2C16B4u: case 0x2C16D4u: case 0x2C17F8u:
        case 0x2C181Cu:
            break;
        default:
            return false;
        }

        const uint32_t vehicle = GPR_U32(ctx, 4);
        const uint32_t a = GPR_U32(ctx, 21), b = GPR_U32(ctx, 20);
        if (a == b || (vehicle != a && vehicle != b) ||
            vehicle < 0x00100000u || vehicle > PS2_RAM_SIZE - 0x78u)
            return false;

        uint32_t player;
        const uint32_t playerInfo = 0x00408E40u + rdram[0x003D9BC8u] * 0x190u;
        std::memcpy(&player, rdram + playerInfo, sizeof(player));
        if (player != (vehicle == a ? b : a) ||
            player < 0x00100000u || player > PS2_RAM_SIZE - 0x539u ||
            rdram[player + 0x538u])
            return false;

        uint64_t flags;
        std::memcpy(&flags, rdram + vehicle + 0x40u, sizeof(flags));
        const unsigned type = (flags >> 33) & 7u;
        const unsigned status = (flags >> 36) & 31u;
        if (type != 2u || (status != 4u && status != 5u))
            return false;

        float velocity[2];
        std::memcpy(velocity, rdram + vehicle + 0x70u, sizeof(velocity));
        constexpr float slowSpeed = 0.12f; // 6 m/s in the game's 50 Hz speed units
        const float x = ctx->f[12], y = ctx->f[13], z = ctx->f[14];
        return velocity[0] * velocity[0] + velocity[1] * velocity[1] < slowSpeed * slowSpeed &&
               x * x + y * y > 4.0f * z * z;
    }

    void scaleForce(R5900Context *ctx, float scale)
    {
        ctx->f[12] *= scale;
        ctx->f[13] *= scale;
        ctx->f[14] *= scale;
    }

    void scalePlayerPush(const uint8_t *rdram, R5900Context *ctx)
    {
        if (!playerPushesVehicle(rdram, ctx))
            return;
        const float scale = frameScale(rdram);
        static const bool trace = std::getenv("LCS_TRACE_PLAYER_PUSH") != nullptr;
        static unsigned samples = 0;
        if (trace && samples++ < 32u)
            std::fprintf(stderr, "[physics-timing] player push at %08x: %.3f %.3f %.3f, scale %.3f\n",
                         GPR_U32(ctx, 31), ctx->f[12], ctx->f[13], ctx->f[14], scale);
        if (scale < 1.0f)
            scaleForce(ctx, scale);
    }

    PS2_GUEST_CC void applyMoveForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scalePlayerPush(rdram, ctx);
        g_origMoveForce(rdram, ctx, runtime);
    }

    PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 31) == kBrakeWobbleReturn)
            scaleForce(ctx, frameScale(rdram));
        else
            scalePlayerPush(rdram, ctx);
        g_origTurnForce(rdram, ctx, runtime);
    }
}

// Called only by the passive rolling resistance branch of the generated wheel routines.
float lcsScaleRollingResistance(const uint8_t *rdram, float resistance)
{
    static const bool enabled = [] {
        const char *v = std::getenv("LCS_PHYSICS_TIMESTEP");
        return !v || *v != '0';
    }();
    return enabled ? resistance * frameScale(rdram) : resistance;
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
        g_origMoveForce = runtime.lookupFunction(kApplyMoveForce);
        const bool move = g_origMoveForce && runtime.replaceFunction(kApplyMoveForce, &applyMoveForce);
        std::fprintf(stderr, "[physics-timing] player vehicle push scaling: %s\n", move && ok ? "on" : "incomplete hooks");
        return ok && move;
    }
}
