// [lcs] The game's two heaps with 64 MB of guest RAM (the user's ask 2026-11-18: "Erweiter den Heap auf 64").
//
// The PS2 has 32 MB. LCS splits 0x673168 .. 0x1FE0000 (the main thread's stack is 0x1FE0000 .. 0x2000000, SetupThread
// at 0x1C6BF4): the main heap (0x406D50) gets all of it but 0x4E2000 bytes (0x14F2F8: size + 0xFFB1E000 at
// 0x14F328 / 0x14F330, constructor 0x14F3C8 through 0x14F358), the second, streaming heap (0x634140; 0x331D78) the
// 4.9 MB after the main heap's end. The runtime's guest RAM is now 64 MB (PS2_RAM_SIZE, runtime/ps2_memory.h), so:
// - the heaps' room ends at 0x2FE0000 instead of 0x1FE0000 (0x14F318) and the main stack moves right above it,
//   0x2FE0000 .. 0x3000000 (SetupThread's a1 at 0x1C6BF4); the main heap keeps its start and gets all of the room
//   (about 41.4 MB instead of 20.5 MB; the user's ask: "Können wir Main nicht auch erhöhen?"),
// - the second heap is put above the stack, 0x3000000 .. 0x3FC0000 (15.75 MB instead of 4.9 MB; the top 256 KB stay
//   unused).
// The generated code calls these at 0x14F318, 0x14F330, 0x1C6BF4 and 0x331DC8 (generated/ee,
// tools/lcs_codegen_edits.json).
// LCS_HEAP_LAYOUT=ps2: the PS2's layout (both heaps as in the executable, inside the first 32 MB).
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kRoomEnd = 0x02FE0000u; // and the main stack 0x2FE0000 .. 0x3000000
    constexpr uint32_t kSecondStart = 0x03000000u;
    constexpr uint32_t kSecondEnd = 0x03FC0000u;
    static_assert(kSecondEnd <= PS2_RAM_SIZE, "the second heap needs the 64 MB guest RAM");

    bool extended()
    {
        static const bool on = [] {
            const char *v = std::getenv("LCS_HEAP_LAYOUT");
            const bool ps2 = v && std::strcmp(v, "ps2") == 0;
            std::fprintf(stderr, ps2 ? "[lcs-memory] heaps as on the PS2 (LCS_HEAP_LAYOUT=ps2)\n"
                                     : "[lcs-memory] 64 MB: main heap to 0x2FE0000, stack 0x2FE0000..0x3000000, second heap 0x3000000..0x3FC0000\n");
            return !ps2;
        }();
        return on;
    }
}

// 0x14F318 ($v0, the heaps' room's end) and 0x1C6BF4 (SetupThread's stack, 0x20000 bytes from there): 0x1FE0000 on
// the PS2
uint32_t lcsHeapRoomEnd()
{
    return extended() ? kRoomEnd : 0x01FE0000u;
}

// 0x14F330: $a1 = the main heap's reserve for the second heap (0xFFB1E000 = -0x4E2000), added to the room's size
uint32_t lcsMainHeapReserve(uint32_t reserve)
{
    return extended() ? 0u : reserve;
}

// 0x331DC8 (after `subu $s0, $s0, $a1`): $s2 = the second heap's start, $s0 = its size
void lcsSecondHeapLayout(uint32_t &start, uint32_t &size)
{
    if (!extended())
        return;
    std::fprintf(stderr, "[lcs-memory] second heap %08x + %08x moved to %08x + %08x\n", start, size, kSecondStart,
                 kSecondEnd - kSecondStart);
    start = kSecondStart;
    size = kSecondEnd - kSecondStart;
}
