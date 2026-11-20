// [lcs-hud] Widescreen HUD "Screen edges" for LCS - the port of the the reference port project's src/game/ref_hud.cpp anchoring
// (docs/SETTINGS_MENU_WIDESCREEN.md there): each HUD element keeps its 4:3 shape and stays at its own screen edge (the
// radar on the left, clock / weapon / money on the right, text in the middle) instead of the whole HUD squeezed
// towards the middle.
//
// LCS's 2D primitive emitter 0x2B1390 (called through 0x2B16D0 by the Im2D entries 0x2B1A10 / 0x2B1A70 / 0x2B1AD0 and by
// 0x2C9288) takes a1 = vertex count, a2 = 16-byte vertices (u, v u16; RGBA bytes; x, y s16 pixels of the 640x448
// frame; z u32), a3 = an optional u16 index list, t1 = 0 for those pixel positions (non-zero: GS coordinates, 0x2C9288's
// off-screen draws - not captured). Every captured primitive's vertex positions are kept for the frame; at the frame's
// chain kick they are grouped into elements (boxes closer than a word gap merge: a line of text, the clock / weapon /
// money cluster) and each element is anchored by where it sits on the 4:3 screen: the left or right edge when it is
// off to that side, the middle otherwise; full-width draws (fades, bars) stretch. The renderer asks classify() about
// each 2D primitive (gsHwSetHudClassifier) and squeezes it towards its anchor.
//
// Not yet as in the reference port: the radar and centred text are not known by who draws them (CHud::Draw, the radar and the font
// render are not found for LCS yet) - by position only. LCS_TRACE_HUDANCHOR=<frames>: the elements and their anchors.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/ps2_render_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace lcs_hud
{
    namespace
    {
        constexpr uint32_t kEmit2D = 0x002B1390u;
        PS2Runtime::RecompiledFunction g_origEmit = nullptr;
        constexpr uint32_t kRadarMap = 0x00115AE0u;
        PS2Runtime::RecompiledFunction g_origRadarMap = nullptr;
        bool g_drawingRadarMap = false; // game thread

        PS2_GUEST_CC void radarMap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool previous = g_drawingRadarMap;
            g_drawingRadarMap = ctx->pc == kRadarMap && GPR_U32(ctx, 31) == 0x00241E6Cu;
            g_origRadarMap(rdram, ctx, runtime);
            g_drawingRadarMap = previous;
        }
        std::atomic<bool> g_on{false};

        struct Prim
        {
            float x0, y0, x1, y1;
            uint32_t firstKey, keyCount;
            bool radarMap = false;
        };
        // this frame's primitives (game thread; the mutex for the chain observer, which may run on another thread)
        std::mutex g_mutex;
        std::vector<Prim> g_prims;
        std::vector<uint32_t> g_keys;
        struct AnchorMap : std::unordered_map<uint32_t, uint8_t>
        {
            bool sniper = false;
        };
        bool g_sniper = false; // captured with this frame's HUD primitives
        std::shared_ptr<const AnchorMap> g_anchors = std::make_shared<AnchorMap>(); // read on the GS stage
        int g_traceAnchors = 0;

        // [lcs] the HUD's z (CSprite2d::SetVertices 0x321068 writes *(u32 *)0x3DA1B0; the weapon icon gets it in
        // emit2D): only primitives at it are elements - the coronas / lights the emitter draws at their real depth were
        // anchored by position and shifted (user report 2026-11-16). LCS_HUD_EDGES_ZFILTER=0: every primitive as before.
        constexpr uint32_t kHudZ = 0x003DA1B0u;
        const bool g_zFilter = !(std::getenv("LCS_HUD_EDGES_ZFILTER") && std::getenv("LCS_HUD_EDGES_ZFILTER")[0] == '0');

        uint32_t positionKey(int x, int y) { return uint32_t(uint16_t(x)) | (uint32_t(uint16_t(y)) << 16); }

        void capture(const uint8_t *rdram, R5900Context *ctx)
        {
            if (!g_on.load(std::memory_order_relaxed) || GPR_U32(ctx, 9) != 0u) // t1: pixel positions only
                return;
            const uint32_t n = GPR_U32(ctx, 5), verts = GPR_U32(ctx, 6) & PS2_RAM_MASK, indices = GPR_U32(ctx, 7) & PS2_RAM_MASK;
            if (n == 0u || n > 1024u || verts + 16u > PS2_RAM_SIZE)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            constexpr uint32_t camera = 0x43BBF0u;
            const uint8_t activeCam = rdram[camera + 0x10Cu];
            if (activeCam < 3)
            {
                uint16_t mode;
                std::memcpy(&mode, rdram + camera + 0x220u + activeCam * 0x2B0u + 0x40u, sizeof(mode));
                g_sniper = mode == 7u;
            }
            if (g_prims.size() >= 8192u)
                return;
            Prim p{1e9f, 1e9f, -1e9f, -1e9f, static_cast<uint32_t>(g_keys.size()), n, g_drawingRadarMap};
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t index = i;
                if (indices)
                {
                    uint16_t k;
                    std::memcpy(&k, rdram + ((indices + i * 2u) & (PS2_RAM_MASK & ~1u)), 2);
                    index = k;
                }
                const uint32_t v = verts + index * 16u;
                if (v + 16u > PS2_RAM_SIZE)
                    return;
                int16_t xy[2];
                std::memcpy(xy, rdram + v + 8u, 4);
                if (g_zFilter && !p.radarMap)
                {
                    uint32_t z, hz;
                    std::memcpy(&z, rdram + v + 12u, 4);
                    std::memcpy(&hz, rdram + kHudZ, 4);
                    if (z != hz) // a projected sprite (corona, light) at its real depth: not HUD
                        return;
                }
                p.x0 = std::min<float>(p.x0, xy[0]);
                p.x1 = std::max<float>(p.x1, xy[0]);
                p.y0 = std::min<float>(p.y0, xy[1]);
                p.y1 = std::max<float>(p.y1, xy[1]);
                g_keys.push_back(positionKey(xy[0], xy[1]));
            }
            g_prims.push_back(p);
        }

        // [lcs-hud] the weapon icon: CHud::Draw (0x23F438..0x2437C8) draws it through CSprite::RenderOneXLUSprite (0x2E0498,
        // VC's: x y z w h in f12..f16, colour a0..a3) - with a real depth for z, not the HUD's far plane that CSprite2d
        // writes (*(u32 *)0x3DA1B0, CSprite2d::SetVertices 0x321068). The renderer takes the far plane for HUD, so the
        // icon kept its 4:3 position on the 16:9 picture while the fist beside it (CSprite2d) moved (user report). As
        // the reference port's ref_hud.cpp (its sprite routine 0x3a07a8): while that sprite is drawn for CHud::Draw, the 2D emitter
        // gives its vertices the HUD's z. LCS_HUD_SPRITE_Z=0: as before.
        constexpr uint32_t kXluSprite = 0x002E0498u, kHudDraw = 0x0023F438u, kHudDrawEnd = 0x002437C8u;
        PS2Runtime::RecompiledFunction g_origXluSprite = nullptr;
        bool g_hudSprite = false; // game thread

        PS2_GUEST_CC void xluSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool hud = ctx->pc == kXluSprite && ra >= kHudDraw && ra < kHudDrawEnd;
            const bool was = g_hudSprite;
            if (hud)
                g_hudSprite = true;
            g_origXluSprite(rdram, ctx, runtime);
            g_hudSprite = was;
        }

        void hudZ(uint8_t *rdram, R5900Context *ctx)
        {
            if (GPR_U32(ctx, 9) != 0u) // t1: pixel positions only
                return;
            const uint32_t n = GPR_U32(ctx, 5), verts = GPR_U32(ctx, 6) & PS2_RAM_MASK;
            if (n == 0u || n > 64u || verts + n * 16u > PS2_RAM_SIZE)
                return;
            uint32_t z;
            std::memcpy(&z, rdram + kHudZ, 4);
            for (uint32_t i = 0; i < n; ++i) // the sprite's vertices (an index list names some of these)
                std::memcpy(rdram + verts + i * 16u + 12u, &z, 4);
        }

        PS2_GUEST_CC void emit2D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kEmit2D)
            {
                if (g_hudSprite)
                    hudZ(rdram, ctx);
                capture(rdram, ctx);
            }
            g_origEmit(rdram, ctx, runtime);
        }

        // as ref_hud.cpp resolveAnchors, without its known parts (radar, centred text, menus)
        std::shared_ptr<const AnchorMap> resolveAnchors()
        {
            auto anchors = std::make_shared<AnchorMap>();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                anchors->sniper = g_sniper;
                g_sniper = false;
                const uint32_t n = static_cast<uint32_t>(g_prims.size());
                std::vector<uint32_t> parent(n), order(n);
                std::iota(parent.begin(), parent.end(), 0u);
                std::iota(order.begin(), order.end(), 0u);
                auto find = [&](uint32_t i) {
                    while (parent[i] != i)
                        i = parent[i] = parent[parent[i]];
                    return i;
                };
                auto full = [](const Prim &p) { return p.x0 <= 1.0f && p.x1 >= 639.0f; };
                // big primitives (backgrounds, map tiles, screen-wide boxes) are elements of their own: grouping them
                // would pull everything near them along
                auto apart = [&](const Prim &p) { return p.radarMap || full(p) || p.x1 - p.x0 > 320.0f; };
                constexpr float gapX = 12.0f, gapY = 3.0f;
                std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_prims[a].x0 < g_prims[b].x0; });
                for (uint32_t oi = 0; oi < n; ++oi)
                {
                    const Prim &a = g_prims[order[oi]];
                    if (apart(a))
                        continue;
                    for (uint32_t oj = oi + 1; oj < n && g_prims[order[oj]].x0 <= a.x1 + gapX; ++oj)
                    {
                        const Prim &b = g_prims[order[oj]];
                        if (apart(b) || b.y0 > a.y1 + gapY || a.y0 > b.y1 + gapY)
                            continue;
                        parent[find(order[oj])] = find(order[oi]);
                    }
                }
                std::vector<Prim> boxes(n, Prim{1e9f, 1e9f, -1e9f, -1e9f, 0u, 0u});
                for (uint32_t i = 0; i < n; ++i)
                {
                    Prim &b = boxes[find(i)];
                    b.x0 = std::min(b.x0, g_prims[i].x0);
                    b.x1 = std::max(b.x1, g_prims[i].x1);
                    b.y0 = std::min(b.y0, g_prims[i].y0);
                    b.y1 = std::max(b.y1, g_prims[i].y1);
                    b.keyCount += 1;
                }
                // (from LCS_TRACE_HUDANCHOR_DELAY seconds on, default 60: in play, not the loading screens)
                static const auto traceStart = std::chrono::steady_clock::now();
                static const int traceDelay = std::getenv("LCS_TRACE_HUDANCHOR_DELAY") ? std::atoi(std::getenv("LCS_TRACE_HUDANCHOR_DELAY")) : 60;
                const bool trace = g_traceAnchors > 0 && std::chrono::steady_clock::now() - traceStart >= std::chrono::seconds(traceDelay);
                if (trace)
                    std::fprintf(stderr, "[hud-anchor] frame (%d left), %u primitives\n", --g_traceAnchors, n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const Prim &p = g_prims[i];
                    const Prim &b = boxes[find(i)];
                    const float centre = (b.x0 + b.x1) * 0.5f;
                    const uint8_t anchor = p.radarMap                           ? GSHwAnchorStretch
                                           : full(p) || full(b)                       ? GSHwAnchorStretch
                                           : centre < 280.0f && b.x0 < 160.0f       ? GSHwAnchorLeft
                                           : centre > 360.0f && b.x1 > 480.0f       ? GSHwAnchorRight
                                                                                    : GSHwAnchorCentre;
                    if (!p.radarMap)
                        for (uint32_t k = 0; k < p.keyCount; ++k)
                            (*anchors)[g_keys[p.firstKey + k]] = anchor;
                    if (trace && find(i) == i)
                        std::fprintf(stderr, "[hud-anchor]   x %4.0f..%4.0f y %4.0f..%4.0f prims %3u -> %s\n", b.x0, b.x1, b.y0, b.y1,
                                     b.keyCount,
                                     anchor == GSHwAnchorStretch ? "stretch"
                                     : anchor == GSHwAnchorLeft  ? "left"
                                     : anchor == GSHwAnchorRight ? "right"
                                                                 : "centre");
                }
                // A radar tile can share vertex coordinates with a different HUD element.
                // Apply the explicit radar anchor after the positional assignments.
                for (const Prim &p : g_prims)
                    if (p.radarMap)
                        for (uint32_t k = 0; k < p.keyCount; ++k)
                            (*anchors)[g_keys[p.firstKey + k]] = GSHwAnchorStretch;
                g_prims.clear();
                g_keys.clear();
            }
            return anchors;
        }

        // The frame's anchors belong to the frame's chain: kept by the chain's key and handed to the GS stage when the
        // render worker starts that chain (in order with the GS packets it makes of it). Posted behind the queued chain
        // instead, they reached the GS one frame late - everything that moves (the radar map, its blips, text) missed
        // its anchor, fell back to the squeeze around the middle and split from the rest (the radar came apart).
        std::mutex g_pendingMutex;
        std::unordered_map<const void *, std::shared_ptr<const AnchorMap>> g_pending;

        void apply(std::shared_ptr<const AnchorMap> anchors)
        {
            if (ps2_render_worker::enabled())
                ps2_render_worker::runOnGsStage([anchors] { g_anchors = anchors; });
            else
                g_anchors = std::move(anchors);
        }

        // the frame's display list is queued: its HUD primitives are complete (small chains - texture uploads between
        // the 2D draws - are left alone: the frame's chain is megabytes)
        void queued(const void *key, uint32_t sizeBytes)
        {
            if (!g_on.load(std::memory_order_relaxed) || sizeBytes < (256u << 10))
                return;
            auto anchors = resolveAnchors();
            std::lock_guard<std::mutex> lock(g_pendingMutex);
            if (g_pending.size() > 16u) // chains never processed (dropped frames)
                g_pending.clear();
            g_pending[key] = std::move(anchors);
        }
        void processing(const void *key, bool begin)
        {
            if (!begin)
                return;
            std::shared_ptr<const AnchorMap> anchors;
            {
                std::lock_guard<std::mutex> lock(g_pendingMutex);
                const auto it = g_pending.find(key);
                if (it == g_pending.end())
                    return;
                anchors = std::move(it->second);
                g_pending.erase(it);
            }
            apply(std::move(anchors));
        }

        // gsHwSetHudClassifier: a primitive whose every vertex is a captured 2D vertex of a kicked frame takes its anchor
        uint8_t classify(const float *xy, uint32_t count)
        {
            const AnchorMap &anchors = *g_anchors;
            // The scope is drawn in pieces that meet at the screen centre.
            // Keep them in the same full-frame space regardless of their individual bounds.
            if (anchors.sniper && count >= 3)
            {
                float x0 = xy[0], x1 = x0, y0 = xy[1], y1 = y0;
                for (uint32_t i = 1; i < count; ++i)
                {
                    x0 = std::min(x0, xy[2 * i]);
                    x1 = std::max(x1, xy[2 * i]);
                    y0 = std::min(y0, xy[2 * i + 1]);
                    y1 = std::max(y1, xy[2 * i + 1]);
                }
                if (x1 - x0 >= 120.0f && y1 - y0 >= 100.0f &&
                    x0 <= 321.0f && x1 >= 319.0f && y0 <= 225.0f && y1 >= 223.0f)
                    return GSHwAnchorStretch;
            }
            if (anchors.empty())
                return GSHwAnchorNone;
            uint8_t anchor = GSHwAnchorNone;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float x = xy[2 * i], y = xy[2 * i + 1];
                const int xs[2] = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(x + 0.5f))};
                const int ys[2] = {static_cast<int>(std::lround(y)), static_cast<int>(std::lround(y + 0.5f))};
                auto it = anchors.end();
                for (int k = 0; k < 4 && it == anchors.end(); ++k)
                    it = anchors.find(positionKey(xs[k & 1], ys[k >> 1]));
                if (it == anchors.end())
                    return GSHwAnchorNone;
                anchor = it->second;
            }
            return anchor;
        }
    }

    // the emitter hook and the chain observer (once; before the game runs)
    void install(PS2Runtime &runtime)
    {
        g_origRadarMap = runtime.lookupFunction(kRadarMap);
        if (g_origRadarMap)
            runtime.replaceFunction(kRadarMap, &radarMap);
        // always hooked (the video page switches "Screen edges" live, and the weapon icon's z is for every HUD layout);
        // the per-element capture runs only while setEdges(true)
        if (!(std::getenv("LCS_HUD_SPRITE_Z") && std::getenv("LCS_HUD_SPRITE_Z")[0] == '0'))
        {
            g_origXluSprite = runtime.lookupFunction(kXluSprite);
            if (g_origXluSprite)
                runtime.replaceFunction(kXluSprite, &xluSprite);
            std::fprintf(stderr, "[lcs-hud] weapon icon at the HUD's z: %s\n", g_origXluSprite ? "on" : "RenderOneXLUSprite 0x2E0498 missing");
        }
        g_origEmit = runtime.lookupFunction(kEmit2D);
        if (!g_origEmit)
        {
            std::fprintf(stderr, "[lcs-hud] 2D emitter 0x2B1390 missing: no per-element HUD\n");
            return;
        }
        runtime.replaceFunction(kEmit2D, &emit2D);
        ps2chain::addChainObservers(&queued, &processing);
        if (const char *t = std::getenv("LCS_TRACE_HUDANCHOR"))
            g_traceAnchors = std::atoi(t);
    }

    // widescreen "Screen edges" on / off (off: the renderer's squeeze around the middle alone)
    void setEdges(bool on)
    {
        if (!g_origEmit) // not installed (LCS_HUD is not "edges"): the HUD as before, nothing to switch
            return;
        g_on.store(on, std::memory_order_relaxed);
        gsHwSetHudClassifier(on ? &classify : nullptr);
        std::fprintf(stderr, "[lcs-hud] HUD at the screen edges %s\n", on ? "on" : "off");
    }
}
