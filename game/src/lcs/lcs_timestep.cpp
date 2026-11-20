// [lcs] CTimer's time step above 100 fps (the port of the reference port's src/game/ref_framerate.cpp minStep + its
// game/lcs/overrides.toml patches to LCS).
//
// CTimer::Update (0x2B4CF8) floors the step (CTimer::ms_fTimeStep at 0x3D9B8C, 1.0 = 1/50 s) at 0.5 = 1/100 s
// (`lui $at, 0x3F00` at 0x2B4EC0, unless a pause / cutscene flag is set): every frame above 100 fps got 10 ms of game
// time and the game ran fast (1.3x - 1.4x at 130 - 140 fps, user report 2026-11-16, once the EE charge shift let
// LCS go that fast). The floor is lifted to 50/240 (240 fps), as in the reference port.
//
// Below 0.5 the game has branches that were dead on the PS2 and are wrong, the same kinds the reference port patched; their 0.5
// becomes 0.001 (every real step takes the 1 / step path, steps >= 0.5 run exactly as before):
// - `v *= step < 0.5 ? 0.5 : 1 / step` (as the reference port FUN_001f1d38, ped animation movement):   0x1D5978 (into +0x4D8)
// - `v *= step < 0.5 ? 2 : 1 / step` (as the reference port FUN_0031ae38): CCutsceneObject::ProcessControl 0x2B7428, the
//   anim's root movement (+0x70) before `+0x210 += step * v`, position = +0x200 + +0x210: 0x2B75A8. Below the floor an
//   object got 2 * step of its movement - the opening's bus 57% of its path at 175 fps (83% at 120) while it turned on
//   time, into the building (the user's report 2026-11-18, measured with LCS_CUTSCENE_TRACE). Same switch as the ped one.
// - `x / (max(step, 0.5) * 0.008)` (as the reference port CBike::ProcessControl's lean):               0x127508, 0x157584, 0x15BBE0,
//   0x180D4C; `50 / (max(step, 0.5) * 4)`:                                               0x1D9388
// The generated code calls these two at those instructions (generated/ee: grep lcsTimeStep). Not touched: the 0.5
// at 0x15BE1C, 0x285320, 0x28854C, 0x2BCD78, 0x303268 (a plain factor there, not a floor).
//
// LCS_MIN_STEP=<x>: the floor (0.5 = the original game, both values then as on the PS2).
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    uint32_t floatBits(float f)
    {
        uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    float minStep()
    {
        static const float step = [] {
            const char *v = std::getenv("LCS_MIN_STEP");
            const float x = v && *v ? static_cast<float>(std::atof(v)) : static_cast<float>(50.0 / 240.0);
            return x > 0.0f && x <= 0.5f ? x : 0.5f;
        }();
        return step;
    }
}

uint32_t lcsTimeStepFloorBits()
{
    static const uint32_t bits = floatBits(minStep());
    return bits;
}

uint32_t lcsTimeStepLowBits()
{
    // LCS_STEP_PATCHES=0: the six branches keep their 0.5 while the floor stays lifted (A/B of the vehicle feel)
    static const bool patches = [] {
        const char *v = std::getenv("LCS_STEP_PATCHES");
        return !(v && v[0] == '0');
    }();
    static const uint32_t bits = minStep() >= 0.5f || !patches ? 0x3F000000u : 0x3A830000u; // 0.5 / ~0.001 (lui $at, 0x3A83)
    return bits;
}

// 0x1D5978 (ped animation movement) only: not part of LCS_STEP_PATCHES=0 (peds would walk at a fraction of their speed)
uint32_t lcsTimeStepPedBits()
{
    static const uint32_t bits = minStep() >= 0.5f ? 0x3F000000u : 0x3A830000u;
    return bits;
}

// COnscreenTimerEntry::Process (0x2E3FF0, the mission countdown / count-up on the HUD; called once a frame from
// CUserDisplay::Process 0x1FCF28) moves the script variable by `(int)(ms_fTimeStep * 20)` (cvt.w.s at 0x2E3E94,
// truncating): the fraction of a millisecond is lost every frame - 1% at 30 fps (as on the PS2), 4% at 120 / 160,
// 13.5% at 144 (6.94 -> 6 ms), the timer then runs slow. The generated code (0x2E401C, after the call) replaces the
// result with the frame's milliseconds carrying the fraction to the next frame (one timer entry, so one carry).
// LCS_TIMER_FRACTION=0: the original truncation.
int32_t lcsOnscreenTimerMs(float ms)
{
    static const bool on = [] {
        const char *v = std::getenv("LCS_TIMER_FRACTION");
        return !(v && v[0] == '0');
    }();
    static float carry = 0.0f;
    if (!on || !(ms >= 0.0f) || ms > 1000.0f)
        return static_cast<int32_t>(ms);
    const float total = ms + carry;
    const int32_t whole = static_cast<int32_t>(total);
    carry = total - static_cast<float>(whole);
    return whole;
}

// [lcs] The floor's excess paid back (the user's report 2026-11-18: uncapped, cutscene speech out of sync). CTimer's step
// comes from the emulated EE clock, which moves in bursts; at ~180 fps many frames see next to no time and get the
// floor (50/240), so the steps add up to more than the game clock (CTimer::m_snTimeInMilliseconds 0x3D9B70, exact):
// the opening cutscene's camera and animations ran up to 28% fast at 180 fps (LCS_CUTSCENE_TRACE), at 120 fps exact.
// After each CTimer::Update the steps' lead over the clock (sum of steps - sum of ms / 20) is kept and taken off the
// next frames' steps, never below the floor: in the long run the steps add up to the game clock again. Frames the
// clock does not move normally (0 or more than 60 ms, the replay mode 0x3D9C8C) leave the step and clear the lead.
// ms_fTimeStep 0x3D9B8C and its unclipped copy 0x3D9D9C are both written. LCS_STEP_PAYBACK=0: off.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdio>

namespace lcs_timestep
{
    namespace
    {
        constexpr uint32_t kTimerUpdate = 0x002B4CF8u;
        constexpr uint32_t kStep = 0x003D9B8Cu, kStepUnclipped = 0x003D9D9Cu;
        constexpr uint32_t kTimeMs = 0x003D9B70u, kPrevTimeMs = 0x003D9BC4u, kReplayMode = 0x003D9C8Cu;
        PS2Runtime::RecompiledFunction g_orig = nullptr;
        float g_lead = 0.0f;

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + a, 4);
            return v;
        }
        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + a, 4);
            return v;
        }

        PS2_GUEST_CC void timerUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kTimerUpdate)
                return g_orig(rdram, ctx, runtime);
            g_orig(rdram, ctx, runtime);
            const float step = rdf(rdram, kStep);
            const int32_t dms = static_cast<int32_t>(rd32(rdram, kTimeMs) - rd32(rdram, kPrevTimeMs));
            if (dms <= 0 || dms > 60 || rdram[kReplayMode] == 1u || !(step > 0.0f) || step > 3.0f)
            {
                g_lead = 0.0f;
                return;
            }
            const float floor = minStep();
            float paid = step;
            if (g_lead > 0.0f && step > floor)
                paid = std::max(floor, step - g_lead);
            g_lead = std::clamp(g_lead + paid - static_cast<float>(dms) / 20.0f, -3.0f, 3.0f);
            if (paid != step)
            {
                std::memcpy(rdram + kStep, &paid, 4);
                std::memcpy(rdram + kStepUnclipped, &paid, 4);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_STEP_PAYBACK"); (e && e[0] == '0') || minStep() >= 0.5f)
        {
            std::fprintf(stderr, "[timestep] floor excess not paid back (LCS_STEP_PAYBACK=0 or the PS2 floor)\n");
            return;
        }
        g_orig = runtime.lookupFunction(kTimerUpdate);
        if (!g_orig)
        {
            std::fprintf(stderr, "[timestep] CTimer::Update 0x2B4CF8 missing: floor excess not paid back\n");
            return;
        }
        runtime.replaceFunction(kTimerUpdate, &timerUpdate);
        std::fprintf(stderr, "[timestep] the floor's excess is paid back on the next frames (steps follow the game clock)\n");
    }
}
