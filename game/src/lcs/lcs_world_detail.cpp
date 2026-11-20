// [lcs] Draw distance: the LOD scale, as the reference port's vcs_world_detail.cpp does it (docs/game-internals.md 6o
// there; the user's ask 2026-11-18: the setting has to really take hold).
//
// CCamera::Process (0x28FAC8, this = TheCamera 0x43BBF0) computes LODDistMultiplier at TheCamera+0x170: 70 / FOV
// (0x291A8C..0x291AC0), times 1 + (z - 55) / 60 above z 55, at most 2.0 (0x291BB4..0x291BCC); copies it into
// GenerationDistMultiplier (+0x174, the spawners' distance) at 0x291C00 and then multiplies +0x170 by the float at
// 0x3D7578 (0x291C08 / 0x291C14) - 1.0 in the executable, written nowhere: LCS's counterpart of VC's
// CRenderer::ms_lodDistScale. Every model's LOD distances and the entity visibility test scale with +0x170, so writing
// 0x3D7578 right before CCamera::Process runs moves where the detailed models switch to LODs and where objects, peds
// and cars stop being drawn, without changing where traffic spawns. The far plane (and so the fog) is left alone.
//
// 1.0 = the original game. LCS_DRAW_DISTANCE=<factor> sets the start value (0.5 .. 3.0); LCS_WORLD_DETAIL=0: no hook.
//
// World distance (the user's ask 2026-11-18: buildings, roads, traffic lights, lamps - the LOD scale moved only peds
// and cars). Those are the world sectors' geometry, cut at the camera's far plane, and in LCS that plane follows the
// timecycle (as in GTA III, unlike VCS's fixed 2000): the frame's render 0x1F5BC0 sets far = *(*0x3D4AE8 + 0x2B8C)
// through 0x1006A0 (camera +0x7C) at 0x1F5DD8 and the fog start = +0x2B90 through 0x100748 (+0x80) at 0x1F5DF0, on
// the camera *0x3D9920; CCamera::Process copies near / far to 0x3D9E20 / 0x3D9E24 for IsSphereVisible 0x298D28. The
// VU projection has a second, fixed far: 0x100120 calls 0x29D790(tan, aspect, near = +0x78, far = 2000 from
// `lui 0x44FA` at 0x100348). So the two timecycle calls get the factor (indoors 0x281E90 and the init 0x2FACF0 not),
// and the projection's 2000 is raised to the camera's far when that is beyond it - only with a factor above 1.0, at
// 1.0 every call passes as the original. LCS_WORLD_DISTANCE=<factor> (0.5 .. 3.0) sets the start value.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs_world
{
    namespace
    {
        constexpr uint32_t kCameraProcess = 0x0028FAC8u;
        constexpr uint32_t kCamera = 0x0043BBF0u;
        constexpr uint32_t kLodDist = kCamera + 0x170u; // LODDistMultiplier (after the scale)
        constexpr uint32_t kGenDist = kCamera + 0x174u; // GenerationDistMultiplier
        constexpr uint32_t kLodScale = 0x003D7578u;     // float 1.0: LODDistMultiplier's last factor
        constexpr uint32_t kMainHeap = 0x00406D50u;     // +0 size, +0x14 used (lcs_bootstrap.cpp [lcs-heap])
        constexpr uint32_t kStreamHeap = 0x00634140u;

        constexpr uint32_t kSetFar = 0x001006A0u, kSetFog = 0x00100748u, kProjection = 0x0029D790u;
        constexpr uint32_t kFarFromTimecycle = 0x001F5DE0u, kFogFromTimecycle = 0x001F5DF8u; // the calls' return
        constexpr uint32_t kProjectionFromBegin = 0x00100358u;
        PS2Runtime::RecompiledFunction g_origSetFar = nullptr, g_origSetFog = nullptr, g_origProjection = nullptr;
        std::atomic<float> g_worldScale{1.0f};
        std::atomic<float> g_tcFar{0.0f}, g_appliedFar{0.0f}, g_tcFog{0.0f};

        PS2_GUEST_CC void setFar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSetFar && GPR_U32(ctx, 31) == kFarFromTimecycle)
            {
                const float tc = ctx->f[12];
                const float s = g_worldScale.load(std::memory_order_relaxed);
                if (s != 1.0f)
                    ctx->f[12] = tc * s;
                g_tcFar.store(tc, std::memory_order_relaxed);
                g_appliedFar.store(ctx->f[12], std::memory_order_relaxed);
            }
            g_origSetFar(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void setFog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kSetFog && GPR_U32(ctx, 31) == kFogFromTimecycle)
            {
                g_tcFog.store(ctx->f[12], std::memory_order_relaxed);
                const float s = g_worldScale.load(std::memory_order_relaxed);
                if (s != 1.0f)
                    ctx->f[12] *= s;
            }
            g_origSetFog(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void projection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kProjection && GPR_U32(ctx, 31) == kProjectionFromBegin &&
                g_worldScale.load(std::memory_order_relaxed) > 1.0f)
            {
                float far;
                std::memcpy(&far, rdram + ((GPR_U32(ctx, 17) + 0x7Cu) & (PS2_RAM_MASK & ~3u)), 4); // $s1 = the camera
                if (far > ctx->f[15])
                    ctx->f[15] = far;
            }
            g_origProjection(rdram, ctx, runtime);
        }

        // [lcs] LCS_WORLD_DUMP=1, F10 (main.cpp): the whole guest RAM to logs\world_<n>.bin (32 MB) and a line with the
        // camera and the world object *0x3CEF78 to logs\world_<n>.txt - written on the game thread after CCamera::Process,
        // so the world's sector lists (0x2007B0 walks W+0x24 / W+0x54, area W+0x4C) can be read offline
        std::atomic<bool> g_dumpWanted{false};
        float rdf(const uint8_t *ram, uint32_t a);
        uint32_t rd32(const uint8_t *ram, uint32_t a);
        void dumpWorld(const uint8_t *ram)
        {
            static int n = 0;
            ++n;
            char path[64];
            std::snprintf(path, sizeof(path), "logs\\world_%d.bin", n);
            if (FILE *f = std::fopen(path, "wb"))
            {
                std::fwrite(ram, 1, PS2_RAM_SIZE, f);
                std::fclose(f);
            }
            std::snprintf(path, sizeof(path), "logs\\world_%d.txt", n);
            if (FILE *f = std::fopen(path, "w"))
            {
                const uint32_t w = rd32(ram, 0x003CEF78u);
                std::fprintf(f, "camera %.2f %.2f %.2f forward %.3f %.3f %.3f world %08x area(+0x4C) %d far %.1f\n",
                             rdf(ram, kCamera + 0x30u), rdf(ram, kCamera + 0x34u), rdf(ram, kCamera + 0x38u),
                             rdf(ram, kCamera + 0x10u), rdf(ram, kCamera + 0x14u), rdf(ram, kCamera + 0x18u), w,
                             static_cast<int>(rd32(ram, (w & (PS2_RAM_MASK & ~3u)) + 0x4Cu)), rdf(ram, 0x003D9E24u));
                std::fclose(f);
            }
            std::fprintf(stderr, "[world] RAM dump %d written (logs\\world_%d.bin / .txt)\n", n, n);
        }

        PS2Runtime::RecompiledFunction g_orig = nullptr;
        std::atomic<float> g_scale{1.0f};
        std::atomic<float> g_lod{0.0f}, g_gen{0.0f};
        std::atomic<uint8_t *> g_ram{nullptr};
        bool g_installed = false;

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

        // [lcs] LCS_CUTSCENE_TRACE=1 (off otherwise, also in the diag build): once a second - host ms, game ms (CTimer 0x3D9B70), frames, the step (0x3D9B8C) and the unclipped
        // one (0x3D9D9C), the CCam's mode (+0x40) and its spline clock (+0xC4 ms, advanced by CCam::Process_FlyBy
        // 0x27CB58 by the unclipped step x 20) against its end (+0x44): which of the clocks runs ahead of real time.
        double cutsceneTrace(const uint8_t *ram) // the report's host ms, 0 when none
        {
            static const bool on = [] {
                const char *v = std::getenv("LCS_CUTSCENE_TRACE");
                return v && v[0] && v[0] != '0';
            }();
            if (!on)
                return 0.0;
            using Clock = std::chrono::steady_clock;
            const uint32_t cam = kCamera + 0x220u + ram[kCamera + 0x10Cu] * 0x2B0u;
            uint16_t mode;
            std::memcpy(&mode, ram + cam + 0x40u, 2);
            static Clock::time_point at{};
            static uint32_t ms0 = 0, frames = 0;
            static float spline0 = 0.0f;
            ++frames;
            const auto now = Clock::now();
            const uint32_t ms = rd32(ram, 0x3D9B70u);
            const float spline = rdf(ram, cam + 0xC4u);
            if (at == Clock::time_point{})
            {
                at = now;
                ms0 = ms;
                spline0 = spline;
                frames = 0;
                return 0.0;
            }
            const double hostMs = std::chrono::duration<double, std::milli>(now - at).count();
            if (hostMs < 1000.0)
                return 0.0;
            std::fprintf(stderr,
                         "[cutscene] host %.0f ms: game %+d ms, %u frames (%.0f fps), step %.3f unclipped %.3f | cam mode %u "
                         "spline %.0f -> %.0f ms (%+.0f, end %u) | flag 0x3D5AB8 %u\n",
                         hostMs, static_cast<int>(ms - ms0), frames, frames * 1000.0 / hostMs, rdf(ram, 0x3D9B8Cu),
                         rdf(ram, 0x3D9D9Cu), mode, spline0, spline, spline - spline0, rd32(ram, cam + 0x44u),
                         rd32(ram, 0x3D5AB8u));
            at = now;
            ms0 = ms;
            spline0 = spline;
            frames = 0;
            return hostMs;
        }

        // [lcs] LCS_CUTSCENE_TRACE=1 also: the animation updates. CEntity::UpdateAnim 0x3067C0 (entity a0; objects get
        // the unclipped step x 0.02, peds the step x 0.02) -> RpAnimBlendClumpUpdateAnimations 0x13FA48 (clump a0, delta
        // f12 in seconds). Once a second: the callers of UpdateAnim (ra) with their counts, and the clumps with the most
        // animation time - more than ~1 s per real second, or more calls than frames, is an animation running fast.
        constexpr uint32_t kUpdateAnim = 0x003067C0u;
        constexpr uint32_t kClumpUpdate = 0x0013FA48u;
        PS2Runtime::RecompiledFunction g_origUpdateAnim = nullptr, g_origClumpUpdate = nullptr;
        struct ClumpStat
        {
            uint32_t clump = 0, calls = 0;
            float sum = 0, maxDelta = 0;
        };
        ClumpStat g_clumps[64];
        struct CallerStat
        {
            uint32_t ra = 0, calls = 0;
        };
        CallerStat g_callers[16];
        uint32_t g_animFrames = 0;
        // the entities UpdateAnim saw this frame (matrix: right +0, forward +0x10, up +0x20, position +0x30)
        uint32_t g_entities[64];
        int g_entityCount = 0;
        // at each whole second of the cutscene camera's spline clock: every animated entity's position and forward -
        // the same moment of the cutscene at any frame rate, so two runs (30 / 180 fps) can be compared line by line
        void entityReport(const uint8_t *ram, float splineMs)
        {
            static int lastSecond = -1;
            const int second = static_cast<int>(splineMs / 1000.0f);
            if (splineMs <= 0.0f)
            {
                lastSecond = -1;
                return;
            }
            if (second == lastSecond)
                return;
            lastSecond = second;
            std::fprintf(stderr, "[cutscene-pos] spline %.0f ms:", splineMs);
            for (int i = 0; i < g_entityCount; ++i)
            {
                const uint32_t e = g_entities[i] & (PS2_RAM_MASK & ~0xFu);
                if (!e)
                    continue;
                std::fprintf(stderr, " %06x (%.2f %.2f %.2f / %.2f %.2f)", e, rdf(ram, e + 0x30u), rdf(ram, e + 0x34u),
                             rdf(ram, e + 0x38u), rdf(ram, e + 0x10u), rdf(ram, e + 0x14u));
            }
            std::fprintf(stderr, "\n");
        }

        PS2_GUEST_CC void updateAnim(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kUpdateAnim)
            {
                const uint32_t ent = GPR_U32(ctx, 4);
                bool known = false;
                for (int i = 0; i < g_entityCount && !known; ++i)
                    known = g_entities[i] == ent;
                if (!known && g_entityCount < 64)
                    g_entities[g_entityCount++] = ent;
                const uint32_t ra = GPR_U32(ctx, 31);
                for (auto &c : g_callers)
                    if (c.ra == ra || c.ra == 0)
                    {
                        c.ra = ra;
                        ++c.calls;
                        break;
                    }
            }
            g_origUpdateAnim(rdram, ctx, runtime);
        }
        PS2_GUEST_CC void clumpUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kClumpUpdate)
            {
                const uint32_t clump = GPR_U32(ctx, 4);
                const float d = ctx->f[12];
                for (auto &c : g_clumps)
                    if (c.clump == clump || c.clump == 0)
                    {
                        c.clump = clump;
                        ++c.calls;
                        c.sum += d;
                        c.maxDelta = std::max(c.maxDelta, d);
                        break;
                    }
            }
            g_origClumpUpdate(rdram, ctx, runtime);
        }
        void animReport(double hostMs)
        {
            std::fprintf(stderr, "[cutscene-anim] %u frames; UpdateAnim callers:", g_animFrames);
            for (auto &c : g_callers)
                if (c.ra)
                    std::fprintf(stderr, " %08x x%u", c.ra, c.calls);
            std::fprintf(stderr, "\n");
            ClumpStat top[64];
            std::memcpy(top, g_clumps, sizeof(top));
            std::sort(std::begin(top), std::end(top), [](const ClumpStat &a, const ClumpStat &b) { return a.sum > b.sum; });
            int used = 0;
            for (const auto &c : g_clumps)
                used += c.clump != 0;
            std::fprintf(stderr, "[cutscene-anim] %d clumps animated; most anim time per %.0f ms:", used, hostMs);
            for (int i = 0; i < 6 && top[i].clump; ++i)
                std::fprintf(stderr, " %06x %.3f s in %u calls (max %.4f)", top[i].clump, top[i].sum, top[i].calls, top[i].maxDelta);
            std::fprintf(stderr, "\n");
            std::memset(g_clumps, 0, sizeof(g_clumps));
            std::memset(g_callers, 0, sizeof(g_callers));
            g_animFrames = 0;
        }

        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kCameraProcess)
                return g_orig(rdram, ctx, runtime);
            const float s = g_scale.load(std::memory_order_relaxed);
            std::memcpy(rdram + kLodScale, &s, 4);
            g_orig(rdram, ctx, runtime);
            if (g_dumpWanted.exchange(false, std::memory_order_acq_rel))
                dumpWorld(rdram);
            ++g_animFrames;
            if (const double ms = cutsceneTrace(rdram); ms > 0.0 && g_origClumpUpdate)
                animReport(ms);
            if (g_origClumpUpdate)
            {
                const uint32_t cam = kCamera + 0x220u + rdram[kCamera + 0x10Cu] * 0x2B0u;
                uint16_t mode;
                std::memcpy(&mode, rdram + cam + 0x40u, 2);
                entityReport(rdram, mode == 17u ? rdf(rdram, cam + 0xC4u) : 0.0f);
                g_entityCount = 0;
            }
            g_lod.store(rdf(rdram, kLodDist), std::memory_order_relaxed);
            g_gen.store(rdf(rdram, kGenDist), std::memory_order_relaxed);
            g_ram.store(rdram, std::memory_order_relaxed);
        }
    }

    void requestWorldDump()
    {
        static const bool on = [] {
            const char *v = std::getenv("LCS_WORLD_DUMP");
            return v && v[0] && v[0] != '0';
        }();
        if (on)
            g_dumpWanted.store(true, std::memory_order_release);
    }

    void setWorldDistance(float scale)
    {
        scale = std::clamp(scale, 0.5f, 3.0f);
        if (scale != g_worldScale.exchange(scale, std::memory_order_relaxed))
            std::fprintf(stderr, "[world] world distance %.2fx (far plane and fog)\n", scale);
    }
    float worldDistance() { return g_worldScale.load(std::memory_order_relaxed); }
    bool worldDistanceInstalled() { return g_origSetFar != nullptr; }
    float timecycleFar() { return g_tcFar.load(std::memory_order_relaxed); }
    float appliedFar() { return g_appliedFar.load(std::memory_order_relaxed); }
    float timecycleFog() { return g_tcFog.load(std::memory_order_relaxed); }

    void setDrawDistance(float scale)
    {
        scale = std::clamp(scale, 0.5f, 3.0f);
        if (scale != g_scale.exchange(scale, std::memory_order_relaxed))
            std::fprintf(stderr, "[world] draw distance %.2fx\n", scale);
    }
    float drawDistance() { return g_scale.load(std::memory_order_relaxed); }
    bool installed() { return g_installed; }
    // the camera's LOD and generation multipliers of the last frame (0 before the first)
    float lodMultiplier() { return g_lod.load(std::memory_order_relaxed); }
    float generationMultiplier() { return g_gen.load(std::memory_order_relaxed); }
    // the two game heaps: false before the first frame
    bool heapUse(int which, uint32_t &used, uint32_t &size)
    {
        const uint8_t *ram = g_ram.load(std::memory_order_relaxed);
        if (!ram)
            return false;
        const uint32_t h = which == 0 ? kMainHeap : kStreamHeap;
        size = rd32(ram, h);
        used = rd32(ram, h + 0x14u);
        return size != 0;
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_WORLD_DETAIL"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[world] off (LCS_WORLD_DETAIL=0): draw distance as on the PS2\n");
            return;
        }
        if (const char *e = std::getenv("LCS_DRAW_DISTANCE"); e && *e)
            g_scale = std::clamp(static_cast<float>(std::atof(e)), 0.5f, 3.0f);
        g_orig = runtime.lookupFunction(kCameraProcess);
        if (!g_orig)
        {
            std::fprintf(stderr, "[world] CCamera::Process 0x28FAC8 missing: draw distance not installed\n");
            return;
        }
        runtime.replaceFunction(kCameraProcess, &cameraProcess);
        if (const char *e = std::getenv("LCS_WORLD_DISTANCE"); e && *e)
            g_worldScale = std::clamp(static_cast<float>(std::atof(e)), 0.5f, 3.0f);
        g_origSetFar = runtime.lookupFunction(kSetFar);
        g_origSetFog = runtime.lookupFunction(kSetFog);
        g_origProjection = runtime.lookupFunction(kProjection);
        if (g_origSetFar && g_origSetFog && g_origProjection)
        {
            runtime.replaceFunction(kSetFar, &setFar);
            runtime.replaceFunction(kSetFog, &setFog);
            runtime.replaceFunction(kProjection, &projection);
            std::fprintf(stderr, "[world] world distance hooks: far 0x1006A0 / fog 0x100748 from the timecycle, projection "
                                 "0x29D790 (start %.2fx)\n", g_worldScale.load());
        }
        else
        {
            g_origSetFar = nullptr;
            std::fprintf(stderr, "[world] far / fog / projection functions missing: world distance not installed\n");
        }
        g_installed = true;
        if (const char *v = std::getenv("LCS_CUTSCENE_TRACE"); v && v[0] && v[0] != '0')
        {
            g_origUpdateAnim = runtime.lookupFunction(kUpdateAnim);
            g_origClumpUpdate = runtime.lookupFunction(kClumpUpdate);
            if (g_origUpdateAnim && g_origClumpUpdate)
            {
                runtime.replaceFunction(kUpdateAnim, &updateAnim);
                runtime.replaceFunction(kClumpUpdate, &clumpUpdate);
                std::fprintf(stderr, "[cutscene] trace on: timers, camera spline, animation updates (0x3067C0 / 0x13FA48)\n");
            }
            else
                g_origClumpUpdate = nullptr;
        }
        std::fprintf(stderr, "[world] draw distance hook on CCamera::Process (scale 0x3D7578, start %.2fx)\n",
                     g_scale.load());
    }
}
