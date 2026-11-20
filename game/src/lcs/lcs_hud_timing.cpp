// [lcs] The HUD's flashing on the game clock, not the frame count - the port of the reference port's
// src/game/ref_framerate.cpp twoDPass to LCS (the user's report 2026-11-18: the wanted stars flashed far too fast at
// 60 / 120 / uncapped when they are gained or cool down).
//
// CHud::Draw (0x23F438) decides what flashes by CTimer::m_FrameCounter (0x3D9B6C; CTimer::Update 0x2B4CF8 adds 1 each
// frame): `counter & 4` / `& 8` (seven tests, at 0x240894 .. 0x241E40 - the wanted stars, low health, the money's
// change, ...). On the PS2 that counter moved 30 times a second; at 120 fps four times as fast. While CHud::Draw runs it
// now sees a counter whose low six bits move 30 times a second of game time (CTimer::m_snTimeInMilliseconds 0x3D9B70
// * 3 / 100) - the high bits stay, so any frame-stamp compare there is off by at most 63; the real counter is put back
// when it returns. At 30 fps the flashing looks as before. LCS_HUD_FLASH=0: off.
//
// The six-star loop in CHud::Draw reads its own real-time blink phase (0x240B28 and 0x240B64).
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs_hud_timing
{
    namespace
    {
        constexpr uint32_t kHudDraw = 0x001F6338u; // the 2D pass (calls CHud::Draw 0x23F438, the wanted stars 0x23B060)
        constexpr uint32_t kFrameCounter = 0x003D9B6Cu;
        constexpr uint32_t kTimeMs = 0x003D9B70u;

        PS2Runtime::RecompiledFunction g_orig = nullptr;

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + a, 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + a, &v, 4); }

        PS2_GUEST_CC void hudDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kHudDraw)
                return g_orig(rdram, ctx, runtime);
            const uint32_t real = rd32(rdram, kFrameCounter);
            const uint32_t at30 = static_cast<uint32_t>(static_cast<uint64_t>(rd32(rdram, kTimeMs)) * 3u / 100u);
            wr32(rdram, kFrameCounter, (real & ~0x3Fu) | (at30 & 0x3Fu));
            g_orig(rdram, ctx, runtime);
            wr32(rdram, kFrameCounter, real);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_HUD_FLASH"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[hud-timing] off (LCS_HUD_FLASH=0)\n");
            return;
        }
        g_orig = runtime.lookupFunction(kHudDraw);
        if (!g_orig)
        {
            std::fprintf(stderr, "[hud-timing] the 2D pass 0x1F6338 missing: not installed\n");
            return;
        }
        runtime.replaceFunction(kHudDraw, &hudDraw);
        std::fprintf(stderr, "[hud-timing] the HUD flashes on the game clock (30 a second)\n");
    }
}

// Called directly by the two-state color test in the wanted HUD.
uint32_t lcsWantedBlinkPhase(uint32_t original)
{
    using Clock = std::chrono::steady_clock;
    static const bool enabled = [] {
        const char *value = std::getenv("LCS_HUD_FLASH");
        return !value || value[0] != '0';
    }();
    if (!enabled)
        return original & 1u;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    const auto tick = elapsed / 250;
    const uint32_t phase = static_cast<uint32_t>(tick & 1);
    static const bool trace = std::getenv("LCS_WANTED_BLINK_LOG") != nullptr;
    if (trace)
    {
        static int64_t lastTick = -1;
        if (tick != lastTick)
        {
            std::fprintf(stderr, "[wanted-blink] host %lld ms phase %u\n", static_cast<long long>(elapsed), phase);
            lastTick = tick;
        }
    }
    return phase;
}
