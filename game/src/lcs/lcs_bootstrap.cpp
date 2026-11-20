#include "lcs_bootstrap.h"
#include "lcs_movie.h"
#include "game_overrides.h"
#include "game/lcs_rsl_world.h"
#include "debug/profiler.h"
#include "lcs_rpc_diag.h"
#include "lcs_diag.h"
#include "lcs_prompts.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "ps2_stubs.h"
#include "ps2x/iop/cdvd_debug.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_present.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_perf.h"
#include "platform/platform.h"
#include <filesystem>
#include <iostream>
#include <cstring>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <share.h>
#include <unordered_map>
#include <cstdlib>
#include <string_view>
#include <algorithm>
#include <vector>

void ps2xReportGuestWrites(uint32_t address, const char *what); // [lcs-write] ps2_iop_host.cpp
namespace lcs_hud { void install(PS2Runtime &runtime); } // [lcs-hud] src/lcs/lcs_hud.cpp
namespace lcs_freecam { void install(PS2Runtime &runtime); } // [lcs-freecam] src/lcs/lcs_freecam.cpp
namespace lcs_menu { void install(PS2Runtime &runtime); void frame(uint8_t *ram, bool paused); } // [lcs-menu] src/lcs/lcs_menu.cpp (experimental, LCS_MENU_VIDEO builds)
namespace lcs_inflate { bool install(PS2Runtime &runtime); } // [lcs-inflate] src/lcs/lcs_inflate.cpp
namespace lcs_physics_timing { bool install(PS2Runtime &runtime); } // [lcs] src/lcs/lcs_physics_timing.cpp
namespace lcs_script_timing { void install(PS2Runtime &runtime); } // [lcs] src/lcs/lcs_script_timing.cpp
#include "lcs/lcs_world_detail.h"
namespace lcs_hud_timing { void install(PS2Runtime &runtime); } // [lcs] src/lcs/lcs_hud_timing.cpp
namespace lcs_timestep { void install(PS2Runtime &runtime); } // [lcs] src/lcs/lcs_timestep.cpp
namespace lcs_input { void install(PS2Runtime &runtime); } // [lcs-input] src/lcs/lcs_input.cpp (menu build)
namespace lcs_stream // [lcs-stream] src/lcs/lcs_stream.cpp
{
    bool install(PS2Runtime &runtime);
    bool readFor(uint32_t addr, uint32_t &dest, uint32_t &lsn, uint32_t &bytes);
    bool readDiscCopy(uint32_t lsn, uint32_t bytes, uint8_t *dst);
}
extern void (*g_ps2xAddressReport)(uint32_t, const char *); // [lcs-free] ps2_runtime.cpp: reportMissingFunction
namespace lcs_bootstrap {
void reportFreed(uint32_t addr, const char *what); // [lcs-free] below
namespace {
bool enabled = false,fastIntroTest=false,nativeBootMovie=false;
PS2Runtime::RecompiledFunction originalAccelerate = nullptr, originalBrake = nullptr;
PS2Runtime::RecompiledFunction originalListForAll = nullptr;

// [lcs-free] every free of the game heap (0x150E98, a0 = block): where, by whom, in which game frame - so a pointer a
// detector finds broken can be traced to the free that left it dangling. Block size from the header word at -8 (the
// large-block path; small pool blocks show their pool word there instead).
struct FreedBlock
{
    uint32_t ptr, header, ra, callerRa, frame, ms;
};
constexpr uint32_t kFreedRing = 1u << 18;
FreedBlock *freedRing = nullptr;
std::atomic<uint64_t> freedCount{0};
// [lcs-alloc] every allocation through 0x150478 (a0 heap, a1 size; v0 block): which block, how big, by whom, when -
// so a stale reference (vif garbage) can be named by the block it pointed into and the IOP write that landed on it.
// ra 0x203698: the sector display-list cache (0x2035D8 copies a built list into the block).
struct AllocBlock
{
    uint32_t ptr, size, ra, callerRa, frame, ms;
};
constexpr uint32_t kAllocRing = 1u << 18;
AllocBlock *allocRing = nullptr;
std::atomic<uint64_t> allocCount{0}, allocUnseen{0};
PS2Runtime::RecompiledFunction originalHeapAlloc = nullptr, originalHeapAllocAligned = nullptr;
const auto traceStart = std::chrono::steady_clock::now();
uint32_t traceMs()
{
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - traceStart).count());
}
uint32_t stackCaller(const uint8_t *ram, uint32_t sp, uint32_t ra)
{
    for (uint32_t j = 0; j < 0x40u; j += 4u)
    {
        uint32_t v;
        std::memcpy(&v, ram + ((sp + j) & (PS2_RAM_MASK & ~3u)), 4);
        if (v >= 0x100000u && v < 0x3A0000u && (v & 3u) == 0u && v != ra)
            return v;
    }
    return 0u;
}
// 0x150478 (a0 heap, a1 size) and the aligned 0x1505D8 (a1 size, a2 alignment, a3 heap): both take the size in a1
template <uint32_t Entry>
PS2_GUEST_CC void recordHeapAlloc(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool entry = ctx->pc == Entry;
    const uint32_t size = getRegU32(ctx, 5), ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
    (Entry == 0x150478u ? originalHeapAlloc : originalHeapAllocAligned)(ram, ctx, runtime);
    if (!entry || !allocRing)
        return;
    if (ctx->pc != ra) // left before its return (a resumption elsewhere): not seen
    {
        allocUnseen.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t p = getRegU32(ctx, 2);
    if (!p)
        return;
    const uint64_t n = allocCount.fetch_add(1, std::memory_order_relaxed);
    allocRing[n & (kAllocRing - 1u)] = {p, size, ra, stackCaller(ram, sp, ra), static_cast<uint32_t>(guestFramesPresented()),
                                        traceMs()};
}
// [lcs-model] 0x18DEE8(file, model index): converts a streamed model file ("mdl") in place (relocation). Recorded with
// a hash per 4 KB page of the file right after it returns, so a later stale or broken model (vif garbage in its
// packets) shows whether the file changed after its conversion - and the block is dumped once next to the disc's
// bytes it was read from (model_<addr>_ram.bin / _disc.bin in the run folder).
struct ModelConversion
{
    uint32_t file, size, index, frame, ms, ra;
    bool done;
    uint64_t pages[64];
};
constexpr uint32_t kConversions = 1024u;
ModelConversion *conversions = nullptr;
std::atomic<uint64_t> conversionCount{0};
PS2Runtime::RecompiledFunction originalModelConvert = nullptr;
uint64_t pageHash(const uint8_t *ram, uint32_t from, uint32_t to)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t a = from; a < to; ++a)
        h = (h ^ ram[a & PS2_RAM_MASK]) * 0x100000001B3ull;
    return h;
}
PS2_GUEST_CC void recordModelConvert(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const bool entry = ctx->pc == 0x18DEE8u;
    const uint32_t file = getRegU32(ctx, 4) & PS2_RAM_MASK, index = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
    originalModelConvert(ram, ctx, runtime);
    if (!entry || !conversions)
        return;
    uint32_t dest = 0, lsn = 0, bytes = 0;
    uint32_t size = lcs_stream::readFor(file, dest, lsn, bytes) ? dest + bytes - file : 0x10000u;
    size = std::min<uint32_t>(size, 64u * 4096u);
    const uint64_t n = conversionCount.fetch_add(1, std::memory_order_relaxed);
    ModelConversion &c = conversions[n % kConversions];
    c.file = file;
    c.size = size;
    c.index = index;
    c.frame = static_cast<uint32_t>(guestFramesPresented());
    c.ms = traceMs();
    c.ra = ra;
    c.done = ctx->pc == ra; // left before its return: the hashes are of a half-converted file
    for (uint32_t p = 0; p < 64u; ++p)
        c.pages[p] = p * 4096u < size ? pageHash(ram, file + p * 4096u, file + std::min(size, (p + 1u) * 4096u)) : 0u;
}
void reportConverted(const uint8_t *ram, uint32_t addr, const char *what)
{
    if (!conversions || !ram)
        return;
    const uint32_t a = addr & PS2_RAM_MASK, now = traceMs();
    const uint64_t n = conversionCount.load(std::memory_order_relaxed);
    const uint64_t first = n > kConversions ? n - kConversions : 0u;
    for (uint64_t i = n; i-- > first;)
    {
        const ModelConversion &c = conversions[i % kConversions];
        if (a < c.file || a >= c.file + c.size)
            continue;
        uint32_t changed = 0, firstChanged = ~0u;
        for (uint32_t p = 0; p * 4096u < c.size && p < 64u; ++p)
            if (pageHash(ram, c.file + p * 4096u, c.file + std::min(c.size, (p + 1u) * 4096u)) != c.pages[p])
            {
                ++changed;
                firstChanged = std::min(firstChanged, p);
            }
        std::fprintf(stderr, "[lcs-model] %s %08x: in model file %08x (+%x, %u bytes) index %u converted at frame %u, %u ms ago, "
                             "by ra %08x%s | %u of %u pages changed since%s",
                     what, addr, c.file, a - c.file, c.size, c.index, c.frame, now - c.ms, c.ra,
                     c.done ? "" : " (left before its return: half converted)", changed, (c.size + 4095u) / 4096u,
                     changed ? "" : " (unchanged: broken at its conversion)");
        if (changed)
            std::fprintf(stderr, ", first at +%x", firstChanged * 4096u);
        std::fprintf(stderr, "\n");
        // the block and the disc bytes it came from, once per file (at most 4)
        static uint32_t dumped[4]{};
        static int dumps = 0;
        bool seen = false;
        for (int k = 0; k < dumps; ++k)
            seen |= dumped[k] == c.file;
        if (!seen && dumps < 4)
        {
            dumped[dumps++] = c.file;
            char name[64];
            std::snprintf(name, sizeof(name), "model_%08x_ram.bin", c.file);
            if (std::FILE *f = std::fopen(name, "wb"))
            {
                std::fwrite(ram + c.file, 1u, c.size, f);
                std::fclose(f);
            }
            uint32_t dest = 0, lsn = 0, bytes = 0;
            if (lcs_stream::readFor(c.file, dest, lsn, bytes))
            {
                std::vector<uint8_t> disc(bytes);
                if (lcs_stream::readDiscCopy(lsn, bytes, disc.data()))
                {
                    std::snprintf(name, sizeof(name), "model_%08x_disc.bin", c.file);
                    if (std::FILE *f = std::fopen(name, "wb"))
                    {
                        std::fwrite(disc.data(), 1u, disc.size(), f);
                        std::fclose(f);
                    }
                    // words that differ between the disc and RAM (the relocated pointers, and whatever else changed)
                    uint32_t differ = 0;
                    const uint32_t off = c.file - dest;
                    std::fprintf(stderr, "[lcs-model]   read from LSN 0x%x (%u bytes to %08x); dumped as model_%08x_ram.bin / _disc.bin; differing words:",
                                 lsn, bytes, dest, c.file);
                    for (uint32_t w = 0; w + 4u <= c.size && off + w + 4u <= bytes; w += 4u)
                    {
                        uint32_t x, y;
                        std::memcpy(&x, ram + c.file + w, 4u);
                        std::memcpy(&y, disc.data() + off + w, 4u);
                        if (x != y && ++differ <= 12u)
                            std::fprintf(stderr, " +%x:%08x>%08x", w, y, x);
                    }
                    std::fprintf(stderr, " (%u in all)\n", differ);
                }
            }
            else
                std::fprintf(stderr, "[lcs-model]   no native read of it remembered (LCS_NATIVE_STREAM=0?)\n");
        }
        return;
    }
}
// the most recent allocations whose block holds addr, newest first; and whether it was freed after that
void reportAllocated(uint32_t addr, const char *what)
{
    if (!allocRing)
        return;
    const uint64_t n = allocCount.load(std::memory_order_relaxed);
    const uint64_t first = n > kAllocRing ? n - kAllocRing : 0u;
    const uint32_t a = addr & 0x1FFFFFFFu, now = traceMs();
    int hits = 0;
    for (uint64_t i = n; i-- > first && hits < 4;)
    {
        const AllocBlock &b = allocRing[i & (kAllocRing - 1u)];
        const uint32_t p = b.ptr & 0x1FFFFFFFu;
        if (a < p || a >= p + std::max<uint32_t>(b.size, 1u))
            continue;
        ++hits;
        // a free of this block after this allocation?
        const uint64_t fn = freedCount.load(std::memory_order_relaxed);
        const uint64_t ffirst = fn > kFreedRing ? fn - kFreedRing : 0u;
        const FreedBlock *freed = nullptr;
        for (uint64_t k = fn; freedRing && k-- > ffirst;)
        {
            const FreedBlock &f = freedRing[k & (kFreedRing - 1u)];
            if (f.ms < b.ms)
                break;
            if ((f.ptr & 0x1FFFFFFFu) == p)
            {
                freed = &f;
                break;
            }
        }
        std::fprintf(stderr, "[lcs-alloc] %s %08x: in block %08x size %u (+%x) allocated at frame %u, %u ms ago, by ra %08x (caller %08x)%s",
                     what, addr, b.ptr, b.size, a - p, b.frame, now - b.ms, b.ra, b.callerRa,
                     b.ra == 0x203698u ? " = sector list cache" : "");
        if (freed)
            std::fprintf(stderr, " | freed at frame %u, %u ms ago, by ra %08x (caller %08x)\n", freed->frame, now - freed->ms,
                         freed->ra, freed->callerRa);
        else
            std::fprintf(stderr, " | not freed since\n");
    }
    if (!hits)
        std::fprintf(stderr, "[lcs-alloc] %s %08x: no allocation through 0x150478 / 0x1505D8 holds it among the last %llu (%llu not seen)\n",
                     what, addr, (unsigned long long)(n - first),
                     (unsigned long long)allocUnseen.load(std::memory_order_relaxed));
}
PS2Runtime::RecompiledFunction originalHeapFree = nullptr, originalHeapFreeIn = nullptr;
uint8_t *freedRam = nullptr;
// [lcs-heap] the heaps the frees go to (0x150740, a0 = heap); +0x14 is the bytes in use (a free subtracts the block size)
std::atomic<uint32_t> heapsSeen[8]{};
void reportHeaps(const uint8_t *ram)
{
    for (auto &h : heapsSeen)
    {
        const uint32_t heap = h.load(std::memory_order_relaxed);
        if (!heap)
            continue;
        uint32_t w[10];
        std::memcpy(w, ram + (heap & (PS2_RAM_MASK & ~0xFu)), sizeof(w));
        std::fprintf(stderr, "[lcs-heap] frame %llu heap %08x used %u (%.1f MB) | +0..0x24: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
                     guestFramesPresented(), heap, w[5], w[5] / 1048576.0, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
    }
    std::fflush(stderr);
}
// [lcs-oom] the allocator's out-of-memory callbacks (heap +4: 0x1F3318 main, 0x1F3190 second), a0 heap, a1 size asked
PS2Runtime::RecompiledFunction originalOomMain = nullptr, originalOomSecond = nullptr;
template <bool Main>
PS2_GUEST_CC void traceOom(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == (Main ? 0x1F3318u : 0x1F3190u))
    {
        static int logged = 0;
        if (logged++ < 64)
        {
            const uint32_t heap = getRegU32(ctx, 4), size = getRegU32(ctx, 5), sp = getRegU32(ctx, 29);
            uint32_t w[6];
            std::memcpy(w, ram + (heap & (PS2_RAM_MASK & ~0xFu)), sizeof(w));
            std::fprintf(stderr, "[lcs-oom] frame %llu %s heap %08x: asked %u bytes (%.1f KB), used %u of %u (%.2f MB free) | ra %08x stack:",
                         guestFramesPresented(), Main ? "main" : "second", heap, size, size / 1024.0, w[5], w[0],
                         (w[0] > w[5] ? w[0] - w[5] : 0u) / 1048576.0, getRegU32(ctx, 31));
            for (uint32_t j = 0; j < 0x80u; j += 4u)
            {
                uint32_t v;
                std::memcpy(&v, ram + ((sp + j) & (PS2_RAM_MASK & ~3u)), 4);
                if (v >= 0x100000u && v < 0x3A0000u && (v & 3u) == 0u)
                    std::fprintf(stderr, " %08x", v);
            }
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
        }
    }
    (Main ? originalOomMain : originalOomSecond)(ram, ctx, runtime);
}
PS2_GUEST_CC void recordHeapFreeIn(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == 0x150740u)
    {
        const uint32_t heap = getRegU32(ctx, 4);
        freedRam = ram; // [lcs-heap] for heapStats (the overlay)
        // [lcs-free] every free lands here (a1 = block): 0x150E98 / 0x150EC8 and ~30 game functions call it directly,
        // the streaming code at 0x18F108 / 0x18F648 among them (run_175135: block 01eff780 allocated twice, no free
        // seen through 0x150E98). ra = the direct caller; for the 0x150E98 / 0x150EC8 wrappers the stack's next one.
        const uint32_t block = getRegU32(ctx, 5);
        if (block && freedRing)
        {
            auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4); return v; };
            const uint32_t ra = getRegU32(ctx, 31);
            const uint64_t n = freedCount.fetch_add(1, std::memory_order_relaxed);
            freedRing[n & (kFreedRing - 1u)] = {block, word(block - 8u), ra, stackCaller(ram, getRegU32(ctx, 29), ra),
                                                static_cast<uint32_t>(guestFramesPresented()), traceMs()};
        }
        for (auto &h : heapsSeen)
        {
            uint32_t cur = h.load(std::memory_order_relaxed);
            if (cur == heap)
                break;
            if (cur == 0u && h.compare_exchange_strong(cur, heap))
                break;
        }
        static auto at = std::chrono::steady_clock::now();
        static const bool heapLog = lcs_diag::wanted("LCS_HEAP_LOG"); // release: only with LCS_HEAP_LOG=1
        if (heapLog && std::chrono::steady_clock::now() - at >= std::chrono::seconds(10))
        {
            at = std::chrono::steady_clock::now();
            reportHeaps(ram);
        }
    }
    originalHeapFreeIn(ram, ctx, runtime);
}
PS2_GUEST_CC void recordHeapFree(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t p = getRegU32(ctx, 4);
    if (ctx->pc == 0x150E98u && p != 0u && freedRing)
    {
        auto word = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4); return v; };
        // (the free itself is recorded in 0x150740, which this calls; here only the bad-header check)
        const uint32_t callerRa = stackCaller(ram, getRegU32(ctx, 29), getRegU32(ctx, 31));
        // [lcs-free] a free whose block header is no size (a double free, or a pointer that was never a heap block):
        // the heap subtracts it from its bytes in use (run_115919: main heap "used" jumped to 1.8 GB at a respawn)
        const uint32_t header = word(p - 8u);
        static int badLogged = 0;
        // only a large block has its size at -8: one of a heap's large-block area (+0x0C start .. +0x10 the small pool);
        // small pool blocks (above +0x10) have the neighbour's data there (run_121750: false reports)
        bool largeBlock = false;
        for (auto &h : heapsSeen)
        {
            const uint32_t heap = h.load(std::memory_order_relaxed);
            if (heap && p >= word(heap + 0x0Cu) && p < word(heap + 0x10u))
                largeBlock = true;
        }
        if (largeBlock && header >= 0x80000000u && badLogged < 32) // a negative size: the free adds it to the bytes in use
        {
            ++badLogged;
            std::fprintf(stderr, "[lcs-free] BAD free %08x: header %08x | frame %u ra %08x caller %08x | block+0..0x20: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                         p, header, static_cast<uint32_t>(guestFramesPresented()), getRegU32(ctx, 31), callerRa, word(p), word(p + 4u),
                         word(p + 8u), word(p + 12u), word(p + 16u), word(p + 20u), word(p + 24u), word(p + 28u));
            reportFreed(p, "earlier free of the same block");
        }
    }
    originalHeapFree(ram, ctx, runtime);
}
} // namespace
// [lcs-heap] the overlay's heap lines: +0x00 the heap's size, +0x14 the bytes in use
int heapStats(unsigned (*out)[3], int max)
{
    const uint8_t *ram = freedRam;
    if (!ram)
        return 0;
    int n = 0;
    for (auto &h : heapsSeen)
    {
        const uint32_t heap = h.load(std::memory_order_relaxed);
        if (!heap || n >= max)
            continue;
        uint32_t w[6];
        std::memcpy(w, ram + (heap & (PS2_RAM_MASK & ~0xFu)), sizeof(w));
        out[n][0] = heap;
        out[n][1] = w[5];
        out[n][2] = w[0];
        ++n;
    }
    return n;
}
// the most recent frees whose block holds addr (or starts within 0x40 after it), newest first
void reportFreed(uint32_t addr, const char *what)
{
    if (addr == 0u)
        return;
    ps2xReportGuestWrites(addr, what); // [lcs-write] the IOP data that last landed there
    if (!freedRing)
        return;
    const uint64_t n = freedCount.load(std::memory_order_relaxed);
    const uint64_t first = n > kFreedRing ? n - kFreedRing : 0u;
    int hits = 0;
    const uint32_t a = addr & 0x1FFFFFFFu;
    for (uint64_t i = n; i-- > first && hits < 4;)
    {
        const FreedBlock &f = freedRing[i & (kFreedRing - 1u)];
        const uint32_t p = f.ptr & 0x1FFFFFFFu;
        const uint32_t size = f.header > 0u && f.header < 0x400000u ? f.header : 0x40u;
        if (a + 0x40u >= p && a < p + size)
        {
            ++hits;
            std::fprintf(stderr, "[lcs-free] %s %08x: in block %08x (header %08x) freed at frame %u by ra %08x (caller %08x), %llu frees ago\n",
                         what, addr, f.ptr, f.header, f.frame, f.ra, f.callerRa, (unsigned long long)(n - 1u - i));
        }
    }
    if (!hits)
        std::fprintf(stderr, "[lcs-free] %s %08x: no free of it among the last %llu\n", what, addr,
                     (unsigned long long)(n - first));
    reportAllocated(addr, what); // [lcs-alloc]
    reportConverted(freedRam, addr, what); // [lcs-model]
    std::fflush(stderr);
}
namespace {

// [lcs-ring] 0x2BB338: the same kind of walk (head at a0 + 8, entry = node - 8; a frame's object list - run_213627 and
// run_121434 hung in it after a respawn). Report a ring or a bad link once per list and caller, then run the original.
PS2Runtime::RecompiledFunction originalFrameForAll = nullptr;
// [lcs-ring] 0x207B98(object): before freeing the object it clears every entry of its list at +0x90 (next at +0x94,
// entry +4 = 0). run_181507 hung in that loop (pc 0x207C20, ra 0x1E0FE4) - a list that runs in a ring. Report the
// ring once per object (max 12): the entries, and for the first repeated one and the object who allocated / freed /
// wrote them. The original runs unchanged afterwards (the hang stays visible).
PS2Runtime::RecompiledFunction originalObjectListClear = nullptr;
PS2_GUEST_CC void checkObjectListClear(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == 0x207B98u)
    {
        auto w32 = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4); return v; };
        const uint32_t object = getRegU32(ctx, 4);
        uint32_t slow = w32(object + 0x90u), fast = slow, steps = 0;
        bool ring = false;
        while (fast && steps < 1000000u)
        {
            fast = w32(fast + 0x94u);
            if (!fast)
                break;
            fast = w32(fast + 0x94u);
            slow = w32(slow + 0x94u);
            ++steps;
            if (fast && fast == slow)
            {
                ring = true;
                break;
            }
        }
        static int reports = 0;
        if (ring && reports < 12)
        {
            ++reports;
            std::fprintf(stderr, "[lcs-ring] list +0x90 of object %08x runs in a ring (ra %08x, caller %08x, frame %llu) | object+0x88..0xA4:",
                         object, getRegU32(ctx, 31), stackCaller(ram, getRegU32(ctx, 29), getRegU32(ctx, 31)),
                         (unsigned long long)guestFramesPresented());
            for (uint32_t o = 0x88u; o <= 0xA4u; o += 4u)
                std::fprintf(stderr, " %08x", w32(object + o));
            std::fprintf(stderr, "\n");
            // the entries from the head until the first one seen again
            uint32_t node = w32(object + 0x90u), first = 0u;
            uint32_t seen[64]{};
            for (int i = 0; i < 64 && node; ++i)
            {
                for (int k = 0; k < i; ++k)
                    if (seen[k] == node)
                        first = node;
                if (first)
                    break;
                seen[i] = node;
                std::fprintf(stderr, "[lcs-ring]   #%d entry %08x: +0..0x10 %08x %08x %08x %08x | next (+0x94) %08x\n", i, node,
                             w32(node), w32(node + 4u), w32(node + 8u), w32(node + 12u), w32(node + 0x94u));
                node = w32(node + 0x94u);
            }
            reportFreed(object, "ring list owner");
            if (first)
                reportFreed(first, "first entry seen twice");
            std::fflush(stderr);
        }
    }
    originalObjectListClear(ram, ctx, runtime);
}

PS2_GUEST_CC void checkFrameForAll(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == 0x2BB338u)
    {
        auto w32 = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4); return v; };
        auto valid = [](uint32_t a) { return (a & 3u) == 0u && (a & 0x1FFFFFFFu) >= 0x100000u && (a & 0x1FFFFFFFu) < PS2_RAM_SIZE; };
        const uint32_t owner = getRegU32(ctx, 4), head = owner + 8u;
        uint32_t slow = w32(head), fast = slow;
        bool ring = false, bad = false;
        for (uint32_t i = 0; i < 1000000u; ++i)
        {
            if (fast == head || !valid(fast)) { bad = fast != head; break; }
            fast = w32(fast);
            if (fast == head || !valid(fast)) { bad = fast != head; break; }
            fast = w32(fast);
            slow = w32(slow);
            if (slow == fast) { ring = true; break; }
        }
        static int reports = 0;
        static uint64_t seen[12];
        const uint64_t key = (uint64_t{owner} << 32) | getRegU32(ctx, 31);
        bool repeat = false;
        for (int i = 0; i < reports; ++i)
            repeat |= seen[i] == key;
        if ((ring || bad) && !repeat && reports < 12)
        {
            seen[reports++] = key;
            std::fprintf(stderr, "[lcs-ring] list 0x2BB338 %s: owner 0x%08x head next %08x prev %08x | callback %08x data %08x | ra %08x | owner+0..0x20:",
                         ring ? "RING without the head" : "bad link", owner, w32(head), w32(head + 4u), getRegU32(ctx, 5), getRegU32(ctx, 6),
                         getRegU32(ctx, 31));
            for (uint32_t j = 0; j < 0x24u; j += 4u)
                std::fprintf(stderr, " %08x", w32(owner + j));
            std::fprintf(stderr, "\n");
            uint32_t node = w32(head), prev = head;
            uint32_t firstBad = 0u;
            for (int k = 0; k < 24 && valid(node) && node != head; ++k, prev = node, node = w32(node))
            {
                const bool backOk = w32(node + 4u) == prev;
                if (!backOk && !firstBad)
                    firstBad = node;
                std::fprintf(stderr, "[lcs-ring]   #%d node %08x next %08x prev %08x%s | entry %08x:", k, node, w32(node), w32(node + 4u),
                             backOk ? "" : " (prev mismatch)", node - 8u);
                for (uint32_t j = 0; j < 0x30u; j += 4u)
                    std::fprintf(stderr, " %08x", w32(node - 8u + j));
                std::fprintf(stderr, "\n");
            }
            reportFreed(owner, "frame list owner");
            if (firstBad)
                reportFreed(firstBad - 8u, "first entry whose prev does not point back");
            std::fflush(stderr);
        }
    }
    originalFrameForAll(ram, ctx, runtime);
}

// [lcs-ring] 0x31E820 walks an RW link list (head at a0 + 8, links next / prev, entry = node - 0x1C) with a callback
// (a1) until it is back at the head. A ring that no longer reaches the head loops forever (run_100050: the clump
// destroy 0x31E7A0 -> 0x31E778). Diagnosis only: report the list once per head, then run the original unchanged.
PS2_GUEST_CC void checkListForAll(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == 0x31E820u) // only on entry (not a resume inside the loop)
    {
        auto w32 = [ram](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4); return v; };
        auto valid = [](uint32_t a) { return (a & 3u) == 0u && (a & 0x1FFFFFFFu) >= 0x100000u && (a & 0x1FFFFFFFu) < PS2_RAM_SIZE; };
        const uint32_t owner = getRegU32(ctx, 4), head = owner + 8u;
        uint32_t slow = w32(head), fast = slow;
        bool ring = false, bad = false;
        for (uint32_t i = 0; i < 1000000u; ++i)
        {
            if (fast == head || !valid(fast)) { bad = fast != head; break; }
            fast = w32(fast);
            if (fast == head || !valid(fast)) { bad = fast != head; break; }
            fast = w32(fast);
            slow = w32(slow);
            if (slow == fast) { ring = true; break; }
        }
        static int reports = 0;
        // once per list and caller (a broken model clump is walked again at each clone), up to 12 different ones
        static uint64_t seen[12];
        const uint64_t key = (uint64_t{owner} << 32) | getRegU32(ctx, 31);
        bool repeat = false;
        for (int i = 0; i < reports; ++i)
            repeat |= seen[i] == key;
        if ((ring || bad) && !repeat && reports < 12)
        {
            seen[reports] = key;
            ++reports;
            std::fprintf(stderr,
                         "[lcs-ring] list 0x31E820 %s: owner 0x%08x head next %08x prev %08x | callback %08x data %08x | ra %08x "
                         "caller-ra(sp+8 of 0x31E7A0 frame) %08x | owner+0..0x20:",
                         ring ? "RING without the head" : "bad link", owner, w32(head), w32(head + 4u), getRegU32(ctx, 5),
                         getRegU32(ctx, 6), getRegU32(ctx, 31), w32(getRegU32(ctx, 29) + 8u));
            for (uint32_t j = 0; j < 0x24u; j += 4u)
                std::fprintf(stderr, " %08x", w32(owner + j));
            std::fprintf(stderr, "\n");
            uint32_t node = w32(head);
            for (int k = 0; k < 24 && valid(node) && node != head; ++k, node = w32(node))
            {
                std::fprintf(stderr, "[lcs-ring]   #%d node %08x next %08x prev %08x | entry %08x:", k, node, w32(node), w32(node + 4u),
                             node - 0x1Cu);
                for (uint32_t j = 0; j < 0x40u; j += 4u)
                    std::fprintf(stderr, " %08x", w32(node - 0x1Cu + j));
                std::fprintf(stderr, "\n");
            }
            // [lcs-free] the owner and the first entry that is no atomic of this owner: were they freed, and by whom
            reportFreed(owner, "list owner");
            node = w32(head);
            for (int k = 0; k < 4096 && node != head; ++k, node = w32(node))
                if (!valid(node) || w32(node - 0x1Cu + 0x18u) != owner)
                {
                    reportFreed(valid(node) ? node - 0x1Cu : node, "first foreign entry");
                    break;
                }
            std::fflush(stderr);
        }
    }
    originalListForAll(ram, ctx, runtime);
}

// [lcs-clump] who breaks a clump's atomic list: RpClumpAddAtomic 0x31E920 (a0 clump, a1 atomic), RpClumpRemoveAtomic
// 0x31E950 (same), RpAtomicDestroy 0x2E9110 (a0 atomic; it does not unlink - RpClumpDestroy's callback 0x31E778 is the
// one caller allowed to destroy an atomic still in its clump). Atomic: +0x18 clump, +0x1C link (next, prev); clump:
// +8 head. Diagnosis only: the originals run unchanged.
PS2Runtime::RecompiledFunction originalClumpAdd = nullptr, originalClumpRemove = nullptr, originalAtomicDestroy = nullptr;
int clumpReports = 0;
uint32_t ramWord(const uint8_t *ram, uint32_t a)
{
    uint32_t v;
    std::memcpy(&v, ram + (a & (PS2_RAM_MASK & ~3u)), 4);
    return v;
}
bool ramPointer(uint32_t a) { return (a & 3u) == 0u && (a & 0x1FFFFFFFu) >= 0x100000u && (a & 0x1FFFFFFFu) < PS2_RAM_SIZE; }
void clumpReport(const uint8_t *ram, R5900Context *ctx, const char *what, uint32_t clump, uint32_t atomic, uint32_t extra)
{
    if (clumpReports >= 400)
        return;
    ++clumpReports;
    std::fprintf(stderr, "[lcs-clump] %s: clump %08x atomic %08x (%08x) | ra %08x sp %08x stack-ra?", what, clump, atomic, extra,
                 getRegU32(ctx, 31), getRegU32(ctx, 29));
    for (uint32_t j = 0; j < 0x60u; j += 4u) // return addresses of the callers: words in the guest code range
    {
        const uint32_t v = ramWord(ram, getRegU32(ctx, 29) + j);
        if (v >= 0x100000u && v < 0x400000u && (v & 3u) == 0u)
            std::fprintf(stderr, " +%x:%08x", j, v);
    }
    std::fprintf(stderr, " | atomic+0..0x28:");
    for (uint32_t j = 0; j < 0x28u; j += 4u)
        std::fprintf(stderr, " %08x", ramWord(ram, atomic + j));
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}
// first bad link of a clump's atomic list, 0 when it is sound
uint32_t clumpListFault(const uint8_t *ram, uint32_t clump, uint32_t &where)
{
    const uint32_t head = clump + 8u;
    uint32_t prev = head, node = ramWord(ram, head);
    for (uint32_t i = 0; i < 4096u; ++i)
    {
        if (node == head)
            return ramWord(ram, head + 4u) == prev ? 0u : (where = head, 4u); // head.prev not the last node
        if (!ramPointer(node))
            return where = prev, 1u;                       // a next that is no pointer
        if (ramWord(ram, node + 4u) != prev)
            return where = node, 2u;                       // its prev is not the node before
        if (ramWord(ram, node - 0x1Cu + 0x18u) != clump)
            return where = node, 3u;                       // an atomic of another (or no) clump
        prev = node;
        node = ramWord(ram, node);
    }
    return where = node, 5u; // longer than 4096: a ring
}
const char *const clumpFaultKinds[] = {"", "bad next", "prev mismatch", "foreign atomic", "head prev", "ring"};
// the whole list once per clump (the first 8 clumps): head, then each node's next / prev and its atomic's clump field
void clumpDumpList(const uint8_t *ram, uint32_t clump)
{
    static uint32_t dumped[8];
    static int dumps = 0;
    for (int i = 0; i < dumps; ++i)
        if (dumped[i] == clump)
            return;
    if (dumps >= 8)
        return;
    dumped[dumps++] = clump;
    const uint32_t head = clump + 8u;
    std::fprintf(stderr, "[lcs-clump]   clump %08x +0..0x30:", clump);
    for (uint32_t j = 0; j < 0x30u; j += 4u)
        std::fprintf(stderr, " %08x", ramWord(ram, clump + j));
    std::fprintf(stderr, "\n[lcs-clump]   head %08x next %08x prev %08x\n", head, ramWord(ram, head), ramWord(ram, head + 4u));
    uint32_t node = ramWord(ram, head);
    for (int k = 0; k < 32 && node != head && ramPointer(node); ++k, node = ramWord(ram, node))
        std::fprintf(stderr, "[lcs-clump]   #%d node %08x next %08x prev %08x atomic %08x type %08x clump %08x\n", k, node,
                     ramWord(ram, node), ramWord(ram, node + 4u), node - 0x1Cu, ramWord(ram, node - 0x1Cu),
                     ramWord(ram, node - 0x1Cu + 0x18u));
    std::fflush(stderr);
}
void clumpCheckAfter(const uint8_t *ram, R5900Context *ctx, const char *what, uint32_t clump, uint32_t atomic)
{
    uint32_t where = 0;
    if (const uint32_t fault = clumpListFault(ram, clump, where))
    {
        char text[96];
        std::snprintf(text, sizeof(text), "list broken after %s (%s at %08x)", what, clumpFaultKinds[fault], where);
        clumpReport(ram, ctx, text, clump, atomic, where);
        clumpDumpList(ram, clump);
    }
}
PS2_GUEST_CC void checkClumpAdd(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t clump = getRegU32(ctx, 4), atomic = getRegU32(ctx, 5);
    const bool entry = ctx->pc == 0x31E920u;
    if (entry)
    {
        uint32_t where = 0;
        if (const uint32_t fault = clumpListFault(ram, clump, where))
        {
            char text[96];
            std::snprintf(text, sizeof(text), "add: list already broken before (%s at %08x)", clumpFaultKinds[fault], where);
            clumpReport(ram, ctx, text, clump, atomic, where);
            clumpDumpList(ram, clump);
        }
        if (const uint32_t owner = ramWord(ram, atomic + 0x18u))
            clumpReport(ram, ctx, "add: atomic still in a clump", clump, atomic, owner);
    }
    R5900Context saved = *ctx;
    originalClumpAdd(ram, ctx, runtime);
    if (entry)
        clumpCheckAfter(ram, &saved, "add", clump, atomic);
}
PS2_GUEST_CC void checkClumpRemove(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t clump = getRegU32(ctx, 4), atomic = getRegU32(ctx, 5);
    const bool entry = ctx->pc == 0x31E950u;
    if (entry)
    {
        const uint32_t owner = ramWord(ram, atomic + 0x18u), link = atomic + 0x1Cu;
        if (owner != clump)
            clumpReport(ram, ctx, "remove: atomic not in this clump", clump, atomic, owner);
        else if (ramWord(ram, ramWord(ram, link) + 4u) != link || ramWord(ram, ramWord(ram, link + 4u)) != link)
            clumpReport(ram, ctx, "remove: neighbours do not point back", clump, atomic, ramWord(ram, link));
    }
    R5900Context saved = *ctx;
    originalClumpRemove(ram, ctx, runtime);
    if (entry && ramPointer(clump))
        clumpCheckAfter(ram, &saved, "remove", clump, atomic);
}
PS2_GUEST_CC void checkAtomicDestroy(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == 0x2E9110u)
    {
        const uint32_t atomic = getRegU32(ctx, 4), owner = ramWord(ram, atomic + 0x18u);
        if (owner != 0u && getRegU32(ctx, 31) != 0x31E78Cu) // only RpClumpDestroy may destroy an atomic still in its clump
            clumpReport(ram, ctx, "destroy: atomic still in a clump", owner, atomic, owner);
    }
    originalAtomicDestroy(ram, ctx, runtime);
}

template <bool Brake>
PS2_GUEST_CC void readDrivingPedal(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime)
{
    constexpr uint32_t playerPad = 0x00408C60;
    const uint32_t pad = getRegU32(ctx, 4);
    uint8_t gas = 0, brake = 0;
    if (pad == playerPad && runtime->padBackend().drivingPedals(gas, brake))
    {
        const bool controlsDisabled = runtime->memory().read16(pad + 0x82) != 0;
        SET_GPR_U32(ctx, 2, controlsDisabled ? 0 : (Brake ? brake : gas));
        ctx->pc = getRegU32(ctx, 31);
        return;
    }
    (Brake ? originalBrake : originalAccelerate)(ram, ctx, runtime);
}
PS2Runtime::RecompiledFunction introTargets[4]{};
constexpr char introCameraHeader[] = "15,\r\n0.000000f,51.774010";
uint32_t introCameraStream = 0;
unsigned introCameraReads = 0;
PS2Runtime::RecompiledFunction introModelTargets[16]{};
unsigned introModelReads = 0;
constexpr uint32_t introTaxiId = 122;
uint32_t introTaxiSector = 0, introTaxiSectors = 0;
uint32_t introTaxiEntity = 0;
bool introTaxiListed = false, introTaxiPrepared = false;
bool introTaxiRenderCalled = false, introTaxiRenderSubmitted = false;
bool introTaxiGuardSampled = false;
uint32_t introTaxiLastGate = 0;
uint8_t introTaxiLastSceneFlag = 0;

uint32_t readIntroStreamWord(const uint8_t* ram, uint32_t stream, uint32_t offset)
{
    uint32_t word;
    std::memcpy(&word, ram + ((stream + offset) & PS2_RAM_MASK), sizeof(word));
    return word;
}

template <unsigned Index>
PS2_GUEST_CC void traceIntroModels(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime)
{
    const uint32_t caller = getRegU32(ctx, 31);
    const uint32_t first = getRegU32(ctx, 4);
    const uint32_t second = getRegU32(ctx, 5);
    const uint32_t stack = getRegU32(ctx, 29);
    if constexpr (Index < 2)
    {
        constexpr uint32_t callsites[] = {0x357268u, 0x3572A4u};
        if (caller == callsites[Index])
        {
            const uint32_t nameAddress = Index == 0 ? first : second;
            char name[33]{};
            for (size_t i = 0; i < sizeof(name) - 1; ++i)
            {
                name[i] = static_cast<char>(ram[(nameAddress + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
                if (!name[i]) break;
            }
            std::cerr << "[LCS cutscene model] stage=" << (Index == 0 ? "lookup" : "request-by-name")
                      << " name=" << name;
            if constexpr (Index == 1) std::cerr << " id=" << first;
            std::cerr << '\n';
            introModelTargets[Index](ram, ctx, runtime);
            if constexpr (Index == 0)
            {
                if (ctx->pc == caller)
                {
                    const bool found = getRegU32(ctx, 2) != 0;
                    std::cerr << "[LCS cutscene model] stage=lookup-result name=" << name << " found=" << found;
                    if (found) std::cerr << " id=" << readIntroStreamWord(ram, second, 0);
                    std::cerr << '\n';
                }
            }
            return;
        }
    }
    else if constexpr (Index == 2)
    {
        if (caller == 0x35727Cu || (caller == 0x1900A0u && readIntroStreamWord(ram, stack, 0x58) == 0x3572A4u))
            std::cerr << "[LCS cutscene model] stage=request id=" << first << " flags=" << second << '\n';
    }
    else if constexpr (Index == 3)
    {
        if (caller == 0x357334u)
        {
            introModelReads = 0;
            std::cerr << "[LCS cutscene model] stage=load-queue mode=" << first << '\n';
        }
    }
    else if constexpr (Index == 4)
    {
        if (caller == 0x18F79Cu && readIntroStreamWord(ram, stack, 0x58) == 0x357334u)
        {
            const uint32_t id = getRegU32(ctx, 17); // s1 is the queue's model ID.
            const uint32_t sector = getRegU32(ctx, 6);
            const uint32_t sectors = getRegU32(ctx, 7);
            if (id == introTaxiId)
            {
                introTaxiSector = sector;
                introTaxiSectors = sectors;
            }
            if (id == introTaxiId || introModelReads < 24)
            {
                ++introModelReads;
                std::cerr << "[LCS cutscene model] stage=stream-read id=" << id << " channel=" << first
                          << " dst=0x" << std::hex << second << std::dec
                          << " sector=" << sector << " count=" << sectors << '\n';
            }
        }
    }
    else if constexpr (Index == 5)
    {
        if (second == introTaxiId)
        {
            const uint32_t buffer = first & PS2_RAM_MASK;
            const uint64_t bytes = uint64_t(introTaxiSectors) * 2048u;
            std::cerr << "[LCS taxi] stage=decode caller=0x" << std::hex << caller
                      << " buffer=0x" << buffer << std::dec << " sector=" << introTaxiSector
                      << " bytes=" << bytes;
            if (caller == 0x18F7F4u && bytes && bytes <= 4u * 1024u * 1024u && bytes <= PS2_RAM_SIZE - buffer)
            {
                uint64_t hash = 14695981039346656037ull;
                for (size_t i = 0; i < bytes; ++i)
                    hash = (hash ^ ram[buffer + i]) * 1099511628211ull;
                std::cerr << " fnv1a=" << std::hex << hash << std::dec;
            }
            std::cerr << '\n';
            introModelTargets[Index](ram, ctx, runtime);
            if (ctx->pc == caller)
                std::cerr << "[LCS taxi] stage=decode-return result=" << getRegU32(ctx, 2) << '\n';
            return;
        }
    }
    else if constexpr (Index == 6)
    {
        if (caller == 0x18E9C0u && getRegU32(ctx, 18) == introTaxiId)
            std::cerr << "[LCS taxi] stage=decode-success-path\n";
    }
    else if constexpr (Index == 7)
    {
        if (caller == 0x358124u && second == introTaxiId)
            std::cerr << "[LCS taxi] stage=instance-resource ptr=0x" << std::hex << first << std::dec << '\n';
    }
    else if constexpr (Index == 8)
    {
        if (caller == 0x35818Cu && getRegU32(ctx, 16) == introTaxiId)
        {
            // The preceding call's delay slot has stored s1 in the scene's entity list.
            introTaxiEntity = getRegU32(ctx, 17);
            introTaxiListed = introTaxiPrepared = false;
            introTaxiRenderCalled = introTaxiRenderSubmitted = false;
            introTaxiGuardSampled = false;
            std::cerr << "[LCS taxi] stage=entity-stored ptr=0x" << std::hex << introTaxiEntity << std::dec << '\n';
        }
    }
    else if constexpr (Index == 9)
    {
        if (ctx->pc == 0x176CE8u && introTaxiEntity)
        {
            const uint32_t gate = readIntroStreamWord(ram, introTaxiEntity, 0x100);
            // The object-list loop branches on this byte at 0x176D1C.
            const uint8_t sceneFlag = ram[0x3D9CF3u];
            if (!introTaxiListed || gate != introTaxiLastGate || sceneFlag != introTaxiLastSceneFlag)
            {
                introTaxiListed = true;
                introTaxiLastGate = gate;
                introTaxiLastSceneFlag = sceneFlag;
                std::cerr << "[LCS taxi] stage=scene-pass entity=0x" << std::hex << introTaxiEntity
                          << " flag_3d9cf3=0x" << unsigned(sceneFlag)
                          << " gate_100=0x" << gate
                          << " rw_object=0x" << readIntroStreamWord(ram, introTaxiEntity, 0x50) << std::dec << '\n';
            }
        }
    }
    else if constexpr (Index == 10)
    {
        if (caller == 0x176D8Cu && introTaxiEntity && first == introTaxiEntity && !introTaxiPrepared)
        {
            introTaxiPrepared = true;
            std::cerr << "[LCS taxi] stage=scene-transform-sync rw_object=0x" << std::hex
                      << readIntroStreamWord(ram, first, 0x50) << std::dec << '\n';
        }
    }
    else if constexpr (Index == 11)
    {
        if (ctx->pc == 0x304FB8u && introTaxiEntity && first == introTaxiEntity && !introTaxiRenderCalled)
        {
            introTaxiRenderCalled = true;
            std::cerr << "[LCS taxi] stage=entity-render caller=0x" << std::hex << caller
                      << " rw_object=0x" << readIntroStreamWord(ram, first, 0x50) << std::dec << '\n';
        }
    }
    else if constexpr (Index == 12)
    {
        if (caller == 0x304FE8u && introTaxiEntity && getRegU32(ctx, 16) == introTaxiEntity && !introTaxiRenderSubmitted)
        {
            // 0x304FB8 preserves its entity in s0 and passes the derived render data in a0.
            introTaxiRenderSubmitted = true;
            std::cerr << "[LCS taxi] stage=renderer-entry data=0x" << std::hex << first << std::dec << '\n';
        }
    }
    else if (introTaxiEntity && getRegU32(ctx, 16) == introTaxiEntity && !introTaxiGuardSampled)
    {
        if constexpr (Index == 13)
        {
            if (caller == 0x304FD0u)
            {
                introModelTargets[Index](ram, ctx, runtime);
                if (ctx->pc == caller)
                {
                    introTaxiGuardSampled = true;
                    std::cerr << "[LCS taxi] stage=render-guard result=" << getRegU32(ctx, 2) << '\n';
                }
                return;
            }
        }
        else if (readIntroStreamWord(ram, stack, 0) == 0x304FD0u)
        {
            // The saved return address belongs to the outer guard at 0x17C608.
            if constexpr (Index == 14)
            {
                if (caller == 0x17C618u)
                {
                    introModelTargets[Index](ram, ctx, runtime);
                    if (ctx->pc == caller)
                        std::cerr << "[LCS taxi] stage=render-lookup entry=0x" << std::hex << getRegU32(ctx, 2) << std::dec << '\n';
                    return;
                }
            }
            else if (caller == 0x17C630u)
            {
                std::cerr << "[LCS taxi] stage=render-field object=0x" << std::hex << first
                          << " field_18=0x" << readIntroStreamWord(ram, first, 0x18) << std::dec << '\n';
            }
        }
    }
    introModelTargets[Index](ram, ctx, runtime);
}

template <unsigned Index>
PS2_GUEST_CC void traceIntroConsumer(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime)
{
    const uint32_t caller = getRegU32(ctx, 31);
    const uint32_t first = getRegU32(ctx, 4);
    const uint32_t second = getRegU32(ctx, 5);
    if constexpr (Index == 0)
    {
        if (caller == 0x3567ACu || caller == 0x3575C4u || caller == 0x357698u)
        {
            char name[25]{};
            const uint32_t address = second & PS2_RAM_MASK;
            for (size_t i = 0; i + 1 < sizeof(name) && address + i < PS2_RAM_SIZE; ++i)
            {
                if (ram[address + i] == 0u) break;
                name[i] = static_cast<char>(ram[address + i]);
            }
            std::cerr << "[LCS intro EE] stage=archive-lookup name=" << name
                      << " caller=0x" << std::hex << caller << std::dec << '\n';
            introTargets[Index](ram, ctx, runtime);
            if (ctx->pc == caller)
                std::cerr << "[LCS intro EE] stage=archive-result name=" << name
                          << " result=0x" << std::hex << getRegU32(ctx, 2) << std::dec << '\n';
            return;
        }
    }
    else if constexpr (Index == 1)
    {
        if (caller == 0x357628u)
            std::cerr << "[LCS intro EE] stage=animation-consumer a0=0x" << std::hex << first
                      << " a1=0x" << second << std::dec << '\n';
    }
    else if constexpr (Index == 2)
    {
        if (caller == 0x3576CCu)
        {
            introCameraStream = second;
            introCameraReads = 0;
            std::cerr << "[LCS intro EE] stage=camera-consumer stream=0x" << std::hex << second
                      << std::dec << '\n';
        }
    }
    else
    {
        if (caller == 0x28F94Cu && first && first == introCameraStream)
        {
            // The buffered reader at 0x2DAA90 advances stream+4 and indexes
            // the backing buffer at stream+0x10 relative to stream+8.
            const uint32_t position = readIntroStreamWord(ram, first, 4);
            introTargets[Index](ram, ctx, runtime);
            if (ctx->pc == caller)
            {
                const uint32_t next = readIntroStreamWord(ram, first, 4);
                const uint32_t bufferStart = readIntroStreamWord(ram, first, 8);
                const uint32_t bufferSize = readIntroStreamWord(ram, first, 12);
                const uint32_t buffer = readIntroStreamWord(ram, first, 16);
                const uint32_t count = getRegU32(ctx, 2);
                std::cerr << "[LCS intro EE] stage=camera-read pos=" << position
                          << " next=" << next << " count=" << count
                          << " cache_start=" << bufferStart << " cache_size=" << bufferSize
                          << " cache=0x" << std::hex << buffer << " dst=0x" << second;
                if (count == 1u)
                {
                    std::cerr << " byte=0x" << unsigned(ram[second & PS2_RAM_MASK]);
                    if (buffer && position >= bufferStart && position - bufferStart < bufferSize)
                        std::cerr << " cache_byte=0x" << unsigned(ram[(buffer + (position - bufferStart)) & PS2_RAM_MASK]);
                    if (position < sizeof(introCameraHeader) - 1u)
                        std::cerr << " expected=0x" << unsigned(static_cast<uint8_t>(introCameraHeader[position]));
                }
                std::cerr << std::dec << '\n';
                if (++introCameraReads == 32)
                    introCameraStream = 0;
            }
            return;
        }
    }
    introTargets[Index](ram, ctx, runtime);
}
std::atomic<bool> moviesActive{false},moviePresenting{false};
std::atomic<bool> moviesFinished{false};
PS2Runtime::RecompiledFunction originalTimerAlarm=nullptr,originalTimerIrq=nullptr,originalDelayWake=nullptr;
PS2_GUEST_CC void traceTimerAlarm(uint8_t* ram,R5900Context* ctx,PS2Runtime* rt) {
 static unsigned logged=0;if(moviesFinished.load() && logged++<24)std::cerr << "[LCS timer alarm] ticks=" << getRegU32(ctx,4) << " callback=0x" << std::hex << getRegU32(ctx,5) << " argument=0x" << getRegU32(ctx,6) << " status=0x" << ctx->cop0_status << " count=0x" << rt->memory().readIORegister(0x10001000) << " mode=0x" << rt->memory().readIORegister(0x10001010) << " compare=0x" << rt->memory().readIORegister(0x10001020) << std::dec << std::endl;
 originalTimerAlarm(ram,ctx,rt);
}
PS2_GUEST_CC void traceTimerIrq(uint8_t* ram,R5900Context* ctx,PS2Runtime* rt) {
 static unsigned count=0;if(moviesFinished.load() && count++<32)std::cerr << "[LCS timer IRQ] cause=" << getRegU32(ctx,4) << " count=" << rt->memory().readIORegister(0x10001000) << " mode=" << rt->memory().readIORegister(0x10001010) << " compare=" << rt->memory().readIORegister(0x10001020) << std::endl;
 originalTimerIrq(ram,ctx,rt);
}
PS2Runtime::RecompiledFunction originalDelayReadTrace=nullptr;
PS2_GUEST_CC void traceStreamingDelay(uint8_t* ram,R5900Context* ctx,PS2Runtime* rt) {
 static unsigned count=0;static bool sawBusy=false;
 const bool retry=getRegU32(ctx,31)==0x18f7a8u;
 const bool firstBusy=retry && getRegU32(ctx,16)==0 && !sawBusy;
 if(retry && (count++<12 || firstBusy)) {
  uint32_t reply=0,status=0;std::memcpy(&reply,ram+0x4280c0,4);std::memcpy(&status,ram+0x4280c4,4);
  std::cerr << "[LCS stream retry] s0=" << getRegU32(ctx,16) << " reply=" << reply << " status=" << status << " sp=0x" << std::hex << getRegU32(ctx,29) << std::dec << std::endl;
 }
 if(firstBusy)sawBusy=true;
 originalDelayReadTrace(ram,ctx,rt);
}
PS2_GUEST_CC void traceDelayWake(uint8_t* ram,R5900Context* ctx,PS2Runtime* rt) {
 static unsigned logged=0;if(moviesFinished.load() && logged++<24)std::cerr << "[LCS delay wake] a0=" << getRegU32(ctx,4) << " a1=" << getRegU32(ctx,5) << " a2=" << getRegU32(ctx,6) << std::endl;
 originalDelayWake(ram,ctx,rt);
}
// LCS owns all user heap RAM above its ELF. The HLE kernel occupies otherwise
// unused kernel RAM below the ELF: support allocations 0x80000..0xC0000,
// invocation stacks 0xC0000..0x100000. Kernel syscall mirrors end below 0x80000.
uint32_t sdkHeapLimit=0x1f00000u;
PS2_GUEST_CC void setupHeapWithSupportArena(uint8_t*,R5900Context* ctx,PS2Runtime*) {
    const uint32_t base=(getRegU32(ctx,4)+15u)&~15u,size=getRegU32(ctx,5);
    sdkHeapLimit=0x1f00000u;
    if(size!=0u && size!=0xffffffffu)sdkHeapLimit=static_cast<uint32_t>(std::min<uint64_t>(uint64_t(base)+size,sdkHeapLimit));
    setReturnU32(ctx,base);ctx->pc=0;
}
PS2_GUEST_CC void endOfProgramHeap(uint8_t*,R5900Context* ctx,PS2Runtime*) {setReturnU32(ctx,sdkHeapLimit);ctx->pc=0;}
PS2Runtime::RecompiledFunction originalRpc = nullptr;
PS2Runtime::RecompiledFunction originalPicture = nullptr;
PS2Runtime::RecompiledFunction originalPad = nullptr;
PS2Runtime::RecompiledFunction originalPresent = nullptr;
PS2_GUEST_CC void tracePresent(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    static unsigned count=0;
    uint32_t base=0,active=0;
    std::memcpy(&base,ram+0x003D4CD0u,4);
    if (base<=PS2_RAM_SIZE-0x814u) std::memcpy(&active,ram+base+0x800u,4);
    if(active && moviesActive.load())moviePresenting.store(true);
    if(lcs_diag::kEnabled && active && count++<12) std::cerr << "[LCS present IRQ] vif1=0x" << std::hex
        << runtime->memory().readIORegister(0x10009000u) << " gif=0x" << runtime->memory().readIORegister(0x1000A000u)
        << " vifstat=0x" << runtime->memory().readIORegister(0x10003C00u)
        << " gifstat=0x" << runtime->memory().readIORegister(0x10003020u) << std::dec << std::endl;
    originalPresent(ram,ctx,runtime);
    static unsigned afterCount=0;
    if (lcs_diag::kEnabled && active && afterCount++<24) {
        auto word=[&](uint32_t a) {uint32_t v=0;if(a<=PS2_RAM_SIZE-4)std::memcpy(&v,ram+a,4);return v;};
        uint32_t ring=base+0x673094u,slots=word(ring+4)&PS2_RAM_MASK,capacity=word(ring+16);
        std::cerr << "[LCS present state] field=" << word(base+0x80c) << " done=" << word(base+0x808)
          << " path=" << word(base+0x810) << " ring=" << std::hex << slots << std::dec
          << " r4=" << word(ring+4) << " r8=" << word(ring+8) << " count=" << word(ring+12) << " capacity=" << capacity;
        for(uint32_t i=0;i<capacity && i<4;++i)std::cerr << " slot" << i << "=" << word(slots+i*0x4ce40u);
        std::cerr << std::endl;
        if(afterCount==1) {
          uint32_t tag=slots+0x40u;
          for(unsigned i=0;i<12;++i) {
            uint32_t lo=word(tag),hi=word(tag+4),id=(lo>>28)&7,qwc=lo&0xffff;
            std::cerr << "[LCS movie chain] tag=0x" << std::hex << tag << " lo=0x" << lo << " hi=0x" << hi << " data=0x" << word(tag+16) << ":" << word(tag+20) << std::dec << " id=" << id << " qwc=" << qwc << std::endl;
            if(id==0 || id==7)break;
            tag=id==1?tag+16+qwc*16: id==2?(hi&PS2_RAM_MASK):tag+16;
          }
        }
    }
}
// This SDK wrapper must free through the same host allocator as sceSifAllocIopHeap.
PS2_GUEST_CC void freeIopHeap(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 ctx->pc=getRegU32(ctx,31);ps2_stubs::sceSifFreeIopHeap(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalLog=nullptr,originalMovieDone=nullptr;
PS2_GUEST_CC void traceLog(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 static unsigned count=0;uint32_t fmt=getRegU32(ctx,4);
 // [lcs-oom] the game's out-of-memory handler 0x1F3318 reports through this log: "... MEMORY ALLOCATION FAILURE",
 // "freed a ped / vechicle", "FREED ALL I CAN..." (0x3B5960..0x3B5A60)
 if(fmt>=0x3b5960u && fmt<0x3b5a60u) {
  static unsigned oom=0;
  if(oom++<200) {
   std::string s;for(uint32_t p=fmt;p<PS2_RAM_SIZE && ram[p] && ram[p]!='\n' && s.size()<80;++p)s+=static_cast<char>(ram[p]);
   std::cerr << "[lcs-oom] frame " << guestFramesPresented() << ": " << s << std::endl;
   reportHeaps(ram);
  }
 }
 if(lcs_diag::kEnabled && (fmt==0x3ab5c0u || fmt==0x3ab5d0u || fmt==0x3ab5e8u || fmt==0x3ab610u) && count++<32) {
  auto str=[&](uint32_t ptr){std::string result;ptr&=PS2_RAM_MASK;while(ptr<PS2_RAM_SIZE && ram[ptr] && result.size()<160)result+=static_cast<char>(ram[ptr++]);return result;};
  std::cerr << "[LCS file diagnostic] format=" << str(fmt);
  if(fmt!=0x3ab5d0u)std::cerr << " path=" << str(getRegU32(ctx,5));
  std::cerr << std::endl;
 }
 originalLog(ram,ctx,runtime);
}
PS2_GUEST_CC void traceMoviesDone(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 moviesFinished.store(true);moviePresenting.store(false);moviesActive.store(false);std::cerr << "[LCS boot movies completed]" << std::endl;originalMovieDone(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalMovie=nullptr;
PS2_GUEST_CC void traceMovie(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 if(ctx->pc==0x35a020u) {
  moviePresenting.store(false);moviesActive.store(true);
  if(fastIntroTest && getRegU32(ctx,6)==0 && getRegU32(ctx,5)==0){ctx->r[5]=_mm_set_epi64x(0,1);std::cerr << "[LCS diagnostic: first intro skip enabled]" << std::endl;}
  uint32_t ptr=getRegU32(ctx,4)&PS2_RAM_MASK;std::string name;
  while(ptr<PS2_RAM_SIZE && ram[ptr] && name.size()<160)name+=static_cast<char>(ram[ptr++]);
  std::cerr << "[LCS movie begin] path=" << name << " skip=" << getRegU32(ctx,5) << " threshold=" << getRegU32(ctx,6) << std::endl;
 }
 if (nativeBootMovie && !fastIntroTest && ctx->pc==0x35a020u && getRegU32(ctx,5)==0) {
  const uint32_t caller=getRegU32(ctx,31);
  const char* file=caller==0x1f374cu ? "RSNY.PSS;1" :
                   caller==0x1f3760u ? "RSLEEDS.PSS;1" :
                   caller==0x1f3774u ? "RSNORTH.PSS;1" :
                   caller==0x1f3788u ? "DM.PSS;1" : nullptr;
  if (file) {
   moviePresenting.store(true);
   const bool played=lcs_movie::play(*runtime,file,getRegU32(ctx,6));
   moviePresenting.store(false);
   if (played) {
    ctx->pc=caller;
    return;
   }
  }
 }
 originalMovie(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalPacket=nullptr;
PS2_GUEST_CC void tracePacket(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 static unsigned count=0;
 if(ctx->pc==0x1fc588u && count++<8)std::cerr << "[LCS movie packet] packet=0x" << std::hex << getRegU32(ctx,4) << " image=0x" << getRegU32(ctx,5) << std::dec << " field=" << getRegU32(ctx,6) << " width=" << getRegU32(ctx,7) << " height=" << getRegU32(ctx,8) << std::endl;
 originalPacket(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalProducer=nullptr;
PS2_GUEST_CC void traceProducer(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
 static unsigned count=0;
 if(++count%10000==0) {
  uint32_t ptr=getRegU32(ctx,4)&PS2_RAM_MASK,queued=0,capacity=0;
  std::memcpy(&queued,ram+ptr+12,4);std::memcpy(&capacity,ram+ptr+16,4);
  std::cerr << "[LCS producer] ptr=0x" << std::hex << ptr << " pc=0x" << ctx->pc << std::dec << " count=" << queued << " cap=" << capacity << std::endl;
 }
 originalProducer(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalYield=nullptr;
PS2_GUEST_CC void traceYield(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    static unsigned count=0;
    if(runtime->eeScheduler().currentThreadId()==3 && (++count<6 || count%10000==0)) {
        uint32_t base=0;std::memcpy(&base,ram+0x3d4cd0,4);
        std::cerr << "[LCS decoder yield] ra=0x" << std::hex << getRegU32(ctx,31) << " sp=0x" << getRegU32(ctx,29) << " base=0x" << base << " s0=0x" << getRegU32(ctx,16) << std::dec << " n=" << count;
        uint32_t queued=0;std::memcpy(&queued,ram+base+0x6730a0,4);std::cerr << " queued=" << queued << std::endl;
    }
    originalYield(ram,ctx,runtime);
}
PS2_GUEST_CC void tracePad(uint8_t* ram,R5900Context* ctx,PS2Runtime* runtime) {
    originalPad(ram,ctx,runtime);
    static unsigned count=0;
    const uint32_t caller=getRegU32(ctx,31);
    if (count<8 && caller>=0x35A140u && caller<=0x35A190u) {
        ++count; const uint32_t address=getRegU32(ctx,2)&PS2_RAM_MASK;
        uint16_t current=0,previous=0;
        std::memcpy(&current,ram+address+10,2);std::memcpy(&previous,ram+address+42,2);
        std::cerr << "[LCS movie input] ptr=0x" << std::hex << address << " current=0x" << current
            << " previous=0x" << previous << " caller=0x" << caller << std::dec << std::endl;
    }
}
PS2_GUEST_CC void tracePicture(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    static unsigned count=0;
    if (count++<8) std::cerr << "[LCS picture enter] tid=" << runtime->eeScheduler().currentThreadId()
        << " pc=0x" << std::hex << ctx->pc << " ra=0x" << getRegU32(ctx,31) << " sp=0x" << getRegU32(ctx,29)
        << " mpeg=0x" << getRegU32(ctx,4) << " image=0x" << getRegU32(ctx,5) << std::dec << std::endl;
    originalPicture(ram,ctx,runtime);
}
PS2Runtime::RecompiledFunction originalFsCommand = nullptr;
// --- RPC client->sid tracking (diagnostic only, no behavior change) ---
PS2Runtime::RecompiledFunction originalInitRpc = nullptr;
PS2Runtime::RecompiledFunction originalBindRpc = nullptr;
std::unordered_map<uint32_t,uint32_t> rpcBindMap; // clientPtr -> sid, filled by traceBindRpc
PS2_GUEST_CC void traceInitRpc(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    rpcBindMap.clear();
    if (lcs_diag::kEnabled)
        std::cerr << "[LCS rpc-trace] SifInitRpc (client map reset) ra=0x" << std::hex << getRegU32(ctx,31) << std::dec << std::endl;
    originalInitRpc(ram,ctx,runtime);
}
PS2_GUEST_CC void traceBindRpc(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const uint32_t clientPtr=getRegU32(ctx,4);
    const uint32_t sid=getRegU32(ctx,5);
    const uint32_t mode=getRegU32(ctx,6);
    const uint32_t ra=getRegU32(ctx,31);
    // ---- LCS DIAG RPC TRACE: EE-BIND ----
    if (lcs_rpc_diag::enabled())
        lcs_rpc_diag::emit("EE-BIND",
            "client=0x%08x sid=0x%08x mode=0x%08x ra=0x%08x pc=0x%08x sp=0x%08x",
            clientPtr, sid, mode, ra, ctx->pc, getRegU32(ctx,29));
    // ---- END EE-BIND ----
    originalBindRpc(ram,ctx,runtime);
    const uint32_t ret=getRegU32(ctx,2);
    uint32_t server=0,sema=0;
    const uint32_t c=clientPtr&PS2_RAM_MASK;
    if(c+40<=PS2_RAM_SIZE){std::memcpy(&server,ram+c+36,4);std::memcpy(&sema,ram+c+8,4);}
    rpcBindMap[clientPtr]=sid;
    if (lcs_diag::kEnabled)
    std::cerr << "[LCS rpc-trace] SifBindRpc client=0x" << std::hex << clientPtr
        << " sid=0x" << sid << " mode=" << std::dec << mode
        << " ret=" << ret
        << " server=0x" << std::hex << server
        << " sema=0x" << std::dec << sema
        << " ra=0x" << std::hex << ra << std::dec << std::endl;
}
PS2_GUEST_CC void traceRpc(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    static unsigned count=0;
    const bool trace=lcs_diag::kEnabled && count++<256;

    // Lazy bind: The FILEIO ORCH (0x381CC8) runs before SifInitRpc in the boot
    // sequence, so its PRE-INIT gate skips SifBindRpc. By the time the client
    // first issues an RPC call, the FILEIO server (0x80000001) is registered.
    // Detect the unbound FILEIO client and perform the deferred bind here.
    {
        const uint32_t client = getRegU32(ctx, 4);
        if (rpcBindMap.find(client) == rpcBindMap.end()) {
            constexpr uint32_t fileioClient = 0x00646200u;
            constexpr uint32_t fileioSid    = 0x80000001u;
            if (client == fileioClient) {
                const uint32_t cc = fileioClient & PS2_RAM_MASK;
                uint32_t storedSid = 0;
                if (cc + 8 <= PS2_RAM_SIZE)
                    std::memcpy(&storedSid, ram + cc + 4, 4);
                if (storedSid == 0) {
                    const uint32_t savedA1 = getRegU32(ctx, 5);
                    const uint32_t savedA2 = getRegU32(ctx, 6);
                    const uint32_t savedA3 = getRegU32(ctx, 7);
                    const uint32_t savedT0 = getRegU32(ctx, 8);

                    // ---- LCS DIAG RPC TRACE: LAZY-BIND-BEFORE ----
                    // Gated on enabled() (NOT tracing()): this fires on the FIRST
                    // FILEIO call, i.e. BEFORE beginTrace() runs inside SifCallRpc.
                    // It is a one-shot setup event (deferred bind), not a per-call
                    // budgeted event.
                    if (lcs_rpc_diag::enabled())
                    {
                        const uint32_t sp0 = getRegU32(ctx, 29);
                        uint32_t s00=0,s10=0,s14=0,s18=0,s1c=0,s20=0;
                        const auto rdU32 = [&](uint32_t gaddr, uint32_t &out) {
                            out = 0;
                            const uint32_t off = gaddr & PS2_RAM_MASK;
                            if (off + 4 <= PS2_RAM_SIZE)
                                std::memcpy(&out, ram + off, 4);
                        };
                        rdU32(sp0 + 0x00u, s00);
                        rdU32(sp0 + 0x10u, s10);
                        rdU32(sp0 + 0x14u, s14);
                        rdU32(sp0 + 0x18u, s18);
                        rdU32(sp0 + 0x1Cu, s1c);
                        rdU32(sp0 + 0x20u, s20);
                        lcs_rpc_diag::emit("LAZY-BIND-BEFORE",
                            "sp=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
                            "t0=0x%08x t1=0x%08x t2=0x%08x t3=0x%08x t4=0x%08x t5=0x%08x t6=0x%08x t7=0x%08x "
                            "ra=0x%08x pc=0x%08x "
                            "sp+0x00=0x%08x sp+0x10=0x%08x sp+0x14=0x%08x sp+0x18=0x%08x sp+0x1C=0x%08x sp+0x20=0x%08x",
                            sp0,
                            getRegU32(ctx,4), getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7),
                            getRegU32(ctx,8), getRegU32(ctx,9), getRegU32(ctx,10), getRegU32(ctx,11),
                            getRegU32(ctx,12), getRegU32(ctx,13), getRegU32(ctx,14), getRegU32(ctx,15),
                            getRegU32(ctx,31), ctx->pc,
                            s00, s10, s14, s18, s1c, s20);
                    }
                    // ---- END LAZY-BIND-BEFORE ----

                    ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(fileioSid)));
                    ctx->r[6] = _mm_set_epi64x(0, 0);
                    originalBindRpc(ram, ctx, runtime);
                    const uint32_t bindRet = getRegU32(ctx, 2);

                    // ---- LCS DIAG RPC TRACE: LAZY-BIND-AFTER ----
                    // Gated on enabled() (NOT tracing()): one-shot, paired with
                    // LAZY-BIND-BEFORE. Shows whether originalBindRpc clobbered
                    // t1-t7 (recvBuf/recvSize/endFunc) that the deferred bind must
                    // preserve for the pending SifCallRpc.
                    if (lcs_rpc_diag::enabled())
                    {
                        const uint32_t sp1 = getRegU32(ctx, 29);
                        uint32_t t0=0,t1=0,t2=0,t3=0,t4=0,t5=0,t6=0,t7=0;
                        t0=getRegU32(ctx,8); t1=getRegU32(ctx,9); t2=getRegU32(ctx,10); t3=getRegU32(ctx,11);
                        t4=getRegU32(ctx,12); t5=getRegU32(ctx,13); t6=getRegU32(ctx,14); t7=getRegU32(ctx,15);
                        lcs_rpc_diag::emit("LAZY-BIND-AFTER",
                            "ret=%u sp=0x%08x "
                            "t0=0x%08x t1=0x%08x t2=0x%08x t3=0x%08x t4=0x%08x t5=0x%08x t6=0x%08x t7=0x%08x "
                            "a1=0x%08x a2=0x%08x a3=0x%08x",
                            bindRet, sp1,
                            t0, t1, t2, t3, t4, t5, t6, t7,
                            getRegU32(ctx,5), getRegU32(ctx,6), getRegU32(ctx,7));
                    }
                    // ---- END LAZY-BIND-AFTER ----

                    ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(savedA1)));
                    ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(savedA2)));
                    ctx->r[7] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(savedA3)));
                    ctx->r[8] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(savedT0)));
                    rpcBindMap[fileioClient] = fileioSid;
                    std::cerr << "[LCS rpc-trace] LAZY BIND client=0x" << std::hex << fileioClient
                        << " sid=0x" << fileioSid << " ret=" << std::dec << bindRet << std::endl;
                }
            }
        }
    }

    if(trace) {
        const uint32_t client=getRegU32(ctx,4);
        const auto it=rpcBindMap.find(client);
        std::cerr << "[LCS rpc-trace] SifCallRpc client=0x" << std::hex << client
            << (it==rpcBindMap.end() ? " bound=NO(default sid=0 expected)" : " boundSid=0x")
            << std::dec;
        if(it!=rpcBindMap.end()) std::cerr << std::hex << it->second << std::dec;
        // stored SID in the guest client struct (t_SifRpcHeader.rpc_id at +4) - ground truth
        uint32_t storedSid=0,storedSema=0;
        const uint32_t cc=client&PS2_RAM_MASK;
        if(cc+12<=PS2_RAM_SIZE){std::memcpy(&storedSid,ram+cc+4,4);std::memcpy(&storedSema,ram+cc+8,4);}
        std::cerr << " storedSid=0x" << std::hex << storedSid << " storedSema=0x" << storedSema << std::dec;
        std::cerr << " fn=" << getRegU32(ctx,5)
            << " mode=" << getRegU32(ctx,6)
            << " bytes=" << getRegU32(ctx,8)
            << " ra=0x" << std::hex << getRegU32(ctx,31) << std::dec;
        uint32_t ptr=getRegU32(ctx,7)&PS2_RAM_MASK;
        for(unsigned i=0;i<4 && ptr+4*i<=PS2_RAM_SIZE-4;++i){uint32_t w=0;std::memcpy(&w,ram+ptr+4*i,4);std::cerr << " w" << i << "=0x" << std::hex << w << std::dec;}
        std::cerr << std::endl;
    }
    originalRpc(ram,ctx,runtime);
}
PS2_GUEST_CC void traceFsCommand(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    // ---- LCS DIAG RPC TRACE: EE-CALLBACK ----
    if (lcs_rpc_diag::enabled())
        lcs_rpc_diag::emit("EE-CALLBACK",
            "pc=0x%08x ra=0x%08x sp=0x%08x a0=0x%08x a1=0x%08x tid=%u",
            ctx->pc, getRegU32(ctx,31), getRegU32(ctx,29),
            getRegU32(ctx,4), getRegU32(ctx,5),
            static_cast<unsigned>(runtime->eeScheduler().currentThreadId()));
    // ---- END EE-CALLBACK ----
    if (lcs_diag::kEnabled)
    {
    std::cerr << "[LCS trace] FILEIO completion command entered packet=0x" << std::hex
        << getRegU32(ctx, 4) << std::dec << std::endl;
    std::cerr << "[LCS callback stack] sp=0x" << std::hex << getRegU32(ctx,29) << std::dec << std::endl;
    for (const auto& thread:runtime->eeScheduler().snapshot().threads)
        std::cerr << "[LCS stack descriptor] tid=" << thread.id << " base=0x" << std::hex << thread.stack
            << " size=0x" << thread.stackSize << " sp=0x" << thread.sp << std::dec << std::endl;
    }
    originalFsCommand(ram, ctx, runtime);
}
// WaitVBlank (0x2D9360, leaf): `do v0 = *0x3D9A44; while (!v0); *0x3D9A44 = 0;`. The frame limiter
// (0x2B54A8) calls it until two VBlanks have passed (30 fps). Spinning only advances the EE clock by
// the per-iteration charge (~19% of real time in the Release build), so VBlanks - which need both
// their cycle and host deadline - arrived at ~11 Hz and the game ran at ~6 fps. Instead block the
// guest thread until the next VBlank (scheduler idle-skips to it; host pacing keeps 60 Hz) and
// re-enter at 0x2D9360 to re-check the flag. LCS_VBLANK_SPIN=1 restores the original spin loop.
// [lcs] Frame counter for the overlay's game fps: the render loop (0x2B5400) programs the GS display for each
// finished frame by calling 0x2D9640 from 0x2B54CC - its only caller. Counting those calls counts the frames the
// game really released, independent of what the window shows.
constexpr uint32_t kDisplayFlip = 0x002D9640u;
std::atomic<unsigned long long> framesPresented{0};
enum class FrameRateMode { Fps30, Fps60, Fps120, Uncapped }; // F6 cycles them
std::atomic<FrameRateMode> frameRateMode{[] {
    const char* value = std::getenv("LCS_FPS60");
    return value && value[0] == '1' ? FrameRateMode::Fps60 : FrameRateMode::Fps30;
}()};
PS2Runtime::RecompiledFunction originalDisplayFlip = nullptr;
// [lcs] Hor+ widescreen as the reference port's ref_display.cpp setViewWindow: LCS's view-window setter 0x113478 (the reference port 0x1AB9E0,
// 94 % instruction match: camera a0, raster rect a1 - null = 640x448, tan(FOV/2) f12, aspect f13) gets the horizontal
// extent x r with the vertical one unchanged (f12 and f13 x r), r = display aspect / 4:3 (LCS_ASPECT, 16:9 default).
// The 4:3 frustum culling is not widened yet (the reference port 0x3816F8 ~ LCS 0x298D28, unsure): objects at the edges may pop.
PS2Runtime::RecompiledFunction originalSetViewWindow = nullptr;
// [lcs] live (the video page's aspect, lcs_bootstrap::setHorPlusWidening); start value from LCS_ASPECT
std::atomic<float> horPlusR{[] {
    const char* e = std::getenv("LCS_ASPECT");
    return (e && std::string_view(e) == "4:3") ? 1.0f : (16.0f / 9.0f) / (4.0f / 3.0f);
}()};
float horPlusWidening() { return horPlusR.load(std::memory_order_relaxed); }
// [lcs] the reference port's widened culling (ref_display.cpp prepareSphereTest / sphereVisible): CCamera::IsSphereVisible
// (LCS 0x298D28 = the reference port 0x3816F8, same code: camera matrix from a2, radius f12) tests camera-space side planes the game
// builds from the FOV alone - for the 4:3 view - at camera +0x1C20 (right: cos h, -sin h) and +0x1C30 (left) (the reference port +0xB60
// / +0xB70). Before the test they are widened to the Hor+ edge (tan h x r): (a, r b) normalised. Once widened they are
// recognised and left until the game rebuilds them. LCS_WIDE_CULL=0: the game's planes.
PS2Runtime::RecompiledFunction originalSphereVisible = nullptr;
struct LcsSidePlanes { uint32_t camera = 0; float game[4] = {}, wide[4] = {}; float r = 0.0f; }; // r: widened for
LcsSidePlanes lcsPlanes[4];
unsigned lcsNextPlanes = 0;
void widenSidePlane(const float* game, float r, float* wide) {
    const float a = game[0], b = game[1] * r, len = std::sqrt(a * a + b * b);
    wide[0] = len > 0.0f ? a / len : game[0];
    wide[1] = len > 0.0f ? b / len : game[1];
}
PS2_GUEST_CC void sphereVisibleWide(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    static const bool wideCull = !(std::getenv("LCS_WIDE_CULL") && std::getenv("LCS_WIDE_CULL")[0] == '0');
    const float r = horPlusWidening();
    if (wideCull && r != 1.0f) {
        const uint32_t cam = getRegU32(ctx, 4) & PS2_RAM_MASK;
        constexpr uint32_t kRight = 0x1C20u, kLeft = 0x1C30u;
        if (cam && cam + kLeft + 8u <= PS2_RAM_SIZE) {
            float now[4];
            std::memcpy(&now[0], ram + cam + kRight, 8);
            std::memcpy(&now[2], ram + cam + kLeft, 8);
            LcsSidePlanes* found = nullptr;
            for (auto& e : lcsPlanes)
                if (e.camera == cam)
                    found = &e;
            const bool same = found && std::memcmp(now, found->wide, sizeof(now)) == 0;
            if (!same || found->r != r) { // the game rebuilt them (or first time), or the aspect changed (video page)
                LcsSidePlanes& p = found ? *found : lcsPlanes[lcsNextPlanes++ % 4u];
                p.camera = cam;
                p.r = r;
                if (!same) // still ours with only r changed: widened again from the game's planes kept
                    std::memcpy(p.game, now, sizeof(now));
                widenSidePlane(p.game, r, p.wide);
                widenSidePlane(p.game + 2, r, p.wide + 2);
                std::memcpy(ram + cam + kRight, &p.wide[0], 8);
                std::memcpy(ram + cam + kLeft, &p.wide[2], 8);
            }
        }
    }
    originalSphereVisible(ram, ctx, runtime);
}
PS2_GUEST_CC void setViewWindowHorPlus(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    const float r = horPlusWidening();
    if (r != 1.0f) {
        ctx->f[12] *= r; // horizontal extent x r
        ctx->f[13] *= r; // vertical extent (x / aspect) unchanged
    }
    originalSetViewWindow(ram, ctx, runtime);
}
// [lcs] A flip whose 0x2D9640 hit a scheduler checkpoint returns here with ctx->pc inside the function: it
// finishes later through a registered re-entry, not through this hook, so its frame was never submitted (one game
// frame without a frame end - the next one then carries two frames' GS work). Such a flip is kept pending and
// submitted at the next frame boundary the hook sees: the limiter's WaitVBlank (0x2B54A8, before the next flip) or,
// at the latest, the next flip itself. One frame end per game frame. LCS_FLIP_DEFER=0: as before.
std::atomic<bool> flipPending{false};
std::atomic<unsigned long long> flipsDeferred{0};
void submitGameFrame(uint8_t* ram);
void submitPendingFlip(uint8_t* ram, const char* where) {
    if (!flipPending.exchange(false, std::memory_order_acq_rel))
        return;
    const auto n = flipsDeferred.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || (n % 500u) == 0)
        std::cerr << "[lcs-flip] deferred frame end #" << n << " submitted at " << where
                  << " (0x2D9640 finished after a scheduler checkpoint)" << std::endl;
    submitGameFrame(ram);
}
std::atomic<uint32_t> flipPendingReturn{0};
std::atomic<unsigned long long> flipsResumed{0};
PS2_GUEST_CC void countDisplayFlip(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    static const bool defer = [] { const char* v = std::getenv("LCS_FLIP_DEFER"); return !(v && v[0] == '0'); }();
    if (defer)
        submitPendingFlip(ram, "the next flip (fallback)");
    const uint32_t returnPc = getRegU32(ctx, 31);
    originalDisplayFlip(ram, ctx, runtime);
    if (ctx->pc != returnPc) {
        if (defer) {
            flipPendingReturn.store(returnPc, std::memory_order_relaxed);
            flipPending.store(true, std::memory_order_release);
        }
        return;
    }
    submitGameFrame(ram);
}
// [lcs] the re-entries of 0x2D9640 after a checkpoint (its jal return addresses, register_functions): the
// frame end goes out where the resumed flip really returns to its caller
PS2_GUEST_CC void resumeDisplayFlip(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    originalDisplayFlip(ram, ctx, runtime);
    if (flipPending.load(std::memory_order_acquire) && ctx->pc == flipPendingReturn.load(std::memory_order_relaxed) &&
        flipPending.exchange(false, std::memory_order_acq_rel)) {
        const auto n = flipsResumed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n % 500u) == 0)
            std::cerr << "[lcs-flip] frame end #" << n << " after a checkpoint inside 0x2D9640, at its return" << std::endl;
        submitGameFrame(ram);
    }
}
void submitGameFrame(uint8_t* ram) {
    { // [lcs] the pause menu: CTimer's pause flags (0x3D9CB4 / 0x3D9C68, VC's m_UserPause / m_CodePause - the ones
      // CTimer::Update 0x2B4CF8 checks) tell the renderer the menu is up, so it is shown 4:3 at once instead of after the
      // 3D history runs out (half a second of the menu stretched, user report). LCS_MENU_PAUSE=0: as before.
        static const bool on = !(std::getenv("LCS_MENU_PAUSE") && std::getenv("LCS_MENU_PAUSE")[0] == '0');
        if (on) {
            uint32_t user = 0, code = 0;
            std::memcpy(&user, ram + 0x3D9CB4u, 4);
            std::memcpy(&code, ram + 0x3D9C68u, 4);
            const bool paused = user != 0u || code != 0u; // read as words by CTimer::Update (lw)
            static bool was = false;
            if (paused != was) {
                was = paused;
                std::cerr << "[lcs-menu] pause flags " << user << "/" << code << ": menu "
                          << (paused ? "up" : "down") << std::endl;
            }
            gsHwSetMenuActive(paused);
            lcs_menu::frame(ram, paused); // [lcs-menu] (empty outside the menu build)
        }
    }
    ps2PresentGameFrame();
    ps2perf::markGameFrame();
    lcs::profilerFrame();
    const auto flip = framesPresented.fetch_add(1, std::memory_order_relaxed) + 1;
    static FILE* trace = [] {
        const char* path = std::getenv("LCS_FLIP_CSV");
        FILE* file = path ? _fsopen(path, "w", _SH_DENYNO) : nullptr;
        if (file)
            std::fputs("flip,host_ms,guest_ms,cutscene\n", file);
        return file;
    }();
    if (trace) {
        using Clock = std::chrono::steady_clock;
        static const auto origin = Clock::now();
        uint32_t gameMs, cutscene;
        std::memcpy(&gameMs, ram + 0x3D9B70u, sizeof(gameMs));
        std::memcpy(&cutscene, ram + 0x3D5AB8u, sizeof(cutscene));
        const double hostMs = std::chrono::duration<double, std::milli>(Clock::now() - origin).count();
        std::fprintf(trace, "%llu,%.3f,%u,%u\n", flip, hostMs, gameMs, cutscene);
        if (flip % 30u == 0)
            std::fflush(trace);
    }
}
constexpr uint32_t kWaitVBlank = 0x002D9360u, kVBlankFlag = 0x003D9A44u;
PS2_GUEST_CC void waitVBlank(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    constexpr uint32_t kLimiterReturn = 0x002B54B0u;
    static const bool independent = [] {
        const char *value = std::getenv("LCS_INDEPENDENT_TIMING");
        return value && std::string_view(value) == "1";
    }();
    const auto mode = frameRateMode.load(std::memory_order_relaxed);
    const bool limiter = getRegU32(ctx, 31) == kLimiterReturn;
    if (limiter)
    {
        submitPendingFlip(ram, "the frame limiter");
        using Clock = std::chrono::steady_clock;
        constexpr auto period = std::chrono::nanoseconds(1000000000 / 30);
        static Clock::time_point deadline{};
        // [lcs] 30 / 60 / 120 FPS: the uncapped path held to a 30 / 60 / 120 Hz deadline. A frame that is late is not
        // held to the next guest VBlank: at 60 FPS an 18 ms frame took 33 ms (run_101529: up to 44 such doubles in 5 s),
        // at 30 FPS a late frame came as 46 ms + a 20 ms catch-up (run_144318). With the deadline it is shown at once and
        // the next frame is timed from it - with VRR (G-Sync) an 18 ms frame instead of a 33 ms hitch.
        // LCS_VBLANK_PACING=1: 30 / 60 FPS on the guest VBlanks as before (LCS_INDEPENDENT_TIMING=1 still paces 30 then).
        static const bool vblankPacing = [] {
            const char *value = std::getenv("LCS_VBLANK_PACING");
            return value && std::string_view(value) == "1";
        }();
        const bool independent30 = (independent || !vblankPacing) && mode == FrameRateMode::Fps30;
        const bool paced60 = !vblankPacing && mode == FrameRateMode::Fps60;
        const bool paced = independent30 || paced60 || mode == FrameRateMode::Fps120;
        const auto pacePeriod = mode == FrameRateMode::Fps120 ? std::chrono::nanoseconds(1000000000 / 120)
                              : mode == FrameRateMode::Fps60  ? std::chrono::nanoseconds(1000000000 / 60)
                                                              : period;
        if (!paced)
            deadline = {};

        if (mode == FrameRateMode::Uncapped || paced)
        {
            if (paced)
            {
                const auto now = Clock::now();
                if (deadline != Clock::time_point{} && deadline + pacePeriod > now)
                {
                    deadline += pacePeriod;
                    ps2perf::PhaseScope wait(ps2perf::Phase::Wait);
                    platform::delayPrecise(std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count());
                }
                else
                    deadline = now;
            }

            // [lcs] Guest work charged at a quarter of its estimated cycles, as the reference port's independent timing does
            // (ref_framerate.cpp): the clock is topped up to real time just below anyway, and at the full estimate it
            // ran ahead of real time in busy scenes - the EE held at every VBlank until real time caught up
            // (run_132213, many helicopters: ~400 ms/s "pacing to a deadline" with the game thread running, ~36 fps).
            // LCS_EE_CHARGE_SHIFT=<n>: 1/2^n (0 = the full estimate, as before).
            static const uint32_t chargeShift = [] {
                const char *v = std::getenv("LCS_EE_CHARGE_SHIFT");
                return v && *v ? static_cast<uint32_t>(std::atoi(v)) : 2u;
            }();
            runtime->eeScheduler().setGuestChargeShift(chargeShift);
            // Keep EE timers advancing. Only the game's frame limiter is bypassed.
            runtime->eeScheduler().syncToRealTime(58982400u);
            const uint32_t done = 2u;
            const uint32_t counter = (getRegU32(ctx, 18) + 4u) & PS2_RAM_MASK;
            std::memcpy(ram + counter, &done, sizeof(done));
            ctx->pc = kLimiterReturn;
            return;
        }
        runtime->eeScheduler().setGuestChargeShift(0u); // [lcs] on the guest VBlanks (LCS_VBLANK_PACING=1): the full estimate
    }

    uint32_t flag;
    std::memcpy(&flag, ram + (kVBlankFlag & PS2_RAM_MASK), sizeof(flag));
    SET_GPR_U32(ctx, 2, flag);
    if (flag == 0u)
        runtime->eeWaitVSyncTicks(1u, kWaitVBlank); // no return: resumes at kWaitVBlank after the VBlank
    SET_GPR_U32(ctx, 1, 0x003E0000u);
    const uint32_t zero = 0u;
    std::memcpy(ram + (kVBlankFlag & PS2_RAM_MASK), &zero, sizeof(zero));
    // At 60 FPS the limiter completes after one real guest VBlank.
    if (mode == FrameRateMode::Fps60 && limiter) {
        const uint32_t counter = (getRegU32(ctx, 18) + 4u) & PS2_RAM_MASK;
        uint32_t vblanks;
        std::memcpy(&vblanks, ram + counter, sizeof(vblanks));
        if (vblanks < 2u) {
            vblanks = 2u;
            std::memcpy(ram + counter, &vblanks, sizeof(vblanks));
        }
    }
    ctx->pc = getRegU32(ctx, 31);
}

PS2_GUEST_CC void reboot(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    // Match the generated wrapper's return convention, then retain the existing reset.
    ctx->pc = getRegU32(ctx, 31);
    ps2_stubs::sceSifRebootIop(rdram, ctx, runtime);
    for (const char* module : {"IOMAN.irx", "CDVDMAN.irx", "LOADFILE.irx", "FILEIO.irx", "CDVDFSV.irx"}) {
        // [lcs] the ISO mode's modules (main.cpp mountIso: from the ISO's IOPRP300.IMG), else game\ioprp300
        const char *irxDir = std::getenv("LCS_IRX_DIR");
        const auto path = (irxDir && *irxDir ? std::filesystem::path(irxDir)
                                             : std::filesystem::current_path() / "game" / "ioprp300") / module;
        const auto loaded = runtime->loadIopModule(path.string());
        std::cerr << "[LCS bootstrap] " << module << " after reboot: id=" << loaded.moduleId
            << " start=" << loaded.startResult << std::endl;
        if (!loaded.handled || loaded.moduleId <= 0 || loaded.startResult < 0) {
            setReturnS32(ctx, 0);
            return;
        }
    }
}
// Run on the EE thread at the normal cheat input update. A registered return
// address lets the guest spawn routine resume after scheduler checkpoints.
constexpr uint32_t kCheatUpdate = 0x170A10u, kSpawnResume = 0x170A14u;
constexpr uint32_t kSpawnVehicle = 0x16D228u, kMaverick = 214u;
std::atomic<bool> maverickRequested{false};
PS2Runtime::RecompiledFunction originalCheatUpdate = nullptr;
R5900Context beforeMaverick{};

PS2_GUEST_CC void resumeMaverick(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    *ctx = beforeMaverick;
    originalCheatUpdate(ram, ctx, runtime);
}

PS2_GUEST_CC void updateMaverick(uint8_t* ram, R5900Context* ctx, PS2Runtime* runtime) {
    if (maverickRequested.exchange(false, std::memory_order_relaxed)) {
        const uint32_t slot = ram[0x3D9BC8u]; // same player slot as guest position routine 0x107170
        uint32_t ped = 0, streaming = 0;
        std::memcpy(&ped, ram + 0x408E40u + slot * 400u, sizeof(ped));
        std::memcpy(&streaming, ram + 0x3D0CC8u, sizeof(streaming));
        if (ped && streaming && !moviesActive.load()) {
            beforeMaverick = *ctx;
            SET_GPR_U32(ctx, 4, kMaverick);
            SET_GPR_U32(ctx, 31, kSpawnResume);
            std::fprintf(stderr, "[lcs] F1: requesting Maverick (214)\n");
            if (runtime->dispatchGuestBranch(ram, ctx, kSpawnVehicle, kCheatUpdate, kSpawnResume,
                    PS2Runtime::GuestBranchKind::DirectCall, "F1 Maverick"))
                resumeMaverick(ram, ctx, runtime);
            return;
        }
    }
    originalCheatUpdate(ram, ctx, runtime);
}

void apply(PS2Runtime& runtime) {
    if (!enabled) return;
    originalCheatUpdate = runtime.lookupFunction(kCheatUpdate);
    if (runtime.hasFunction(kSpawnResume) || !runtime.hasFunction(kSpawnVehicle) ||
        !runtime.registerFunction(kSpawnResume, resumeMaverick) ||
        !runtime.replaceFunction(kCheatUpdate, updateMaverick))
        throw std::runtime_error("LCS Maverick hook could not be bound");
    const char *controllerProfile = std::getenv("LCS_CONTROLLER_PROFILE");
    if (controllerProfile && (std::strcmp(controllerProfile, "modern") == 0 || std::strcmp(controllerProfile, "classic") == 0))
    {
        // Both values are consumed by vehicle physics on the 0..255 scale.
        constexpr uint32_t accelerate = 0x171B38, brake = 0x1717E0;
        originalAccelerate = runtime.lookupFunction(accelerate);
        originalBrake = runtime.lookupFunction(brake);
        if (!originalAccelerate || !originalBrake ||
            !runtime.replaceFunction(accelerate, readDrivingPedal<false>) ||
            !runtime.replaceFunction(brake, readDrivingPedal<true>))
            throw std::runtime_error("LCS driving pedal hooks could not be bound");
    }
    lcs::rslworld::installLcs(runtime);
    lcs_stream::install(runtime); // [lcs-stream] disc streaming on the host (src/lcs/lcs_stream.cpp)
    lcs_inflate::install(runtime); // [lcs-inflate] zlib inflate_fast natively (src/lcs/lcs_inflate.cpp)
    lcs_physics_timing::install(runtime); // [lcs] bike brake wobble per unit of time (src/lcs/lcs_physics_timing.cpp)
    lcs_script_timing::install(runtime); // [lcs] mission scripts that count per frame on the time step (src/lcs/lcs_script_timing.cpp)
    lcs_world::install(runtime); // [lcs] draw distance: the LOD scale 0x3D7578 (src/lcs/lcs_world_detail.cpp)
    lcs_timestep::install(runtime); // [lcs] the step floor's excess paid back (src/lcs/lcs_timestep.cpp)
    lcs_hud_timing::install(runtime); // [lcs] the HUD's flashing (wanted stars ...) on the game clock (src/lcs/lcs_hud_timing.cpp)
    lcs_prompts::install(runtime);
    lcs_hud::install(runtime);   // [lcs-hud] the 2D primitives for the HUD's per-element anchoring (src/lcs/lcs_hud.cpp)
    lcs_freecam::install(runtime); // [lcs-freecam] F8: fly the camera around the world (src/lcs/lcs_freecam.cpp)
    lcs_menu::install(runtime);    // [lcs-menu] experimental pause-menu video page (empty outside the menu build)
    lcs_input::install(runtime);   // [lcs-input] keyboard and mouse as GTA SA PC, device detection (menu build)
    if (lcs_diag::kEnabled) // [lcs-free] [lcs-model] [lcs-alloc] crash forensics: rings of frees, allocations, model conversions
    {
        freedRing = new FreedBlock[kFreedRing](); // [lcs-free]
        originalHeapFree = runtime.lookupFunction(0x150E98u);
        if (!originalHeapFree || !runtime.replaceFunction(0x150E98u, recordHeapFree))
            throw std::runtime_error("LCS heap free trace could not be bound");
        g_ps2xAddressReport = &reportFreed;
        conversions = new ModelConversion[kConversions](); // [lcs-model]
        originalModelConvert = runtime.lookupFunction(0x18DEE8u);
        if (!originalModelConvert || !runtime.replaceFunction(0x18DEE8u, recordModelConvert))
            throw std::runtime_error("LCS model conversion trace could not be bound");
        allocRing = new AllocBlock[kAllocRing](); // [lcs-alloc]
        originalHeapAlloc = runtime.lookupFunction(0x150478u);
        originalHeapAllocAligned = runtime.lookupFunction(0x1505D8u);
        if (!originalHeapAlloc || !originalHeapAllocAligned || !runtime.replaceFunction(0x150478u, recordHeapAlloc<0x150478u>) ||
            !runtime.replaceFunction(0x1505D8u, recordHeapAlloc<0x1505D8u>))
            throw std::runtime_error("LCS heap allocation trace could not be bound");
    }
    // [lcs-heap] every free of the game: the diag build, or LCS_HEAP_LOG=1 (the F2 heap lines read the heaps without it)
    if (lcs_diag::kEnabled || lcs_diag::wanted("LCS_HEAP_LOG"))
    {
        originalHeapFreeIn = runtime.lookupFunction(0x150740u);
        if (!originalHeapFreeIn || !runtime.replaceFunction(0x150740u, recordHeapFreeIn))
            throw std::runtime_error("LCS heap trace could not be bound");
    }
    originalOomMain = runtime.lookupFunction(0x1F3318u); // [lcs-oom]
    originalOomSecond = runtime.lookupFunction(0x1F3190u);
    if (!originalOomMain || !originalOomSecond || !runtime.replaceFunction(0x1F3318u, traceOom<true>) ||
        !runtime.replaceFunction(0x1F3190u, traceOom<false>))
        throw std::runtime_error("LCS out-of-memory trace could not be bound");
    if (lcs_diag::kEnabled) // [lcs-ring] [lcs-clump] list checks (diagnosis only, the originals run unchanged)
    {
        originalObjectListClear = runtime.lookupFunction(0x207B98u); // [lcs-ring] the object list clear (run_181507)
        if (!originalObjectListClear || !runtime.replaceFunction(0x207B98u, checkObjectListClear))
            throw std::runtime_error("LCS object list check could not be bound");
        originalFrameForAll = runtime.lookupFunction(0x2BB338u); // [lcs-ring] the frame object list walk
        if (!originalFrameForAll || !runtime.replaceFunction(0x2BB338u, checkFrameForAll))
            throw std::runtime_error("LCS frame list check could not be bound");
        originalListForAll = runtime.lookupFunction(0x31E820u); // [lcs-ring] diagnosis of the 0x31E820 list loop
        if (!originalListForAll || !runtime.replaceFunction(0x31E820u, checkListForAll))
            throw std::runtime_error("LCS list check could not be bound");
        originalClumpAdd = runtime.lookupFunction(0x31E920u); // [lcs-clump]
        originalClumpRemove = runtime.lookupFunction(0x31E950u);
        originalAtomicDestroy = runtime.lookupFunction(0x2E9110u);
        if (!originalClumpAdd || !originalClumpRemove || !originalAtomicDestroy || !runtime.replaceFunction(0x31E920u, checkClumpAdd) ||
            !runtime.replaceFunction(0x31E950u, checkClumpRemove) || !runtime.replaceFunction(0x2E9110u, checkAtomicDestroy))
            throw std::runtime_error("LCS clump checks could not be bound");
    }
    if (ps2x::iop::cdvd_debug::introTraceEnabled())
    {
        constexpr uint32_t entries[] = {0x1450D8u, 0x1B99F0u, 0x28F8A0u, 0x146E88u};
        constexpr PS2Runtime::RecompiledFunction traces[] = {
            traceIntroConsumer<0>, traceIntroConsumer<1>, traceIntroConsumer<2>, traceIntroConsumer<3>
        };
        for (size_t i = 0; i < std::size(entries); ++i)
        {
            introTargets[i] = runtime.lookupFunction(entries[i]);
            if (!introTargets[i] || !runtime.replaceFunction(entries[i], traces[i]))
                throw std::runtime_error("LCS intro consumer trace unavailable");
        }
        constexpr uint32_t modelEntries[] = {
            0x31F850u, 0x18FF80u, 0x18B400u, 0x18F648u, 0x1F6CA0u,
            0x18E4B8u, 0x18A388u, 0x357F28u, 0x18EA98u, 0x176CE8u, 0x26C918u, 0x304FB8u, 0x333568u,
            0x17C608u, 0x17C538u, 0x3332C0u
        };
        constexpr PS2Runtime::RecompiledFunction modelTraces[] = {
            traceIntroModels<0>, traceIntroModels<1>, traceIntroModels<2>, traceIntroModels<3>, traceIntroModels<4>,
            traceIntroModels<5>, traceIntroModels<6>, traceIntroModels<7>, traceIntroModels<8>,
            traceIntroModels<9>, traceIntroModels<10>, traceIntroModels<11>, traceIntroModels<12>,
            traceIntroModels<13>, traceIntroModels<14>, traceIntroModels<15>
        };
        for (size_t i = 0; i < std::size(modelEntries); ++i)
        {
            introModelTargets[i] = runtime.lookupFunction(modelEntries[i]);
            if (!introModelTargets[i] || !runtime.replaceFunction(modelEntries[i], modelTraces[i]))
                throw std::runtime_error("LCS cutscene model trace unavailable");
        }
    }
    // Host-only syscall trampolines use verified zero padding between EE functions.
    constexpr uint32_t setupTrampoline=0x1138bc,endTrampoline=0x1138c0;
    uint32_t padding[2]{};std::memcpy(padding,runtime.memory().getRDRAM()+setupTrampoline,8);
    if(padding[0] || padding[1])throw std::runtime_error("Support trampoline padding differs from original ELF");
    if(runtime.hasFunction(setupTrampoline) || runtime.hasFunction(endTrampoline))throw std::runtime_error("Support trampoline overlaps generated code");
    if(!runtime.registerFunction(setupTrampoline,setupHeapWithSupportArena) || !runtime.registerFunction(endTrampoline,endOfProgramHeap))throw std::runtime_error("Support syscall registration failed");
    if (lcs_diag::kEnabled) // [LCS stream retry] trace
    {
        originalDelayReadTrace=runtime.lookupFunction(0x37e2b0);
        if(!originalDelayReadTrace || !runtime.replaceFunction(0x37e2b0,traceStreamingDelay))
            throw std::runtime_error("LCS streaming delay trace unavailable");
    }
    runtime.configureGuestHeap(0x80000,0xc0000);
    // [lcs] callback stacks 0xC0000-0x100000: kAsyncCallbackStackFloor in ps2_runtime.h
    runtime.setEeSyscallOverride(runtime.memory().getRDRAM(),0x3d,setupTrampoline);
    runtime.setEeSyscallOverride(runtime.memory().getRDRAM(),0x3e,endTrampoline);
    if (!runtime.replaceFunction(0x00383330u, reboot))
        throw std::runtime_error("LCS reboot override could not be bound");
    if(!runtime.replaceFunction(0x382C78u,freeIopHeap))throw std::runtime_error("FreeIopHeap binding failed");
    originalLog=runtime.lookupFunction(0x104BA0u);originalMovieDone=runtime.lookupFunction(0x35A000u);
    if(!originalLog || !originalMovieDone || !runtime.replaceFunction(0x104BA0u,traceLog) || !runtime.replaceFunction(0x35A000u,traceMoviesDone))throw std::runtime_error("Boot trace binding failed");
    originalMovie=runtime.lookupFunction(0x35A020u);
    if(!originalMovie || !runtime.replaceFunction(0x35A020u,traceMovie))throw std::runtime_error("Movie trace binding failed");
    if (lcs_diag::kEnabled) // movie packet / producer, decoder yield and timer traces
    {
        originalPacket=runtime.lookupFunction(0x1FC588u);
        if(!originalPacket || !runtime.replaceFunction(0x1FC588u,tracePacket))throw std::runtime_error("Packet trace binding failed");
        originalProducer=runtime.lookupFunction(0x22EDD0u);
        if(!originalProducer || !runtime.replaceFunction(0x22EDD0u,traceProducer))throw std::runtime_error("Producer trace binding failed");
        originalYield=runtime.lookupFunction(0x35A3D8u);
        if(!originalYield || !runtime.replaceFunction(0x35A3D8u,traceYield))throw std::runtime_error("Yield trace binding failed");
        originalTimerAlarm=runtime.lookupFunction(0x384730);originalTimerIrq=runtime.lookupFunction(0x383f18);originalDelayWake=runtime.lookupFunction(0x37e4f0);
        if(!originalTimerAlarm || !originalTimerIrq || !originalDelayWake)throw std::runtime_error("Timer diagnostic bindings missing");
        runtime.replaceFunction(0x384730,traceTimerAlarm);runtime.replaceFunction(0x383f18,traceTimerIrq);runtime.replaceFunction(0x37e4f0,traceDelayWake);
    }
    originalPresent=runtime.lookupFunction(0x001FC908u);
    if (!originalPresent || !runtime.replaceFunction(0x001FC908u,tracePresent)) throw std::runtime_error("Present trace binding failed");
    if (lcs_diag::kEnabled) // movie input and picture traces
    {
        originalPad=runtime.lookupFunction(0x00170BE8u);
        if (!originalPad || !runtime.replaceFunction(0x00170BE8u,tracePad)) throw std::runtime_error("Pad trace binding failed");
        originalPicture=runtime.lookupFunction(0x0036A160u);
        if (!originalPicture || !runtime.replaceFunction(0x0036A160u,tracePicture)) throw std::runtime_error("Picture trace binding failed");
    }
    originalRpc = runtime.lookupFunction(0x00380CF0u);
    originalFsCommand = runtime.lookupFunction(0x00381560u);
    if (!originalRpc || !originalFsCommand ||
        !runtime.replaceFunction(0x00380CF0u, traceRpc) ||
        !runtime.replaceFunction(0x00381560u, traceFsCommand))
        throw std::runtime_error("LCS trace targets could not be bound");
    originalInitRpc = runtime.lookupFunction(0x003804C8u);
    originalBindRpc = runtime.lookupFunction(0x00380B10u);
    if (!originalInitRpc || !originalBindRpc ||
        !runtime.replaceFunction(0x003804C8u, traceInitRpc) ||
        !runtime.replaceFunction(0x00380B10u, traceBindRpc))
        throw std::runtime_error("LCS rpc-trace targets could not be bound");
    originalSetViewWindow = runtime.lookupFunction(0x00113478u);
    if (!originalSetViewWindow || !runtime.replaceFunction(0x00113478u, setViewWindowHorPlus))
        throw std::runtime_error("LCS Hor+ view window hook could not be bound");
    std::cerr << "[lcs] Hor+ widening " << horPlusWidening() << " (view window 0x113478)" << std::endl;
    originalSphereVisible = runtime.lookupFunction(0x00298D28u);
    if (!originalSphereVisible || !runtime.replaceFunction(0x00298D28u, sphereVisibleWide))
        throw std::runtime_error("LCS widened culling hook could not be bound");
    // This CSR read uses only FIELD, which VBlank sets independently of GS work.
    // Keep the other CSR reads fenced for SIGNAL and FINISH.
    if (!std::getenv("LCS_CSR_FENCE"))
        runtime.addCsrReadWithoutFence(0x002D9678u);
    originalDisplayFlip = runtime.lookupFunction(kDisplayFlip);
    if (!originalDisplayFlip || !runtime.replaceFunction(kDisplayFlip, countDisplayFlip))
        throw std::runtime_error("LCS display flip counter could not be bound");
    for (const uint32_t resume : {0x2D9790u, 0x2D9798u, 0x2D97BCu, 0x2D97C4u, 0x2D97CCu, 0x2D97D8u, 0x2D99A0u, 0x2D99D0u,
                                  0x2D9A00u, 0x2D9A28u}) // [lcs] 0x2D9640's re-entries (see resumeDisplayFlip)
        if (runtime.lookupFunction(resume) == originalDisplayFlip && !runtime.replaceFunction(resume, resumeDisplayFlip))
            throw std::runtime_error("LCS display flip re-entry could not be bound");
    if (const char* spin = std::getenv("LCS_VBLANK_SPIN"); !(spin && std::string_view(spin) == "1")) {
        if (!runtime.hasFunction(kWaitVBlank) || !runtime.replaceFunction(kWaitVBlank, waitVBlank))
            throw std::runtime_error("LCS WaitVBlank override could not be bound");
        std::cerr << "[LCS bootstrap] WaitVBlank 0x2D9360 blocks until VBlank (LCS_VBLANK_SPIN=1: original spin)" << std::endl;
    }
    std::cerr << "[LCS bootstrap] original IOMAN/CDVDMAN/LOADFILE/FILEIO/CDVDFSV bootstrap enabled" << std::endl;
}
}
void setNativeBootMovie(bool enabled){nativeBootMovie=enabled;}
bool bootMoviesActive(){return moviesActive.load();}
unsigned long long guestFramesPresented(){return framesPresented.load(std::memory_order_relaxed);}
void requestMaverick() { maverickRequested.store(true, std::memory_order_relaxed); }
const char* frameRateModeLabel() {
    switch (frameRateMode.load(std::memory_order_relaxed)) {
    case FrameRateMode::Fps30: return "30 FPS";
    case FrameRateMode::Fps60: return "60 FPS";
    case FrameRateMode::Fps120: return "120 FPS";
    case FrameRateMode::Uncapped: return "Uncapped";
    }
    return "";
}
// [lcs] the F2 menu's video page: the frame-rate mode by index (0 30, 1 60, 2 120, 3 uncapped) and the Hor+ factor
int frameRateModeIndex() { return static_cast<int>(frameRateMode.load(std::memory_order_relaxed)); }
void setFrameRateMode(int index) {
    if (index < 0 || index > 3 || index == frameRateModeIndex())
        return;
    frameRateMode.store(static_cast<FrameRateMode>(index), std::memory_order_relaxed);
    std::cerr << "[lcs-timing] video page: mode " << frameRateModeLabel() << " at guest flip " << guestFramesPresented() << std::endl;
}
void setHorPlusWidening(float r) { horPlusR.store(r > 0.0f ? r : 1.0f, std::memory_order_relaxed); }
void cycleFrameRateMode() {
    const auto current = frameRateMode.load(std::memory_order_relaxed);
    const auto next = current == FrameRateMode::Fps30 ? FrameRateMode::Fps60 :
        current == FrameRateMode::Fps60 ? FrameRateMode::Fps120 :
        current == FrameRateMode::Fps120 ? FrameRateMode::Uncapped : FrameRateMode::Fps30;
    frameRateMode.store(next, std::memory_order_relaxed);
    std::cerr << "[lcs-timing] F6 mode " << frameRateModeLabel()
              << " at guest flip " << guestFramesPresented() << std::endl;
}
bool bootMoviePresenting(){return moviesActive.load() && moviePresenting.load();}
void setEnabled(bool value) { enabled = value; }
void setFastIntroTest(bool value) {fastIntroTest=value;}
PS2_REGISTER_GAME_OVERRIDE("LCS US original IOP bootstrap", "SLUS_214.23", 0x001C6A60u, 0xDC486B9Du, apply)
}


