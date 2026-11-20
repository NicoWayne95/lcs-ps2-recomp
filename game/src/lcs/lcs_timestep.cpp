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
#include <cmath>
#include <unordered_map>

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
        bool g_payback = true;
        bool g_effectTick30 = false;
        float g_effectShare30 = 1.0f;
        float g_cloudPhaseRemainder = 0.0f;

        uint32_t effectFrame30(uint32_t milliseconds)
        {
            return static_cast<uint32_t>((uint64_t(milliseconds) * 30u + 29u) / 1000u);
        }

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
            // Capture the clock before the timer advances instead of using its internal
            // capped-step baseline. All emitters and particles share this decision.
            const uint32_t before = rd32(rdram, kTimeMs);
            g_orig(rdram, ctx, runtime);
            const uint32_t after = rd32(rdram, kTimeMs);
            g_effectTick30 = after > before && effectFrame30(after) != effectFrame30(before);
            const uint32_t elapsed = after >= before ? after - before : 0u;
            g_effectShare30 = static_cast<float>(elapsed) * (30.0f / 1000.0f);
            if (after < before)
                g_cloudPhaseRemainder = 0.0f;
            if (!g_payback)
                return;
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
        const char *payback = std::getenv("LCS_STEP_PAYBACK");
        g_payback = !(payback && payback[0] == '0') && minStep() < 0.5f;
        g_effectTick30 = false;
        g_effectShare30 = 1.0f;
        g_cloudPhaseRemainder = 0.0f;
        g_lead = 0.0f;
        g_orig = runtime.lookupFunction(kTimerUpdate);
        if (!g_orig)
        {
            std::fprintf(stderr, "[timestep] CTimer::Update 0x2B4CF8 missing: floor excess not paid back\n");
            return;
        }
        if (!runtime.replaceFunction(kTimerUpdate, &timerUpdate))
        {
            std::fprintf(stderr, "[timestep] could not install the timer hook\n");
            return;
        }
        std::fprintf(stderr, "[timestep] 30 Hz effects clock active; step payback %s\n", g_payback ? "on" : "off");
    }
}

// Quantize the game clock for effects with discrete 30 FPS updates.
uint32_t lcsEffectFrame30(const uint8_t *rdram)
{
    return lcs_timestep::effectFrame30(lcs_timestep::rd32(rdram, lcs_timestep::kTimeMs));
}

// CEntity::UpdateAnim supplies a floored simulation step to gameplay peds.
// Use the elapsed game clock while the player is stationary and on foot.
float lcsPlayerIdleDelta(const uint8_t *rdram, uint32_t entity, float delta)
{
    using namespace lcs_timestep;
    const uint32_t playerInfo = 0x00408E40u + rdram[0x003D9BC8u] * 0x190u;
    if (entity < 0x00100000u || entity > PS2_RAM_SIZE - 0x539u ||
        entity != rd32(rdram, playerInfo) || rdram[entity + 0x538u] || rdram[kReplayMode] == 1u ||
        !(delta > 0.0f) || delta >= 0.031f)
        return delta;

    const float vx = rdf(rdram, entity + 0x70u);
    const float vy = rdf(rdram, entity + 0x74u);
    const float vz = rdf(rdram, entity + 0x78u);
    if (vx * vx + vy * vy + vz * vz >= 0.000025f)
        return delta;

    const float corrected = g_effectShare30 / 30.0f;
    if (!(corrected >= 0.0f && corrected <= 0.050f))
        return delta;
    static const bool trace = std::getenv("LCS_TRACE_IDLE_ANIM") != nullptr;
    if (trace)
    {
        static unsigned frames = 0;
        static float stockSum = 0, elapsedSum = 0;
        stockSum += delta;
        elapsedSum += corrected;
        if (++frames == 120)
        {
            std::fprintf(stderr, "[idle-anim] 120 updates: original %.3f s, game clock %.3f s\n", stockSum, elapsedSum);
            frames = 0;
            stockSum = elapsedSum = 0.0f;
        }
    }
    return corrected;
}

// Each board has its own phase in the eight-step scheduler. Remember the
// bucket per slot so a repeated render or a missed exact phase cannot change the rate.
bool lcsAdvanceSign(const uint8_t *rdram, uint32_t slot)
{
    static uint32_t lastBucket[12]{};
    if (slot >= 12)
        return false;
    const uint32_t bucket = (lcsEffectFrame30(rdram) + slot) / 8u + 1u;
    if (bucket == lastBucket[slot])
        return false;
    lastBucket[slot] = bucket;
    return true;
}

float lcsSignStep()
{
    // Render-side interpolated scroll still reads the floored simulation step.
    return lcs_timestep::g_effectShare30 * (50.0f / 30.0f);
}

bool lcsEffectTick30(const uint8_t *)
{
    return lcs_timestep::g_effectTick30;
}

float lcsSmokeExpansion(const uint8_t *rdram, float expansion)
{
    float step;
    std::memcpy(&step, rdram + lcs_timestep::kStep, sizeof(step));
    constexpr float ps2Step = 50.0f / 30.0f;
    return step >= 0.0f && step < ps2Step ? expansion * (step / ps2Step) : expansion;
}

namespace
{
    struct TyreSmokeAge
    {
        uint32_t born;
        uint32_t fadeSteps = 0;
    };

    // The guest reuses particle slots. Retain one entry per slot and reset it on creation.
    std::unordered_map<uint32_t, TyreSmokeAge> g_tyreSmoke;
}

void lcsStartTyreSmoke(const uint8_t *rdram, uint32_t particle)
{
    g_tyreSmoke.insert_or_assign(particle, TyreSmokeAge{lcs_timestep::rd32(rdram, lcs_timestep::kTimeMs)});
}

int lcsTyreSmokeFade(const uint8_t *rdram, uint32_t particle, int fade)
{
    const auto found = g_tyreSmoke.find(particle);
    if (found == g_tyreSmoke.end())
        return lcsEffectTick30(rdram) ? fade : 0;

    TyreSmokeAge &age = found->second;
    const uint32_t now = lcs_timestep::rd32(rdram, lcs_timestep::kTimeMs);
    if (now < age.born)
        age = {now};
    const uint32_t steps = 1u + lcs_timestep::effectFrame30(now - age.born);
    const uint32_t due = steps > age.fadeSteps ? steps - age.fadeSteps : 0u;
    age.fadeSteps = steps;
    return fade * static_cast<int>(std::min(due, 255u));
}

// Sample the actual retirement path to distinguish alpha fade from expiry or other removal.
void lcsTraceTyreSmokeRemoval(const uint8_t *rdram, uint32_t particle)
{
    static const bool trace = std::getenv("LCS_TRACE_TYRE_SMOKE") != nullptr;
    if (!trace)
        return;

    static unsigned retired = 0, expired = 0, transparent = 0, other = 0;
    const uint32_t now = lcs_timestep::rd32(rdram, lcs_timestep::kTimeMs);
    const uint32_t expiry = lcs_timestep::rd32(rdram, particle + 0x20u);
    if (expiry < now)
        ++expired;
    else if (rdram[particle + 0x2Du] == 0u)
        ++transparent;
    else
        ++other;

    if (++retired == 16u || retired % 128u == 0u)
    {
        const auto found = g_tyreSmoke.find(particle);
        const uint32_t lifetime = found != g_tyreSmoke.end() && now >= found->second.born ? now - found->second.born : 0u;
        std::fprintf(stderr, "[tyre-smoke] t=%u retired=%u expired=%u alpha-zero=%u other=%u last-life-ms=%u step=%.3f\n",
                     now, retired, expired, transparent, other, lifetime, lcs_timestep::rdf(rdram, lcs_timestep::kStep));
    }
}

// Scale only legacy per-frame animation terms. Existing time-based wave phases stay intact.
float lcsEffectShare30()
{
    return lcs_timestep::g_effectShare30;
}

float lcsCloudPhaseStep(float increment)
{
    // The guest stores the resulting cloud angle as an integer. Preserve sub-unit motion.
    float &remainder = lcs_timestep::g_cloudPhaseRemainder;
    const float accumulated = increment + remainder;
    const float whole = std::trunc(accumulated);
    remainder = accumulated - whole;
    return whole;
}
