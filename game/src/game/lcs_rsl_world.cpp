// World and model draws from the RSL draw calls - see game/lcs_rsl_world.h.
#include "lcs/lcs_diag.h"
#include "game/lcs_rsl_world.h"

#include "game/lcs_hle_world.h"
#include "game/lcs_rsl_native.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_chain_observer.h"
#include "runtime/gs/gs_hw.h"
#include "runtime/ps2_hle_survey.h"
#include "runtime/ps2_write_watch.h"

#include <algorithm>
#include <array>
#include <immintrin.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>
#include "runtime/ps2_host_std.h"

namespace lcs::rslworld
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kPktLo = 0x520F90u, kPktHi = 0x6EDC70u; // the packet buffer (game-internals.md 6i)
        constexpr uint32_t kWorldDraw = 0x002F7EB0u;               // FUN_002F7EB0(geometry, qwc): REF geometry
        constexpr uint32_t kSectorDraw = 0x002409C8u;              // FUN_002409C8(level, sector, pass): a sector's draws
        constexpr uint32_t kSectorCaching = 0x00241060u;           // its return address when FUN_00240ED8 caches its packets
        // step 5a: FUN_002409C8 caches a material's setup - FUN_00408F30 (texture bind) and FUN_002F7B30 (GS registers)
        // between these calls are copied into a block CALLed on later frames (game-internals.md 6i)
        constexpr uint32_t kTextureBind = 0x00408F30u, kMaterialCacheBind = 0x00240D1Cu, kMaterialCacheGs = 0x00240D24u;
        constexpr uint32_t kGsSendA = 0x002F7908u, kGsSendB = 0x002F7B30u; // send the queued GS registers
        constexpr uint32_t kTexMatrix = 0x002F7090u;                       // FUN_002F7090(m): 3 qwords -> VU 0x2C
        constexpr uint32_t kVuConstants = 0x001CEF18u;                     // FUN_001CEF18(vu, m): 3 qwords -> VU vu
        constexpr uint32_t kFlushA = 0x002F9CE8u;                          // FUN_002F9CE8: CNT with FLUSHA
        constexpr uint32_t kUpload = 0x003298D8u;                          // FUN_003298D8: a texture upload's two REFs
        constexpr uint32_t kModelDraw = 0x002F7D58u;               // FUN_002F7D58(packet): CALL packet
        constexpr uint32_t kModelDrawB = 0x002F80A0u;              // FUN_002F80A0(packet): CALL packet

        bool g_world = false, g_models = false, g_rasters = false, g_verify = false, g_log = false;
        PS2Runtime::RecompiledFunction g_worldOriginal = nullptr, g_modelOriginal = nullptr, g_modelBOriginal = nullptr;

        enum Kind : uint8_t
        {
            World,     // a world draw's REF
            Model,     // FUN_002F7D58's CALL
            ModelB,    // FUN_002F80A0's CALL
            OtherCall, // a CALL in the packet buffer no draw call above wrote (FUN_002F80A0's state packet, ...)
            RasterUpload, // a REF sending a raster's pixels (VIF DIRECT): its address names the texture (step 3d)
            KindCount
        };
        const char *const kKindNames[KindCount] = {"world", "model", "model-b", "other call", "raster upload"};

        struct Draw
        {
            uint32_t id, addr, qwc; // the tag the draw call writes: REF addr qwc / CALL addr (qwc unused)
            Kind kind;
            uint32_t units = 0; // step 4: a block copied without its bytes - its length (8-byte units) as last copied
            bool wouldSkip = false; // LCS_RSL_SKIP_VERIFY: copied all the same, checked against the recording
            bool producerSlot = false; // submitted at the LCS draw site
            uint64_t flatHash = 0;  // LCS_RSL_ITEMS_VERIFY: the hash of the block as an item would have copied it (0: none)
        };

        // ---- step 4c: draw items (docs/FULLY_NATIVE.md step 4) -------------------------------------------------------
        // A world / model draw call whose state flush is native writes no packets for itself: its flush (when it is all
        // CNTs), its GS registers and its block go into an item, and the packet buffer gets one 16-byte CNT tag whose two
        // VIFcodes are MARK (kItemMark + index) and a NOP holding kItemNop - its place in the chain. At the kick the item's
        // block is decided as the walker decides a marked block (skipped when unchanged and confirmed, else copied from
        // guest RAM as the walker copies it); at the MARK the worker runs the item's VIF bytes and the block.
        constexpr uint32_t kItemMark = 0x8000u;    // MARK imm of an item: kItemMark + index (the walker's own marks stay below)
        // the NOP after an item's MARK (opcode 0: a NOP; its low bits ignored): kItemNop | the frame's number (low 16 bits) -
        // a tag the game copied and sends again in a later frame is told apart from this frame's (FUN_00240ED8 caches whole
        // sector draws, game-internals.md 6i)
        constexpr uint32_t kItemNop = 0x00D40000u;
        constexpr uint32_t kMaxItems = 0x7FFFu;
        struct RasterInBlock // a raster upload inside an item's copied block (step 3d names textures by them)
        {
            uint32_t offset, address, bytes; // offset: in the block's bytes
        };
        struct ConfirmedBlock
        {
            uint64_t hash = 0;
            std::vector<uint8_t> bytes;
            std::vector<RasterInBlock> rasters;
        };
        struct Item
        {
            Draw d;
            std::shared_ptr<const ConfirmedBlock> confirmed;
            uint32_t prefixAt = 0, prefixBytes = 0; // in the table's arena: the flush's VIF bytes, then the GS registers' DIRECTHL
            uint32_t blockAt = 0, blockBytes = 0;   // in the arena: the block, copied at the kick (none when skipped)
            uint32_t rastersAt = 0, rasterCount = 0; // in the table's itemRasters
            bool skip = false, lost = false;         // skip: replayed by key; lost: its block could not be copied
            bool hasBlock = false;                   // step 5a: an item of state ops alone has none
            uint32_t refAt = 0, refCount = 0;        // step 5b: its reference ops (the frame's / the table's refs)
            uint32_t prefixRastersAt = 0, prefixRasterCount = 0; // step 5b: raster uploads in its prefix (itemRasters)
            bool continued = false;                  // step 5f: the next item runs right after it (no tag of its own)
        };
        // step 5b: a REF the item stands for - its two VIFcodes and the payload in guest RAM, copied at the kick (when the DMA
        // would read it) into the item's VIF bytes at `pos` (the inline bytes before it)
        struct RefOp
        {
            uint32_t pos;
            uint32_t code[2];
            uint32_t addr, qwc;
            uint32_t owner = 0; // the item it belongs to (checked at the kick)
        };
        std::vector<Draw> g_current, g_kicked; // this frame's draw calls / the frame whose chain is being copied
        size_t g_next = 0;                     // the next of g_kicked whose tag the chain should hold

        // the copied chain's marked tags, by MARK index, for the thread that processes it
        struct RasterRef // a texture upload's pixels: where they are in the copied chain, whose they are
        {
            uint32_t offset, address, bytes;
        };
        struct ChainTable
        {
            std::vector<Draw> draws;
            std::unordered_map<uint32_t, uint32_t> lcsDrawItems;
            std::vector<RasterRef> rasters; // by offset
            uint64_t seq = 0;               // step 4: which kick (g_chainSeq)
            std::vector<Item> items;        // step 4c: the frame's draw items, by MARK index - kItemMark
            std::vector<uint8_t> arena;     // their VIF bytes and blocks
            std::vector<RasterInBlock> itemRasters;
            std::vector<RefOp> refs;        // step 5b: the items' reference ops, by Item::refAt
            uint32_t chainBytes = 0;        // the copied chain's size
            uint32_t itemFrame = 0;         // the frame whose items these are (g_itemFrame)
        };
        uint64_t g_chainSeq = 0; // game thread

        // step 4: the worker's word that a block's recording matches a copy (recording key, the chain it was in) - handed
        // over at the end of each chain, read by the game thread at the next kick
        std::mutex g_confirmMutex;
        std::vector<std::pair<uint64_t, uint64_t>> g_confirmPending;
        thread_local std::vector<std::pair<uint64_t, uint64_t>> t_confirm;
        std::atomic<uint64_t> g_skipped{0}, g_skipReplayed{0}, g_skipFallback{0};
        std::atomic<uint64_t> g_lcsItemBytes[KindCount]{}, g_lcsItemCount[KindCount]{};
        std::atomic<uint64_t> g_lcsStateCount{0}, g_lcsUploadCount{0};
        std::atomic<uint64_t> g_lcsProducerSlots{0}, g_lcsProducerExecuted{0}, g_lcsPreparedBytes{0};
        std::atomic<uint64_t> g_lcsNoSnapshot{0}, g_lcsDirty{0}, g_lcsUnconfirmed{0}, g_lcsTagCommands{0}, g_lcsUnsupported{0};
        std::atomic<uint64_t> g_skipVerifySame{0}, g_skipVerifyDiffer{0};
        std::atomic<uint64_t> g_skipLost{0}, g_skipSeen{0}; // step 4: skip marks the worker saw / could not place
        bool g_skip = false, g_skipVerify = false;
        ChainTable g_building;
        std::mutex g_tablesMutex;
        std::map<const void *, std::shared_ptr<ChainTable>> g_tables;
        thread_local std::shared_ptr<ChainTable> t_table;
        thread_local const uint8_t *t_chain = nullptr; // the chain being processed (its copy's data)
        // step 4c: an item's block being processed (its bytes are in the table's arena, not in the chain) and its rasters
        thread_local const uint8_t *t_block = nullptr;
        thread_local uint32_t t_blockBytes = 0;
        thread_local const RasterInBlock *t_blockRasters = nullptr;
        thread_local uint32_t t_blockRasterCount = 0;
        // LCS_RSL_MEASURE=1 (step 4 sizing): per chain, the bytes of marked blocks, and of distinct ones
        struct Measure
        {
            uint64_t chains = 0, chainBytes = 0, blocks = 0, blockBytes = 0, distinct = 0, distinctBytes = 0;
        } g_measure;
        thread_local std::unordered_map<uint64_t, uint32_t> t_seen;
        // step 4: marked blocks copied, and those unchanged since their last copy (game thread; logged in report)
        struct CleanStats
        {
            uint64_t blocks = 0, clean = 0, unseen = 0;
        } g_cleanStats;
        std::atomic<uint32_t> g_lastChainBytes{0};

        // the raster upload whose pixels are at `data` in the chain being processed (step 3d), if one is
        const RasterRef *rasterAt(const uint8_t *data, uint32_t bytes)
        {
            if (t_block && data >= t_block && data < t_block + t_blockBytes) // inside an item's block
            {
                thread_local RasterRef found;
                const uint32_t offset = static_cast<uint32_t>(data - t_block);
                for (uint32_t i = 0; i < t_blockRasterCount; ++i)
                    if (t_blockRasters[i].offset == offset && t_blockRasters[i].bytes == bytes)
                    {
                        found = {offset, t_blockRasters[i].address, bytes};
                        return &found;
                    }
                return nullptr;
            }
            const ChainTable *table = t_table.get();
            if (!table || table->rasters.empty() || !t_chain || data < t_chain || data >= t_chain + table->chainBytes)
                return nullptr;
            const uint32_t offset = static_cast<uint32_t>(data - t_chain);
            const auto it = std::lower_bound(table->rasters.begin(), table->rasters.end(), offset,
                                             [](const RasterRef &r, uint32_t o) { return r.offset < o; });
            return it != table->rasters.end() && it->offset == offset && it->bytes == bytes ? &*it : nullptr;
        }

        // ---- recordings (the processing thread) ----------------------------------------------------------------------
        struct Op
        {
            enum Type : uint8_t
            {
                Unpack,
                Mscal,
                Direct,
                Raw // step 5e: VIFcodes replayed through the interpreter as they were (BASE / OFFSET / ITOP; MPG with its code)
            } type = Unpack;
            uint32_t startPc = 0;            // MSCAL
            lcs::hleworld::WorldMesh mesh;   // MSCAL: its world packet's mesh
            bool topRelative = false;        // UNPACK
            uint8_t lanes = 0xF;             // UNPACK: the fields it writes (the others keep VU memory)
            bool directHl = false;           // DIRECT
            uint32_t rasterAddress = 0, rasterBytes = 0; // DIRECT: the raster upload it sends (step 3d), if known
            uint32_t vuAddr = 0, qwords = 0; // UNPACK: VU address (relative to TOPS when topRelative); DIRECT: qwords
            std::vector<uint8_t> data;       // UNPACK: the qwords as written; DIRECT: the GS data
        };

        struct Recording
        {
            uint64_t hash = 0;
            uint32_t mode = 0, cycle = 0; // VIF state the UNPACK results depend on
            // a block the game rebuilds every frame (changed bytes three times running) or one not replayable: processed
            // by the packet path for the next `skip` hits, without hashing or recording
            uint32_t changes = 0, skip = 0;
            bool probe = false; // the skip ran out: record once more to see whether it settled
            bool replayable = false;
            const char *refused = nullptr; // why not
            std::vector<Op> ops;
            bool setsMask = false, setsRow = false, setsCycle = false; // the block's STMASK / STROW / STCYCL
            uint32_t mask = 0, row[4] = {}, finalCycle = 0;             // the VIF mask / row / cycle it leaves
        };

        bool rawOps() // step 5e: LCS_RSL_RAW=0 refuses blocks with BASE / OFFSET / ITOP / MPG as before
        {
            static const bool on = !(std::getenv("LCS_RSL_RAW") && *std::getenv("LCS_RSL_RAW") == '0');
            return on;
        }

        class Recorder final : public ps2chain::VifRecorder
        {
        public:
            Recording *rec = nullptr;
            const char *failed = nullptr;
            bool maskSet = false, rowSet = false, cycleSet = false;

            void fail(const char *why)
            {
                if (!failed)
                    failed = why;
            }

            void unpack(uint32_t vuAddr, uint32_t qwords, bool topRelative, uint32_t tops, uint32_t cycle, bool masked, uint32_t mode,
                        uint32_t lanes, bool lanesFromCol, const uint8_t *vuData) override
            {
                if ((cycle & 0xFFFFu) != 0x0101u && (cycle & 0xFFFFu) != 0u)
                    fail("write cycle"); // CL / WL other than 1 / 1: the writes skip, not reproduced
                if (masked && (!maskSet || !rowSet))
                    fail("mask from before"); // the mask / row would come from before the block
                if (lanesFromCol)
                    fail("COL"); // the block never sets COL
                if ((mode & 3u) != 0u)
                    fail("mode"); // MODE adds / accumulates ROW
                Op &op = rec->ops.emplace_back();
                op.topRelative = topRelative;
                op.lanes = static_cast<uint8_t>(lanes & 0xFu);
                op.vuAddr = topRelative ? (vuAddr - tops) & 0x3FFu : vuAddr & 0x3FFu;
                op.qwords = qwords;
                op.data.resize(size_t(qwords) * 16u);
                for (uint32_t i = 0; i < qwords; ++i)
                    std::memcpy(op.data.data() + i * 16u, vuData + ((vuAddr + i) & 0x3FFu) * 16u, 16);
            }

            void mscal(uint32_t startPc) override
            {
                Op &op = rec->ops.emplace_back();
                op.type = Op::Mscal;
                op.startPc = startPc;
                // the native world handler stores a drawn world packet's mesh (the program at 0x30)
                lcs::hleworld::setMeshCapture(startPc == 0x30u ? &op.mesh : nullptr);
            }

            void direct(const uint8_t *data, uint32_t qwords, bool directHl, bool complete) override
            {
                if (!complete)
                    fail("DIRECT cut off");
                Op &op = rec->ops.emplace_back();
                op.type = Op::Direct;
                op.directHl = directHl;
                op.qwords = qwords;
                op.data.assign(data, data + size_t(qwords) * 16u);
                if (const RasterRef *r = rasterAt(data, qwords * 16u))
                {
                    op.rasterAddress = r->address;
                    op.rasterBytes = r->bytes;
                }
            }

            void other(uint32_t vifcode) override
            {
                const uint32_t opcode = (vifcode >> 24) & 0x7Fu;
                if (opcode == 0x20u) // STMASK / STROW / STCYCL: the block sets its own (restored after a replay)
                    maskSet = true;
                else if (opcode == 0x30u)
                    rowSet = true;
                else if (opcode == 0x01u)
                    cycleSet = true; // (the cycle each UNPACK saw is checked there)
                else if ((opcode == 0x02u || opcode == 0x03u || opcode == 0x04u || opcode == 0x4Au) && !rawOps())
                    fail(vifcodeName(opcode)); // LCS_RSL_RAW=0: refused as before step 5e
                else if (opcode == 0x02u || opcode == 0x03u || opcode == 0x04u)
                {
                    // step 5e: OFFSET / BASE / ITOP (the double buffer's registers, TOPS): replayed as they are, in order
                    Op &op = rec->ops.emplace_back();
                    op.type = Op::Raw;
                    const auto *c = reinterpret_cast<const uint8_t *>(&vifcode);
                    op.data.assign(c, c + 4);
                }
                else if (opcode == 0x4Au)
                {
                    // MPG: recorded with its microcode by mpg()
                }
                else if (opcode != 0x00u && opcode != 0x10u && opcode != 0x11u && opcode != 0x13u && opcode != 0x50u && opcode != 0x51u)
                    fail(vifcodeName(opcode)); // not NOP / FLUSH* / DIRECT*: STMOD / MSKPATH3 / MARK / MSCNT / ...
            }

            // step 5e: a VU1 microcode upload - replayed through the interpreter (the same compare, copy and decode-cache
            // invalidation as the packet path's)
            void mpg(uint32_t vifcode, const uint8_t *data, uint32_t bytes, bool complete) override
            {
                if (!rawOps())
                    return; // (refused in other())
                if (!complete)
                {
                    fail("MPG cut off");
                    return;
                }
                Op &op = rec->ops.emplace_back();
                op.type = Op::Raw;
                const auto *c = reinterpret_cast<const uint8_t *>(&vifcode);
                op.data.assign(c, c + 4);
                op.data.insert(op.data.end(), data, data + bytes);
            }

            static const char *vifcodeName(uint32_t opcode)
            {
                static char names[128][16];
                std::snprintf(names[opcode & 0x7Fu], sizeof(names[0]), "VIFcode %02x", opcode & 0x7Fu);
                return names[opcode & 0x7Fu];
            }
        };

        std::unordered_map<uint64_t, Recording> g_recordings; // by (kind, address, length)
        struct Stats
        {
            uint64_t blocks = 0, replayed = 0, recorded = 0, notReplayable = 0, changing = 0;
        } g_stats[KindCount];
        uint64_t g_packets = 0, g_verifySame = 0, g_verifyDiffer = 0, g_unmarked = 0, g_rasterMarks = 0;
        std::map<const char *, uint64_t> g_refusals;
        auto g_reportAt = ps2x::hostNow();

        uint64_t hashBlock(const uint8_t *p, uint32_t bytes)
        {
            // four lanes: the model packets are most of the chain's bytes
            uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ bytes, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
            uint32_t i = 0;
            for (; i + 32u <= bytes; i += 32u)
                for (int l = 0; l < 4; ++l)
                {
                    uint64_t w;
                    std::memcpy(&w, p + i + l * 8u, 8);
                    h[l] = (h[l] ^ w) * 0xFF51AFD7ED558CCDull;
                    h[l] ^= h[l] >> 29;
                }
            for (; i + 8u <= bytes; i += 8u)
            {
                uint64_t w;
                std::memcpy(&w, p + i, 8);
                h[0] = (h[0] ^ w) * 0xFF51AFD7ED558CCDull;
                h[0] ^= h[0] >> 29;
            }
            return ((h[0] * 31u + h[1]) * 31u + h[2]) * 31u + h[3];
        }

        // process the block as the packet path would, recording what it does
        void record(PS2Memory &memory, const uint8_t *bytes, uint32_t size, Recording &rec)
        {
            rec.ops.clear();
            rec.mode = memory.vif1_regs.mode;
            rec.cycle = memory.vif1_regs.cycle;
            Recorder recorder;
            recorder.rec = &rec;
            ps2chain::setVifRecorder(&recorder);
            memory.processVIF1Data(bytes, size);
            ps2chain::setVifRecorder(nullptr);
            lcs::hleworld::setMeshCapture(nullptr);
            rec.replayable = !recorder.failed;
            rec.refused = recorder.failed;
            rec.setsMask = recorder.maskSet;
            rec.setsRow = recorder.rowSet;
            rec.setsCycle = recorder.cycleSet;
            rec.mask = memory.vif1_regs.mask;
            std::memcpy(rec.row, memory.vif1_regs.row, sizeof(rec.row));
            rec.finalCycle = memory.vif1_regs.cycle;
        }

        void replay(PS2Memory &memory, const Recording &rec)
        {
            uint8_t *vu = memory.getVU1Data();
            for (const Op &op : rec.ops)
            {
                if (op.type == Op::Mscal)
                {
                    lcs::hleworld::setMeshHint(op.mesh.valid ? &op.mesh : nullptr);
                    memory.vu1Mscal(op.startPc);
                    lcs::hleworld::setMeshHint(nullptr);
                    ++g_packets;
                }
                else if (op.type == Op::Raw) // step 5e: BASE / OFFSET / ITOP / MPG, through the interpreter as they were
                    memory.processVIF1Data(op.data.data(), static_cast<uint32_t>(op.data.size()));
                else if (op.type == Op::Direct)
                {
                    if (op.rasterBytes) // the texture keeps its name when its upload comes from a recording
                        gsHwSetUploadSource(op.rasterAddress, op.rasterBytes);
                    memory.submitVif1DirectData(op.data.data(), op.qwords, op.directHl);
                }
                else
                {
                    const uint32_t at = op.topRelative ? (op.vuAddr + memory.vif1_regs.tops) & 0x3FFu : op.vuAddr;
                    // one copy when the run does not wrap around VU memory; masked lanes as one blend per qword
                    if (op.lanes == 0xFu && (at & 0x3FFu) + op.qwords <= 0x400u)
                        std::memcpy(vu + (at & 0x3FFu) * 16u, op.data.data(), op.qwords * 16u);
                    else if (op.lanes == 0xFu)
                        for (uint32_t i = 0; i < op.qwords; ++i)
                            std::memcpy(vu + ((at + i) & 0x3FFu) * 16u, op.data.data() + i * 16u, 16);
                    else
                    {
                        const __m128i keep = _mm_setr_epi32((op.lanes & 1u) ? 0 : -1, (op.lanes & 2u) ? 0 : -1,
                                                            (op.lanes & 4u) ? 0 : -1, (op.lanes & 8u) ? 0 : -1);
                        for (uint32_t i = 0; i < op.qwords; ++i)
                        {
                            auto *dst = reinterpret_cast<__m128i *>(vu + ((at + i) & 0x3FFu) * 16u);
                            const __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i *>(op.data.data() + i * 16u));
                            _mm_storeu_si128(dst, _mm_or_si128(_mm_and_si128(keep, _mm_loadu_si128(dst)), _mm_andnot_si128(keep, src)));
                        }
                    }
                }
            }
            if (rec.setsMask)
                memory.vif1_regs.mask = rec.mask;
            if (rec.setsRow)
                std::memcpy(memory.vif1_regs.row, rec.row, sizeof(rec.row));
            if (rec.setsCycle)
                memory.vif1_regs.cycle = rec.finalCycle;
        }

        bool sameLanes(const Op &x, const Op &y)
        {
            if (x.lanes == 0xFu)
                return x.data == y.data;
            for (uint32_t i = 0; i < x.qwords; ++i)
                for (uint32_t f = 0; f < 4u; ++f)
                    if (((x.lanes >> f) & 1u) && std::memcmp(x.data.data() + i * 16u + f * 4u, y.data.data() + i * 16u + f * 4u, 4) != 0)
                        return false;
            return true;
        }

        // the cached recording against a fresh one of the same block; a mesh is compared when both drew the packet (whether
        // a packet is culled / clipped depends on the camera, not on the block)
        bool same(const Recording &a, const Recording &b, Kind kind)
        {
            const char *what = nullptr;
            size_t at = 0;
            if (a.ops.size() != b.ops.size())
                what = "op count";
            else if (a.setsMask != b.setsMask || a.setsRow != b.setsRow || a.setsCycle != b.setsCycle ||
                     (a.setsMask && a.mask != b.mask) || (a.setsRow && std::memcmp(a.row, b.row, sizeof(a.row)) != 0) ||
                     (a.setsCycle && a.finalCycle != b.finalCycle))
                what = "final mask / row / cycle";
            for (size_t i = 0; !what && i < a.ops.size(); ++i)
            {
                const Op &x = a.ops[i], &y = b.ops[i];
                at = i;
                if (x.type != y.type || x.startPc != y.startPc || x.directHl != y.directHl)
                    what = "op kind";
                else if (x.topRelative != y.topRelative || x.vuAddr != y.vuAddr || x.qwords != y.qwords || x.lanes != y.lanes)
                    what = "unpack address / size / fields";
                else if (x.type == Op::Direct || x.type == Op::Raw ? x.data != y.data : !sameLanes(x, y))
                    what = x.type == Op::Direct ? "direct data" : x.type == Op::Raw ? "raw VIFcodes / microcode" : "unpack data";
                else if (x.mesh.valid && y.mesh.valid && (x.mesh.key != y.mesh.key || x.mesh.vertices != y.mesh.vertices))
                    what = "mesh";
            }
            if (what)
            {
                static int logged = 0;
                if (logged++ < 12)
                {
                    std::fprintf(stderr, "[rsl-world] verify: %s block differs in %s (op %zu of %zu / %zu)", kKindNames[kind], what, at,
                                 a.ops.size(), b.ops.size());
                    if (std::strcmp(what, "unpack data") == 0)
                    {
                        const Op &x = a.ops[at], &y = b.ops[at];
                        size_t q = 0;
                        while (q * 16u < x.data.size() && std::memcmp(x.data.data() + q * 16u, y.data.data() + q * 16u, 16) == 0)
                            ++q;
                        uint32_t u[4], v[4];
                        std::memcpy(u, x.data.data() + q * 16u, 16);
                        std::memcpy(v, y.data.data() + q * 16u, 16);
                        std::fprintf(stderr, ": unpack at %03x%s, %u qwords, first differing qword %zu: cached %08x %08x %08x %08x now %08x %08x %08x %08x",
                                     x.vuAddr, x.topRelative ? "+TOP" : "", x.qwords, q, u[0], u[1], u[2], u[3], v[0], v[1], v[2], v[3]);
                    }
                    std::fprintf(stderr, "\n");
                }
            }
            return !what;
        }

        void reportItems(); // step 4c

        // [lcs] the F2 overlay's copy of the last report (overlayStats); the setup counts are run totals
        std::mutex g_overlayMutex;
        OverlayStats g_overlay;
        std::atomic<uint64_t> g_setupsCompared{0}, g_setupsDiffer{0}, g_setupsEmitted{0};
        bool g_lcsInstalled = false;

        void publishOverlay()
        {
            OverlayStats o;
            o.active = true;
            o.replay = !g_verify;
            const Stats &s = g_stats[OtherCall];
            o.sectorBlocks = s.blocks;
            o.sectorReplayed = s.replayed;
            o.sectorRecorded = s.recorded;
            o.sectorNotReplayable = s.notReplayable;
            o.skipped = g_skipSeen.load();
            o.skipFallback = g_skipFallback.load();
            o.setupsCompared = g_setupsCompared.load(std::memory_order_relaxed);
            o.setupsDiffer = g_setupsDiffer.load(std::memory_order_relaxed);
            o.setupsEmitted = g_setupsEmitted.load(std::memory_order_relaxed);
            o.chainKb = g_measure.chains ? g_measure.chainBytes / double(g_measure.chains) / 1024.0 : 0.0;
            std::lock_guard<std::mutex> lock(g_overlayMutex);
            g_overlay = o;
        }

        void report()
        {
            const auto now = ps2x::hostNow();
            if (!g_log || now - g_reportAt < std::chrono::seconds(10))
                return;
            g_reportAt = now;
            if (g_lcsInstalled)
                publishOverlay(); // before the counters below are reset
            for (int k = 0; k < KindCount; ++k)
            {
                const Stats &s = g_stats[k];
                if (s.blocks)
                    std::fprintf(stderr, "[rsl-world] %s: %llu blocks: %llu replayed, %llu recorded, %llu not replayable, %llu changing\n",
                                 kKindNames[k], (unsigned long long)s.blocks, (unsigned long long)s.replayed, (unsigned long long)s.recorded,
                                 (unsigned long long)s.notReplayable, (unsigned long long)s.changing);
                g_stats[k] = Stats{};
            }
            std::fprintf(stderr, "[rsl-world] %llu packets replayed, %llu marks without a length, %llu raster uploads named; %zu recordings\n",
                         (unsigned long long)g_packets, (unsigned long long)g_unmarked, (unsigned long long)g_rasterMarks, g_recordings.size());
            if (!g_refusals.empty())
            {
                std::fprintf(stderr, "[rsl-world] not replayable:");
                for (const auto &[why, n] : g_refusals)
                    std::fprintf(stderr, " %s %llu", why, (unsigned long long)n);
                std::fprintf(stderr, "\n");
            }
            if (g_verify)
                std::fprintf(stderr, "[rsl-world] verify: %llu recordings the same as the packet path, %llu differ\n",
                             (unsigned long long)g_verifySame, (unsigned long long)g_verifyDiffer);
            g_packets = g_verifySame = g_verifyDiffer = g_unmarked = g_rasterMarks = 0;
            if (g_cleanStats.blocks) // (game thread counts; read here only for the log)
            {
                std::fprintf(stderr, "[rsl-world] write watch: %llu blocks copied, %llu (%.1f%%) unchanged since their last copy, %llu seen for the first time\n",
                             (unsigned long long)g_cleanStats.blocks, (unsigned long long)g_cleanStats.clean,
                             100.0 * g_cleanStats.clean / double(g_cleanStats.blocks), (unsigned long long)g_cleanStats.unseen);
                g_cleanStats = CleanStats{};
            }
            if (g_skipVerify)
                std::fprintf(stderr, "[rsl-world] skip verify: %llu blocks would have replayed their recording rightly, %llu wrongly\n",
                             (unsigned long long)g_skipVerifySame.exchange(0), (unsigned long long)g_skipVerifyDiffer.exchange(0));
            if (g_skipped.load() || g_skipSeen.load())
            {
                std::fprintf(stderr, "[rsl-world] skip marks seen %llu, without a table entry %llu\n", (unsigned long long)g_skipSeen.exchange(0),
                             (unsigned long long)g_skipLost.exchange(0));
                std::fprintf(stderr, "[rsl-world] skipped (not copied): %llu blocks, %llu replayed, %llu from guest RAM\n",
                             (unsigned long long)g_skipped.exchange(0), (unsigned long long)g_skipReplayed.exchange(0),
                             (unsigned long long)g_skipFallback.exchange(0));
            }
            if (g_lcsInstalled && g_skip)
            {
                std::fprintf(stderr, "[lcs-rsl] admission: missing snapshot %llu, dirty pages %llu, unconfirmed %llu, "
                                     "tag commands %llu, unsupported block %llu\n",
                             (unsigned long long)g_lcsNoSnapshot.exchange(0), (unsigned long long)g_lcsDirty.exchange(0),
                             (unsigned long long)g_lcsUnconfirmed.exchange(0), (unsigned long long)g_lcsTagCommands.exchange(0),
                             (unsigned long long)g_lcsUnsupported.exchange(0));
                for (const Kind kind : {World, Model, ModelB, OtherCall})
                    std::fprintf(stderr, "[lcs-rsl] %s: %llu items executed without DMA copying, %llu bytes bypassed\n",
                                 kKindNames[kind], (unsigned long long)g_lcsItemCount[kind].exchange(0),
                                 (unsigned long long)g_lcsItemBytes[kind].exchange(0));
            }
            if (g_lcsInstalled && !g_verify)
            {
                std::fprintf(stderr, "[lcs-rsl] emitter: %llu draw slots submitted, %llu executed without DMA copying; "
                                     "%llu bytes prepared for confirmed blocks\n",
                             (unsigned long long)g_lcsProducerSlots.exchange(0),
                             (unsigned long long)g_lcsProducerExecuted.exchange(0),
                             (unsigned long long)g_lcsPreparedBytes.exchange(0));
                std::fprintf(stderr, "[lcs-rsl] worker state items: %llu, including %llu texture payloads\n",
                             (unsigned long long)g_lcsStateCount.exchange(0),
                             (unsigned long long)g_lcsUploadCount.exchange(0));
            }
            reportItems();
            if (g_measure.chains)
            {
                const double c = double(g_measure.chains);
                std::fprintf(stderr, "[rsl-world] per chain: %.0f KB copied, %.0f marked blocks (%.0f KB), %.0f distinct (%.0f KB)\n",
                             g_measure.chainBytes / c / 1024.0, g_measure.blocks / c, g_measure.blockBytes / c / 1024.0, g_measure.distinct / c,
                             g_measure.distinctBytes / c / 1024.0);
                g_measure = Measure{};
            }
            g_refusals.clear();
        }

        uint64_t recordingKey(Kind kind, uint32_t addr, uint32_t units)
        {
            return (uint64_t(kind) << 56) | (uint64_t(addr & kRamMask) << 24) | (units & 0xFFFFFFu);
        }

        // a CALLed packet flattened from guest RAM as the chain walker copies it (each tag's upper half, then its payload) up
        // to the RET that returns from it - step 4's fallback for a skipped block whose recording cannot be used
        bool flattenCall(const uint8_t *rdram, uint32_t target, std::vector<uint8_t> &out)
        {
            out.clear();
            uint32_t tag = target & kRamMask, ret[2] = {}, depth = 0;
            for (int n = 0; n < 65536; ++n)
            {
                if ((tag & 0xF0000000u) == 0x70000000u || tag + 16u > kRamMask + 1u)
                    return false;
                uint64_t lo;
                std::memcpy(&lo, rdram + tag, 8);
                out.insert(out.end(), rdram + tag + 8, rdram + tag + 16);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu), id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>(lo >> 32) & kRamMask;
                const uint32_t after = tag + 16u;
                const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : after;
                if (qwc)
                {
                    if ((data & 0xF0000000u) == 0x70000000u || (data & kRamMask) + qwc * 16u > kRamMask + 1u)
                        return false;
                    out.insert(out.end(), rdram + (data & kRamMask), rdram + (data & kRamMask) + qwc * 16u);
                }
                switch (id)
                {
                case 1: tag = after + qwc * 16u; break;                  // cnt
                case 2: tag = addr; break;                               // next
                case 3: case 4: tag = after; break;                      // ref, refs
                case 5:                                                  // call
                    if (depth >= 2u)
                        return false;
                    ret[depth++] = after + qwc * 16u;
                    tag = addr;
                    break;
                case 6:                                                  // ret: from a nested call, or out of the packet
                    if (depth == 0u)
                        return true;
                    tag = ret[--depth];
                    break;
                default: return false;                                   // refe / end inside a CALLed packet
                }
            }
            return false;
        }

        uint32_t handleBlock(PS2Memory &memory, const Draw &d, uint64_t seq, const uint8_t *bytes, uint32_t size, uint32_t units, uint64_t confirmedHash = 0);

        // step 4: a block the walker copied no bytes of - its recording, or (cannot be used) its bytes from guest RAM, which
        // by the game thread's write watch still hold what the recording was made from
        void replaySkipped(PS2Memory &memory, const Draw &d, uint64_t seq)
        {
            const uint64_t key = recordingKey(d.kind, d.addr, d.units);
            const auto it = g_recordings.find(key);
            if (it != g_recordings.end())
            {
                Recording &rec = it->second;
                if (!rec.ops.empty() && rec.replayable && rec.mode == memory.vif1_regs.mode && rec.cycle == memory.vif1_regs.cycle)
                {
                    replay(memory, rec);
                    ++g_stats[d.kind].blocks;
                    ++g_stats[d.kind].replayed;
                    ++g_skipReplayed;
                    t_confirm.push_back({key, seq});
                    return;
                }
            }
            ++g_skipFallback;
            if (d.kind == World)
            {
                handleBlock(memory, d, seq, memory.getRDRAM() + (d.addr & kRamMask), d.units * 8u, d.units);
                return;
            }
            thread_local std::vector<uint8_t> flat;
            if (flattenCall(memory.getRDRAM(), d.addr, flat) && flat.size() == size_t(d.units) * 8u)
                handleBlock(memory, d, seq, flat.data(), static_cast<uint32_t>(flat.size()), d.units);
        }

        // step 4c: an item's MARK - its VIF bytes (the flush, the GS registers), then its block
        void onItem(PS2Memory &memory, uint32_t index, uint32_t frame);

        // the MARK before a marked REF's payload / CALL's packet (the processing thread, in stream order); the marked
        // bytes follow the MARK qword: their length (8-byte units) in the word after the MARK, then two NOPs
        uint32_t onMark(PS2Memory &memory, uint32_t imm, const uint8_t *next, uint32_t avail)
        {
            if (imm >= kItemMark && avail >= 4u)
            {
                uint32_t w;
                std::memcpy(&w, next, 4);
                if ((w & 0xFFFF0000u) == kItemNop)
                {
                    onItem(memory, imm - kItemMark, w & 0xFFFFu);
                    return 4u; // its NOP
                }
            }
            const ChainTable *table = t_table.get();
            uint32_t signature = 0u;
            if (avail >= 12u)
                std::memcpy(&signature, next + 4u, sizeof(signature));
            if (signature != ps2chain::kCopiedMarkNop)
                return 0u; // a MARK in the original guest stream
            if (avail >= 4u) // step 4: skipped marks the worker cannot place (their block would be lost)
            {
                uint32_t w;
                std::memcpy(&w, next, 4);
                if ((w & 0x00FFFFFFu) == ps2chain::kSkippedLength && (!table || imm >= table->draws.size()))
                    ++g_skipLost;
            }
            uint32_t length = 0u;
            if (avail >= 12u)
                std::memcpy(&length, next, sizeof(length));
            if (table && length == ps2chain::kStateItemLength)
            {
                if (imm < table->items.size())
                {
                    ++g_lcsStateCount;
                    if (table->items[imm].prefixRasterCount)
                        ++g_lcsUploadCount;
                }
                onItem(memory, imm, table->itemFrame & 0xFFFFu);
                return 12u;
            }
            if (!table || imm >= table->draws.size() || avail < 12u)
                return 0u; // a MARK the game wrote itself
            const Draw &d = table->draws[imm];
            uint32_t units;
            std::memcpy(&units, next, 4);
            units &= 0x00FFFFFFu;
            if (units == ps2chain::kSkippedLength) // step 4: no bytes copied
            {
                ++g_skipSeen;
                const auto item = table->lcsDrawItems.find(imm);
                if (item != table->lcsDrawItems.end())
                    onItem(memory, item->second, table->itemFrame & 0xFFFFu);
                else
                    replaySkipped(memory, d, table->seq);
                report();
                return 12u;
            }
            const uint32_t size = units * 8u;
            if (!size || size > avail - 12u)
            {
                ++g_unmarked; // a CALL whose RET the chain did not reach: the packet path runs on
                return 0u;
            }
            return 12u + handleBlock(memory, d, table->seq, next + 12u, size, units);
        }

        std::atomic<uint64_t> g_itemsSeenW{0}, g_itemsUnplacedW{0}, g_itemBlockSameW{0}, g_itemBlockDifferW{0};

        std::atomic<uint64_t> g_itemsStaleW{0};
        void onItem(PS2Memory &memory, uint32_t index, uint32_t frame)
        {
            const ChainTable *table = t_table.get();
            thread_local int depth = 0;
            if (table && (frame != (table->itemFrame & 0xFFFFu) || depth > 0)) // (an item inside an item's block: copied too)
            {
                ++g_itemsStaleW; // another frame's tag (the game copied it) - never drawn from this frame's items
                static int logged = 0;
                if (logged++ < 10)
                    std::fprintf(stderr, "[rsl-items] a MARK of frame %u in frame %u's chain (index %u, depth %d): not drawn\n", frame,
                                 table->itemFrame & 0xFFFFu, index, depth);
                return;
            }
            if (!table || index >= table->items.size())
            {
                ++g_itemsUnplacedW;
                return;
            }
            ++g_itemsSeenW;
            ++depth;
            struct Leave
            {
                int &d;
                ~Leave() { --d; }
            } leave{depth};
            for (;; ++index) // step 5f: an item, then the items that continue it
            {
            const Item &it = table->items[index];
            if (it.prefixBytes)
            {
                t_block = table->arena.data() + it.prefixAt; // step 5b: the uploads in it keep their raster names
                t_blockBytes = it.prefixBytes;
                t_blockRasters = it.prefixRasterCount ? &table->itemRasters[it.prefixRastersAt] : nullptr;
                t_blockRasterCount = it.prefixRasterCount;
                memory.processVIF1Data(t_block, it.prefixBytes);
                t_block = nullptr;
                t_blockBytes = 0;
                t_blockRasters = nullptr;
                t_blockRasterCount = 0;
            }
            if (it.skip)
                replaySkipped(memory, it.d, table->seq);
            else if (it.confirmed || it.blockBytes)
            {
                if (it.confirmed)
                {
                    const auto &block = *it.confirmed;
                    t_block = block.bytes.data();
                    t_blockBytes = static_cast<uint32_t>(block.bytes.size());
                    t_blockRasters = block.rasters.data();
                    t_blockRasterCount = static_cast<uint32_t>(block.rasters.size());
                    ++g_lcsItemCount[it.d.kind];
                    if (it.d.producerSlot)
                        ++g_lcsProducerExecuted;
                    g_lcsItemBytes[it.d.kind].fetch_add(t_blockBytes, std::memory_order_relaxed);
                }
                else
                {
                    t_block = table->arena.data() + it.blockAt;
                    t_blockBytes = it.blockBytes;
                    t_blockRasters = it.rasterCount ? &table->itemRasters[it.rastersAt] : nullptr;
                    t_blockRasterCount = it.rasterCount;
                }
                handleBlock(memory, it.d, table->seq, t_block, t_blockBytes, t_blockBytes / 8u,
                            it.confirmed ? it.confirmed->hash : 0u);
                t_block = nullptr;
                t_blockBytes = 0;
                t_blockRasters = nullptr;
                t_blockRasterCount = 0;
            }
            if (!it.continued || index + 1u >= table->items.size())
                break;
            }
            report();
        }

        // a marked block's bytes: replayed from its recording, recorded, or processed by the packet path; returns size
        uint32_t handleBlock(PS2Memory &memory, const Draw &d, uint64_t seq, const uint8_t *bytes, uint32_t size, uint32_t units, uint64_t confirmedHash)
        {
            if (d.flatHash) // LCS_RSL_ITEMS_VERIFY: an item would have copied these bytes?
            {
                const bool same = hashBlock(bytes, size) == d.flatHash;
                ++(same ? g_itemBlockSameW : g_itemBlockDifferW);
                static int logged = 0;
                if (!same && logged++ < 10)
                    std::fprintf(stderr, "[rsl-items] verify: %s block at 0x%08x (%u bytes) copied otherwise than flattenBlock copies it\n",
                                 kKindNames[d.kind], d.addr, size);
            }
            static const bool measure = std::getenv("LCS_RSL_MEASURE") != nullptr;
            if (measure)
            {
                ++g_measure.blocks;
                g_measure.blockBytes += size;
                if (t_seen.emplace((uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units, size).second)
                {
                    ++g_measure.distinct;
                    g_measure.distinctBytes += size;
                }
            }
            Stats &stats = g_stats[d.kind];
            ++stats.blocks;
            Recording &rec = g_recordings[(uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units];
            if (rec.skip && !g_verify)
            {
                --rec.skip;
                memory.processVIF1Data(bytes, size);
                ++(rec.replayable ? stats.changing : stats.notReplayable);
                return size; // (onMark adds the MARK qword's 12 bytes: returning them here too swallowed the next tag)
            }
            const uint64_t hash = confirmedHash ? confirmedHash : hashBlock(bytes, size);
            if (d.wouldSkip) // LCS_RSL_SKIP_VERIFY: the skip would have replayed the recording of these bytes?
            {
                const auto it = g_recordings.find(recordingKey(d.kind, d.addr, d.units));
                const bool same = units == d.units && it != g_recordings.end() && it->second.hash == hash && !it->second.ops.empty();
                ++(same ? g_skipVerifySame : g_skipVerifyDiffer);
                static int logged = 0;
                if (!same && logged++ < 10)
                    std::fprintf(stderr, "[rsl-world] skip verify: %s block at 0x%08x would have replayed %s\n", kKindNames[d.kind], d.addr,
                                 units != d.units ? "another length" : it == g_recordings.end() ? "no recording" : "a recording of other bytes");
            }
            const bool valid = rec.hash == hash && !rec.ops.empty() && rec.mode == memory.vif1_regs.mode &&
                               rec.cycle == memory.vif1_regs.cycle;
            const uint64_t key = (uint64_t(d.kind) << 56) | (uint64_t(d.addr & kRamMask) << 24) | units;
            if (valid && rec.replayable && !g_verify)
            {
                replay(memory, rec);
                rec.changes = 0;
                ++stats.replayed;
                if (g_skip)
                    t_confirm.push_back({key, seq});
            }
            else if (valid && rec.replayable && g_verify)
            {
                Recording fresh;
                fresh.hash = hash;
                record(memory, bytes, size, fresh);
                (same(rec, fresh, d.kind) ? g_verifySame : g_verifyDiffer) += 1;
            }
            else if (valid)
            {
                memory.processVIF1Data(bytes, size); // recorded before: not replayable
                rec.skip = 255;
                ++stats.notReplayable;
                ++g_refusals[rec.refused];
            }
            else if (rec.changes >= 3 && !rec.probe && !g_verify)
            {
                memory.processVIF1Data(bytes, size); // rebuilt every frame: recording it costs more than it saves
                rec.skip = 63;
                rec.probe = true;
                ++stats.changing;
            }
            else
            {
                if (!rec.ops.empty())
                    ++rec.changes;
                rec.probe = false;
                rec.hash = hash;
                record(memory, bytes, size, rec);
                ++stats.recorded;
                if (g_skip && rec.replayable)
                    t_confirm.push_back({key, seq});
            }
            report();
            return size;
        }

        // a DIRECT's data on the processing thread: a raster upload's pixels tell the renderer whose they are (step 3d)
        void onDirect(const uint8_t *data, uint32_t bytes)
        {
            if (const RasterRef *r = rasterAt(data, bytes))
            {
                gsHwSetUploadSource(r->address, r->bytes);
                ++g_rasterMarks;
            }
        }

        // ---- the game thread ----------------------------------------------------------------------------------------
        // Step 4 (docs/FULLY_NATIVE.md): the guest pages a marked block is made of - a world REF's payload, a CALLed
        // packet's tags and payloads - with their write-watch generations when it was last copied. Unchanged pages: the
        // block holds the bytes it held then (runtime/ps2_write_watch.h).
        struct PageSnapshot
        {
            std::vector<std::pair<uint32_t, uint32_t>> pages; // page, generation
            bool usable = false;                                // only RAM pages, all seen
            uint32_t units = 0;                                 // its length as copied (8-byte units)
            uint64_t stableSince = 0;                           // the copy (g_chainSeq) since which its pages are unchanged
        };
        // [perf] Open addressing for the two maps the skip test reads per marked block (~2,000 a frame): a probe in one
        // array instead of std::unordered_map's bucket list (rslworld's `skippable` was 2.4% of the game thread,
        // PERFORMANCE.md 23). Only lookups and inserts (nothing is erased); a key of ~0 is never used (kind << 56).
        template <typename V>
        class FlatMap
        {
        public:
            V *find(uint64_t key)
            {
                if (m_keys.empty())
                    return nullptr;
                for (size_t i = slot(key);; i = (i + 1) & m_mask)
                {
                    if (m_keys[i] == key)
                        return &m_values[i];
                    if (m_keys[i] == kEmpty)
                        return nullptr;
                }
            }
            V &operator[](uint64_t key)
            {
                if ((m_count + 1) * 2 > m_keys.size())
                    grow();
                for (size_t i = slot(key);; i = (i + 1) & m_mask)
                {
                    if (m_keys[i] == key)
                        return m_values[i];
                    if (m_keys[i] == kEmpty)
                    {
                        m_keys[i] = key;
                        ++m_count;
                        return m_values[i];
                    }
                }
            }

        private:
            static constexpr uint64_t kEmpty = ~0ull;
            size_t slot(uint64_t key) const
            {
                key ^= key >> 33; // (murmur3's finaliser)
                key *= 0xFF51AFD7ED558CCDull;
                key ^= key >> 33;
                return static_cast<size_t>(key) & m_mask;
            }
            void grow()
            {
                std::vector<uint64_t> keys(std::max<size_t>(1024u, m_keys.size() * 2u), kEmpty);
                std::vector<V> values(keys.size());
                std::swap(keys, m_keys);
                std::swap(values, m_values);
                m_mask = m_keys.size() - 1u;
                m_count = 0;
                for (size_t i = 0; i < keys.size(); ++i)
                    if (keys[i] != kEmpty)
                        (*this)[keys[i]] = std::move(values[i]);
            }
            std::vector<uint64_t> m_keys;
            std::vector<V> m_values;
            size_t m_mask = 0, m_count = 0;
        };

        FlatMap<uint64_t> g_confirmed; // recording key -> latest chain the worker confirmed it in
        FlatMap<PageSnapshot> g_snapshots;
        struct OpenBlock
        {
            bool open = false;
            uint64_t key = 0;
            int depth = 0;
            bool usable = true;
            uint32_t units = 0; // bytes copied so far / 8
            std::vector<uint32_t> pages;
        } g_openBlock;

        bool unchanged(const PageSnapshot &snap);

        // step 4: may this block go uncopied? Its pages unchanged since a copy the worker confirmed its recording for.
        // The fill-in for snapshotKey's Draw: its length as last copied.
        bool skippable(uint64_t snapshotKey, Kind kind, uint32_t addr, uint32_t &units)
        {
            // LCS_RSL_SKIP_KINDS: a bit per Kind (default all)
            static const unsigned kinds = std::getenv("LCS_RSL_SKIP_KINDS") ? unsigned(std::strtoul(std::getenv("LCS_RSL_SKIP_KINDS"), nullptr, 0)) : 0xFu;
            if (!g_skip || !((kinds >> kind) & 1u))
                return false;
            const PageSnapshot *snap = g_snapshots.find(snapshotKey);
            if (!snap || !unchanged(*snap))
                return false;
            const uint64_t *c = g_confirmed.find(recordingKey(kind, addr, snap->units));
            if (!c || *c < snap->stableSince)
                return false;
            units = snap->units;
            return true;
        }

        uint64_t blockKey(Kind kind, uint32_t addr, uint32_t qwc)
        {
            return (uint64_t(kind) << 56) | (uint64_t(addr & kRamMask) << 24) | (qwc & 0xFFFFFFu);
        }

        void addPages(std::vector<uint32_t> &pages, bool &usable, uint32_t addr, uint32_t bytes)
        {
            if ((addr & 0xF0000000u) == 0x70000000u) // scratchpad: not watched
            {
                usable = false;
                return;
            }
            addr &= kRamMask;
            for (uint32_t p = addr >> ps2writewatch::kPageShift; bytes && p <= (addr + bytes - 1u) >> ps2writewatch::kPageShift; ++p)
                pages.push_back(p);
        }

        // a marked block's page set complete: was it unchanged since its last copy? then remember this copy's
        bool unchanged(const PageSnapshot &snap)
        {
            if (!snap.usable)
                return false;
            for (const auto &[page, gen] : snap.pages)
                if (!ps2writewatch::unchangedSince(page, gen))
                    return false;
            return true;
        }

        void closeBlock(uint64_t key, std::vector<uint32_t> &pages, bool usable, uint32_t units)
        {
            ++g_cleanStats.blocks;
            PageSnapshot &snap = g_snapshots[key];
            const bool clean = unchanged(snap) && snap.units == units;
            if (snap.usable)
                g_cleanStats.clean += clean ? 1u : 0u;
            else
                ++g_cleanStats.unseen;
            if (!clean)
                snap.stableSince = g_chainSeq; // this copy is the first of the bytes it holds now
            snap.units = units;
            std::sort(pages.begin(), pages.end());
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
            snap.pages.clear();
            for (uint32_t page : pages)
                snap.pages.push_back({page, ps2writewatch::generation(page)});
            snap.usable = usable && ps2writewatch::enabled();
        }

        void noteFlattenable(uint64_t key, uint32_t units); // step 4c

        // every tag of the chain, while a CALLed packet is open: its pages (the walker copies the tag's upper half and the
        // payload); the matching RET closes it
        void trackOpenBlock(uint32_t tagAddr, uint32_t id, uint32_t dataAddr, uint32_t qwc)
        {
            if (!g_openBlock.open)
                return;
            addPages(g_openBlock.pages, g_openBlock.usable, tagAddr, 16u);
            if (dataAddr && qwc)
                addPages(g_openBlock.pages, g_openBlock.usable, dataAddr, qwc * 16u);
            g_openBlock.units += 1u + qwc * 2u; // the tag's upper half and its payload, as the walker copies them
            if (id == 5u)
                ++g_openBlock.depth;
            else if (id == 6u && --g_openBlock.depth == 0)
            {
                g_openBlock.open = false;
                closeBlock(g_openBlock.key, g_openBlock.pages, g_openBlock.usable, g_openBlock.units);
                if (g_openBlock.usable)
                    noteFlattenable(g_openBlock.key, g_openBlock.units); // step 4c
            }
            else if (id == 0u || id == 7u) // the chain ended inside it
                g_openBlock.open = false;
        }
        // ---- step 4c: draw items, the game thread ----------------------------------------------------------------
        bool g_items = false, g_itemVerify = false, g_ops = false;
        const uint8_t *g_rdram = nullptr;
        std::vector<Item> g_itemsCurrent, g_itemsKicked;       // this frame's items / the frame whose chain is being copied
        std::vector<uint8_t> g_arenaCurrent, g_arenaKicked;    // their VIF bytes
        // the tables' arenas / item lists go back to the game thread after processing (a fresh one each frame cost page
        // faults on ~1 MB a frame)
        std::mutex g_poolMutex;
        std::vector<std::vector<uint8_t>> g_arenaPool;
        std::vector<std::vector<Item>> g_itemPool;
        template <typename T>
        void takePooled(std::vector<std::vector<T>> &pool, std::vector<T> &into)
        {
            if (into.capacity())
                return;
            std::lock_guard<std::mutex> lock(g_poolMutex);
            if (!pool.empty())
            {
                into = std::move(pool.back());
                pool.pop_back();
                into.clear();
            }
        }
        std::unordered_map<uint64_t, bool> g_flatOk;           // model blocks (blockKey): flattenBlock copies them as the walker does
        uint32_t g_itemFrame = 1, g_itemFrameKicked = 0;       // frames counted at the kick: whose items an item tag names
        bool g_caching = false; // FUN_00240ED8 is caching a sector's packets (they are copied and sent again): no items
        PS2Runtime::RecompiledFunction g_sectorOriginal = nullptr;
        struct ItemStats
        {
            uint64_t made[KindCount] = {}, flush = 0, stateCall = 0, unknown = 0, noRoom = 0, shape = 0, full = 0, caching = 0;
            uint64_t skipped = 0, copied = 0, lost = 0, verifySame = 0, verifyDiffer = 0, badRefs = 0;
        } g_itemStats;                                          // game thread

        constexpr uint32_t kPacketCursor = 0x520F90u + 0x18u; // the packet buffer's DMA cursor (game-internals.md 6i)
        constexpr uint32_t kDirty = 0x4882A0u, kGsQueue = 0x729510u, kGsQueueCount = 0x488308u;

        uint32_t rd32(const uint8_t *rdram, uint32_t addr)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (addr & kRamMask), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t addr, uint32_t v) { std::memcpy(rdram + (addr & kRamMask), &v, 4); }

        // DMA tags with inline data (CNT only) as the VIF stream the walker makes of them: each tag's upper half, then its
        // payload
        void tagsToVif(const uint8_t *tags, uint32_t size, std::vector<uint8_t> &out)
        {
            size_t at = out.size();
            out.resize(at + size); // at most the tags' size (each loses its lower half)
            for (uint32_t t = 0; t + 16u <= size;)
            {
                uint64_t lo;
                std::memcpy(&lo, tags + t, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu);
                std::memcpy(out.data() + at, tags + t + 8, 8 + qwc * 16u); // the upper half and the payload after it
                at += 8u + qwc * 16u;
                t += 16u + qwc * 16u;
            }
            out.resize(at);
        }

        // the draw's GS register packet as FUN_002F7D58 / FUN_002F80A0 / FUN_002F7EB0 write it (game-internals.md 6i): CNT
        // (n + 1), FLUSHE, DIRECTHL (n + 1), a GIFtag, the n queued {value, register} pairs - the queue in guest memory,
        // then the flush's own
        void gsPacket(const uint8_t *rdram, const lcs::rslnative::Flush &f, std::vector<uint8_t> &out)
        {
            const uint32_t inRam = rd32(rdram, kGsQueueCount), n = inRam + f.queued;
            if (n == 0u)
                return;
            const uint64_t head[4] = {uint64_t(n + 1u) | 0x10000000u, 0x10000000ull | (uint64_t((n + 1u) | 0x51000000u) << 32),
                                      uint64_t(n) | 0x1000000000008000ull, 0xEull};
            const auto *h = reinterpret_cast<const uint8_t *>(head);
            out.insert(out.end(), h, h + sizeof(head));
            out.insert(out.end(), rdram + kGsQueue, rdram + kGsQueue + inRam * 16u);
            const auto *q = reinterpret_cast<const uint8_t *>(f.queue);
            out.insert(out.end(), q, q + f.queued * 16u);
        }

        // the GS register packet as the VIF stream the walker makes of it (FLUSHE, DIRECTHL, then the payload), in one append
        void gsVif(const uint8_t *rdram, const lcs::rslnative::Flush &f, std::vector<uint8_t> &out)
        {
            const uint32_t inRam = rd32(rdram, kGsQueueCount), n = inRam + f.queued;
            if (n == 0u)
                return;
            const size_t at = out.size();
            out.resize(at + 8u + 16u + n * 16u);
            uint8_t *p = out.data() + at;
            const uint64_t head[3] = {0x10000000ull | (uint64_t((n + 1u) | 0x51000000u) << 32), uint64_t(n) | 0x1000000000008000ull, 0xEull};
            std::memcpy(p, head, sizeof(head));
            std::memcpy(p + 24, rdram + kGsQueue, inRam * 16u);
            std::memcpy(p + 24 + inRam * 16u, f.queue, f.queued * 16u);
        }

        // a CALLed packet flattened from guest RAM as the chain walker copies it (each tag's upper half, then its payload)
        // up to the RET that returns from it, with the pages it is made of (as trackOpenBlock notes them) and the raster
        // uploads in it (as observe finds them); false: not as the walker would copy it (scratchpad, nesting, no RET)
        bool flattenBlock(const uint8_t *rdram, uint32_t target, std::vector<uint8_t> &out, std::vector<uint32_t> &pages, bool &usable,
                          std::vector<RasterInBlock> *rasters, bool lcs = false)
        {
            const size_t start = out.size();
            uint32_t tag = target, ret[2] = {}, depth = 0;
            for (int n = 0; n < 65536; ++n)
            {
                if ((tag & 0xF0000000u) == 0x70000000u) // a tag in scratchpad (the walker reads it there)
                    return false;
                tag &= kRamMask;
                if (tag + 16u > kRamMask + 1u)
                    return false;
                uint64_t lo, hi;
                std::memcpy(&lo, rdram + tag, 8);
                std::memcpy(&hi, rdram + tag + 8, 8);
                if (lcs)
                    ps2chain::decodeLcsSubmissionSlot(lo, hi);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu), id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>(lo >> 32) & 0x7FFFFFFFu;
                if (lcs && (lo >> 63) && (id == 2u || id == 3u || id == 4u || id == 5u))
                    return false;
                const uint32_t after = tag + 16u;
                const uint32_t data = (id == 0u || id == 3u || id == 4u) ? addr : after;
                addPages(pages, usable, tag, 16u);
                const auto *codes = reinterpret_cast<const uint8_t *>(&hi);
                out.insert(out.end(), codes, codes + sizeof(hi));
                if (qwc)
                {
                    if ((data & 0xF0000000u) == 0x70000000u || (data & kRamMask) + qwc * 16u > kRamMask + 1u)
                        return false;
                    addPages(pages, usable, data, qwc * 16u);
                    const uint32_t code = static_cast<uint32_t>(hi >> 32);
                    if (rasters && g_rasters && id == 3u && ((code >> 24) & 0x7Eu) == 0x50u && (code & 0xFFFFu) == qwc &&
                        (!lcs || static_cast<uint32_t>(hi) == 0u))
                        rasters->push_back({static_cast<uint32_t>(out.size() - start), data & kRamMask, qwc * 16u});
                    out.insert(out.end(), rdram + (data & kRamMask), rdram + (data & kRamMask) + qwc * 16u);
                }
                switch (id)
                {
                case 1: tag = after + qwc * 16u; break; // cnt
                case 2: tag = addr; break;              // next
                case 3: case 4: tag = after; break;     // ref, refs
                case 5:                                 // call
                    if (depth >= 1u)                    // the walker's ASR holds two: this packet's return and one more
                        return false;
                    ret[depth++] = after + qwc * 16u;
                    tag = addr;
                    break;
                case 6: // ret: from a nested call, or out of the packet
                    if (depth == 0u)
                        return true;
                    tag = ret[--depth];
                    break;
                default: return false; // refe / end inside a CALLed packet
                }
            }
            return false;
        }

        // ---- step 5a: state ops ----------------------------------------------------------------------------------------
        // An item of VIF bytes alone (a setter's UNPACK, a GS register packet, a standalone flush), in chain order like a
        // draw's. Consecutive ones - and the draw after them - share one item and one tag while nothing else was written
        // to the packet buffer in between (the cursor is still right after the open item's tag).
        int g_openItem = -1;      // the last item, still taking ops (no block yet)
        uint32_t g_openEnd = 0;   // the cursor right after its tag
        bool g_materialCaching = false; // FUN_002409C8 is caching a material's setup (its packets are copied)
        bool g_materialCacheEnds = false; // its last packet written: the window ends at the next settle
        struct OpStats
        {
            uint64_t ops = 0, merged = 0, packets = 0, caching = 0, mergedDraws = 0, uploads = 0;
        } g_opStats;

        // step 5d: this hook runs inside an emitter being captured (deeper on the guest stack): its packets go into the
        // emitter's region as packets, never as ops of their own (settleIm2D sets it at every hook's entry)
        bool g_inCapture = false;
        bool opsAllowed() { return g_items && !g_itemVerify && !g_caching && !g_materialCaching && !g_inCapture; }

        // the half in use and the room for `bytes` at the cursor: false = the packet path
        bool cursorForItem(const uint8_t *rdram, uint32_t bytes, uint32_t &cursor)
        {
            if (!lcs::rslnative::packetRoom(rdram, bytes, cursor))
                return false;
            const uint32_t half = 0x520F90u + rd32(rdram, 0x520F90u + 0x14u) * 0xE6670u + 0x20u;
            return cursor >= half && cursor + bytes <= half + 0xE6670u;
        }

        uint32_t g_lastCursor = 0; // step 5h: the cursor after the last hook point / our last write (gaps start here)

        void writeItemTag(uint8_t *rdram, uint32_t cursor, uint32_t index, bool moveCursor = true)
        {
            const uint32_t tag[4] = {0x10000000u, 0u, 0x07000000u | (kItemMark + index), kItemNop | (g_itemFrame & 0xFFFFu)}; // CNT: MARK, NOP
            std::memcpy(rdram + (cursor & kRamMask), tag, 16);
            if (moveCursor)
            {
                wr32(rdram, kPacketCursor, cursor + 16u);
                g_lastCursor = cursor + 16u;
            }
        }

        std::vector<RefOp> g_refsCurrent, g_refsKicked; // step 5b: the frame's reference ops

        // step 5b: REFs as ops, written by the game into a slot it reserved (`at`, 16 bytes per REF; the cursor is past it):
        // the slot gets the item's tag (or, joining the open item, nothing) and empty CNTs for the rest
        bool appendRefs(uint8_t *rdram, uint32_t at, const RefOp *ops, uint32_t n)
        {
            const uint32_t cursor = rd32(rdram, kPacketCursor);
            const uint32_t half = 0x520F90u + rd32(rdram, 0x520F90u + 0x14u) * 0xE6670u + 0x20u;
            const uint32_t bytes = n * 16u;
            if ((at & 0xFu) != 0u || at < half || at + bytes > cursor || cursor > half + 0xE6670u)
                return false;
            const bool merge = g_openItem >= 0 && at == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size();
            if (!merge && g_itemsCurrent.size() >= kMaxItems)
                return false;
            const uint32_t index = merge ? static_cast<uint32_t>(g_openItem) : static_cast<uint32_t>(g_itemsCurrent.size());
            Item &it = merge ? g_itemsCurrent[index] : g_itemsCurrent.emplace_back();
            if (!merge)
                it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
            if (!it.refCount) // its first references (also when it is an open item of inline ops)
                it.refAt = static_cast<uint32_t>(g_refsCurrent.size());
            for (uint32_t i = 0; i < n; ++i)
            {
                RefOp r = ops[i];
                r.pos = it.prefixBytes;
                r.owner = index;
                g_refsCurrent.push_back(r);
            }
            it.refCount += n;
            const uint32_t empty[4] = {0x10000000u, 0u, 0u, 0u}; // CNT 0, NOP NOP
            for (uint32_t i = 0; i < n; ++i)
                if (i == 0 && !merge)
                    writeItemTag(rdram, at, index, false);
                else
                    std::memcpy(rdram + ((at + i * 16u) & kRamMask), empty, 16);
            g_openItem = static_cast<int>(index);
            g_openEnd = at + bytes;
            ++(merge ? g_opStats.merged : g_opStats.ops);
            return true;
        }

        // VIF bytes as a state op: into the open item, or a new item and its tag; false = the caller writes its packets
        bool appendOp(uint8_t *rdram, const uint8_t *vif, uint32_t bytes)
        {
            uint32_t cursor = 0;
            if (!cursorForItem(rdram, 16u, cursor))
                return false;
            if (g_openItem >= 0 && cursor == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size())
            {
                g_arenaCurrent.insert(g_arenaCurrent.end(), vif, vif + bytes);
                g_itemsCurrent[g_openItem].prefixBytes += bytes;
                ++g_opStats.merged;
                return true;
            }
            if (g_itemsCurrent.size() >= kMaxItems)
                return false;
            const uint32_t index = static_cast<uint32_t>(g_itemsCurrent.size());
            Item &it = g_itemsCurrent.emplace_back();
            it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
            g_arenaCurrent.insert(g_arenaCurrent.end(), vif, vif + bytes);
            it.prefixBytes = bytes;
            writeItemTag(rdram, cursor, index);
            g_openItem = static_cast<int>(index);
            g_openEnd = cursor + 16u;
            ++g_opStats.ops;
            return true;
        }

        // ---- steps 5c / 5d: captured emitters ---------------------------------------------------------------------------
        // An emitter (Im2D's FUN_002FB690, the effect / immediate-geometry writers: kCaptured) writes its packets as it always
        // does; its entry notes the cursor and the guest stack pointer. At the next of our hook points on the same stack
        // level or above (or the kick) the region it wrote - CNTs and REFs only, nothing else written after it - becomes an
        // op: the CNTs' VIF bytes inline, the REFs as references (their payloads copied at the kick), into the open item or
        // a new one, the cursor back at the region's start. Hooks deeper on the stack run inside the emitter: they neither
        // settle it nor make ops (their packets are part of its region).
        bool g_im2dPending = false;  // (a capture pending - Im2D's name kept from step 5c)
        uint32_t g_im2dStart = 0, g_captureSp = 0;
        int g_captureEmitter = 0;
        constexpr int kMaxEmitters = 24;
        // the emitters whose packets are captured (game-internals.md 6i): Im2D, then the effect and immediate-geometry
        // writers left in the chain after 5a-5c (LCS_RSL_CENSUS / LCS_TRACE_RSL)
        constexpr struct
        {
            uint32_t addr;
            const char *name;
        } kCaptured[] = {
            {0x002FB690u, "Im2D 2FB690"},  {0x0040C430u, "40C430"},     {0x0040F6C8u, "40F6C8"}, {0x00266868u, "266868"},
            {0x002F3DD0u, "2F3DD0"},       {0x002A92C0u, "2A92C0"},     {0x002F5520u, "2F5520"}, {0x002ABF68u, "2ABF68"},
            {0x0032D480u, "32D480"},       {0x0032D7C8u, "32D7C8"},     {0x002F8250u, "2F8250"}, {0x002F0360u, "2F0360"},
            {0x002F39D8u, "2F39D8"},       {0x0032CD60u, "32CD60"},     {0x0032D880u, "32D880"}, {0x002F9FA0u, "2F9FA0"},
            {0x002F53D8u, "camera 2F53D8"},
            // the texture binds (they reserve upload slots, fill them through FUN_003298D8, then write TEXA / MIPTBP1 /
            // TEX2 / TEXFLUSH as a CNT DIRECTHL): captured whole - their slots are never seen half filled (step 5h)
            {0x002F62B8u, "bind 2F62B8"},  {0x002F6A78u, "bind 2F6A78"},
        };
        constexpr int kCapturedCount = int(sizeof(kCaptured) / sizeof(kCaptured[0]));
        constexpr int kDrawEmitter = kCapturedCount; // step 5f: a draw left to its original (stats slot after the emitters)
        constexpr int kGapEmitter = kCapturedCount + 1; // step 5h: what was written between two hook points
        static_assert(kCapturedCount + 2 <= kMaxEmitters);
        struct Im2dStats
        {
            uint64_t ops[kMaxEmitters] = {}, kept[kMaxEmitters] = {}, partial[kMaxEmitters] = {}, empty = 0, verifySame = 0, verifyDiffer = 0;
            uint64_t notCntRef = 0, irq = 0, notRam = 0;
        } g_im2dStats;
        struct Im2dCheck // LCS_RSL_ITEMS_VERIFY: the regions as captured, compared with guest memory at the kick
        {
            uint32_t at;
            std::vector<uint8_t> bytes;
        };
        std::vector<Im2dCheck> g_im2dChecks;
        std::vector<std::pair<uint32_t, uint32_t>> g_regionsWalk; // step 5h verify: the kicked frame's regions [start, end)
        uint64_t g_regionReferenced = 0;

        // step 5f: a captured region in pieces - inline VIF bytes and references, then optionally a block (a CALLed packet, or
        // the draw's own REF / CALL when the region is a draw left to its original): one item each, the first with the
        // region's tag (or joining the open item), the others continuing it
        struct Piece
        {
            std::vector<uint8_t> inl;
            std::vector<std::pair<uint32_t, RefOp>> refs;
            bool hasBlock = false;
            Draw d{};
        };

        // the pieces into items. resume 0: the whole region (the cursor is at `start`); else only [start, resume) - the
        // packets from `resume` on stay, the cursor stays, and the region's start gets the tag and a NEXT to `resume`
        bool appendRegion(uint8_t *rdram, uint32_t start, const Piece *pieces, size_t count, uint32_t resume = 0u)
        {
            uint32_t cursor = 0;
            const bool join = g_openItem >= 0 && start == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size();
            if (resume)
            {
                if (resume - start < (join ? 16u : 32u) && !(resume - start == 16u && !join))
                    return false; // no room for the tags it needs
            }
            else if (!cursorForItem(rdram, 16u, cursor) || cursor != start)
                return false;
            if (g_itemsCurrent.size() + count + 1u >= kMaxItems)
                return false;
            uint32_t first = 0, index = 0;
            for (size_t k = 0; k < count; ++k)
            {
                const Piece &pc = pieces[k];
                const bool joinThis = k == 0 && join;
                index = joinThis ? static_cast<uint32_t>(g_openItem) : static_cast<uint32_t>(g_itemsCurrent.size());
                if (k == 0)
                    first = index;
                else
                    g_itemsCurrent[index - 1u].continued = true; // (the item before is the one just made)
                Item &it = joinThis ? g_itemsCurrent[index] : g_itemsCurrent.emplace_back();
                if (!joinThis)
                    it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
                const uint32_t base = it.prefixBytes;
                if (!pc.refs.empty() && !it.refCount)
                    it.refAt = static_cast<uint32_t>(g_refsCurrent.size());
                for (const auto &[pos, r] : pc.refs)
                {
                    RefOp op = r;
                    op.pos = base + pos;
                    op.owner = index;
                    g_refsCurrent.push_back(op);
                }
                it.refCount += static_cast<uint32_t>(pc.refs.size());
                g_arenaCurrent.insert(g_arenaCurrent.end(), pc.inl.begin(), pc.inl.end());
                it.prefixBytes += static_cast<uint32_t>(pc.inl.size());
                if (pc.hasBlock)
                {
                    it.d = pc.d;
                    it.hasBlock = true;
                }
            }
            const bool lastOpen = !pieces[count - 1u].hasBlock;
            if (resume)
            {
                uint32_t at = start;
                if (!join)
                {
                    writeItemTag(rdram, at, first, false);
                    at += 16u;
                }
                if (at < resume)
                {
                    const uint64_t next[2] = {(uint64_t(resume) << 32) | 0x20000000u, 0u}; // NEXT resume, NOP NOP
                    std::memcpy(rdram + (at & kRamMask), next, 16);
                }
                g_openItem = -1; // the packets after it come before anything later
                ++(join ? g_opStats.merged : g_opStats.ops);
                return true;
            }
            if (join)
                ++g_opStats.merged; // (the cursor stays at the region's start, the open item's end)
            else
            {
                writeItemTag(rdram, start, first);
                g_openEnd = start + 16u;
                ++g_opStats.ops;
            }
            g_openItem = lastOpen ? static_cast<int>(index) : -1; // a later op may follow the last piece when it has no block
            return true;
        }

        // step 5f: a draw left to its original (its flush calls guest code), captured like an emitter
        bool g_captureDraw = false;
        // the first of this frame's draw-list entries (g_current) made during the capture: the captured draw's own, or the
        // draws an emitter makes inside it. Their tags are in the region in the same order; a tag taken into an item is
        // never seen by the walker, so its entry goes (else the walker's matching, g_kicked, falls out of step)
        size_t g_captureCurrentAt = 0;
        uint32_t g_captureDrawFn[2] = {};  // the draw function's code (game/lcs/functions.csv)

        // a hook deeper on the stack than a draw being captured runs inside it only when called from the draw, its flush
        // (FUN_002F8710), the plug-in callbacks' wrappers or FUN_001CFAE0: other hooks deeper on the stack run after the
        // draw returned (a draw's caller calling another function)
        bool insideDraw(uint32_t ra)
        {
            static constexpr uint32_t kInside[][2] = {
                {0x002F8710u, 0x002F937Cu}, {0x002A9538u, 0x002A9560u}, {0x002F4080u, 0x002F40ACu},
                {0x0040F798u, 0x0040F7C0u}, {0x001CEF90u, 0x001CEFACu}, {0x001CFAE0u, 0x001CFCDCu},
            };
            if (ra >= g_captureDrawFn[0] && ra < g_captureDrawFn[1])
                return true;
            for (const auto &r : kInside)
                if (ra >= r[0] && ra < r[1])
                    return true;
            return false;
        }

        // LCS_RSL_CENSUS=2: the cursor at every hook point (the hooked function, its caller) - the census names each tag
        // left in the chain after the last hook point before it
        struct Origin
        {
            uint32_t cursor, fn, ra;
        };
        std::vector<Origin> g_originNow, g_originWalk;
        bool censusOrigins()
        {
            static const bool on = std::getenv("LCS_RSL_CENSUS") && *std::getenv("LCS_RSL_CENSUS") == '2';
            return on;
        }

        // step 5h: a texture bind being captured - only its uploads run inside it (called from the bind itself); a hook
        // deeper on the stack called from elsewhere runs after the bind returned (its caller's next call)
        bool g_captureBind = false;
        uint32_t g_captureBindFn[2] = {};
        bool insideBind(uint32_t ra)
        {
            return (ra >= g_captureBindFn[0] && ra < g_captureBindFn[1]) || (ra >= 0x003298D8u && ra < 0x00329BFCu);
        }

        // sp / ra: the guest stack pointer and return address of the hook asking (sp ~0u: the kick - every emitter has
        // returned); fn: the hooked function (census origins)
        void settleRegion(uint8_t *rdram);
        void captureGap(uint8_t *rdram);
        uint32_t g_gapFn = 0, g_gapRa = 0; // (LCS_RSL_GAP_LOG)
        size_t g_currentAtSettle = 0; // g_current's size after the last settle (draws made since: their tags in the gap)

        void settleIm2D(uint8_t *rdram, uint32_t sp, uint32_t ra, uint32_t fn)
        {
            if (censusOrigins() && sp != ~0u)
                g_originNow.push_back({rd32(rdram, kPacketCursor), fn, ra});
            g_inCapture = g_im2dPending && sp < g_captureSp && (!g_captureDraw || insideDraw(ra)) && (!g_captureBind || insideBind(ra));
            if (g_inCapture)
                return;
            g_gapFn = fn;
            g_gapRa = ra;
            if (g_im2dPending)
                settleRegion(rdram);
            else
                captureGap(rdram);
            if (g_materialCacheEnds)
            {
                g_materialCaching = false;
                g_materialCacheEnds = false;
            }
            g_lastCursor = rd32(rdram, kPacketCursor);
            g_currentAtSettle = g_current.size();
        }

        // LCS_RSL_GAPS=0: no gap captures
        bool g_gaps = false;

        // step 5h: what any code wrote since the last hook point - a region like an emitter's (not while a cache is being
        // filled: those bytes are copied and sent again; not from a cursor outside this frame's half)
        uint64_t g_gapWhy[6] = {}; // (LCS_RSL_WORLD_LOG) not taken: cache, material cache, no ops, cursor, nothing new, taken
        void captureGap(uint8_t *rdram)
        {
            if (!g_gaps)
                return;
            if (g_caching || g_materialCaching || !(opsAllowed() || g_itemVerify))
            {
                ++g_gapWhy[g_caching ? 0 : g_materialCaching ? 1 : 2];
                return;
            }
            static const char *fns = std::getenv("LCS_RSL_GAP_FNS"); // (debug: gaps only at these hook points; 0 = the kick)
            if (fns)
            {
                char want[16];
                std::snprintf(want, sizeof(want), ",%x,", g_gapFn);
                if (!std::strstr(fns, want))
                    return;
            }
            uint32_t cursor = 0;
            if (!cursorForItem(rdram, 16u, cursor))
            {
                ++g_gapWhy[3];
                return;
            }
            const uint32_t half = 0x520F90u + rd32(rdram, 0x520F90u + 0x14u) * 0xE6670u + 0x20u;
            const uint32_t from = g_lastCursor >= half && g_lastCursor <= cursor ? g_lastCursor : half; // (a new frame: its start)
            if (from >= cursor || (from & 0xFu) != 0u)
            {
                ++g_gapWhy[4];
                return;
            }
            ++g_gapWhy[5];
            g_im2dPending = true;
            g_im2dStart = from;
            g_captureEmitter = kGapEmitter;
            g_captureDraw = false;
            g_captureBind = false;
            g_captureCurrentAt = std::min(g_currentAtSettle, g_current.size());
            static const bool gapLog = std::getenv("LCS_RSL_GAP_LOG") != nullptr;
            static int logged = 0;
            if (gapLog && g_itemFrame == 400u && logged < 2000)
            {
                ++logged;
                uint64_t lo;
                std::memcpy(&lo, rdram + (from & kRamMask), 8);
                std::fprintf(stderr, "[rsl-gap] frame %u: 0x%08x-0x%08x (%u bytes, half 0x%08x, last 0x%08x) first tag %016llx code %08x %08x, settle fn %06x ra %06x\n",
                             g_itemFrame, from, cursor, cursor - from, half, g_lastCursor, (unsigned long long)lo, rd32(rdram, from + 8u),
                             rd32(rdram, from + 12u), g_gapFn, g_gapRa);
            }
            settleRegion(rdram);
        }

        // the pending capture's region into items (settleIm2D). Step 5h: in runs - a tag the walker must still read (a CALL
        // of a packet not known yet, a REF outside main RAM, an item's own tag) stays where it is and the parse goes on
        // after it; each run of convertible tags before / between / after them becomes items in place (its start gets the
        // item's tag and a NEXT to the next kept tag). Only a region taken whole in one run moves the cursor back (as
        // before), and never a gap's.
        void settleRegion(uint8_t *rdram)
        {
            g_im2dPending = false;
            g_captureDraw = false;
            const int e = g_captureEmitter;
            const uint32_t start = g_im2dStart, cursor = rd32(rdram, kPacketCursor);
            if (cursor == start)
            {
                ++g_im2dStats.empty; // nothing written
                return;
            }
            thread_local std::vector<Piece> pieces; // (kept with their buffers: hundreds of settles a frame)
            size_t count = 0;
            const auto newPiece = [&]() -> Piece & {
                if (count == pieces.size())
                    pieces.emplace_back();
                Piece &pc = pieces[count++];
                pc.inl.clear();
                pc.refs.clear();
                pc.hasBlock = false;
                return pc;
            };
            thread_local std::vector<size_t> runDraws, erase; // draw-list entries whose tags this run / the region took
            erase.clear();
            size_t draw = g_captureCurrentAt; // the next draw-list entry whose tag the region should hold
            static const bool calls = !(std::getenv("LCS_RSL_CAPTURE_CALLS") && *std::getenv("LCS_RSL_CAPTURE_CALLS") == '0');
            static const bool runs = !(std::getenv("LCS_RSL_RUNS") && *std::getenv("LCS_RSL_RUNS") == '0');
            bool whole = false, tookAny = false;
            uint32_t run = start;
            for (;;)
            {
                count = 0;
                newPiece();
                runDraws.clear();
                uint32_t at = run;
                const char *why = nullptr;
                uint32_t stopId = 0, stopQwc = 0;
                bool stopOwn = false;
                while (!why && at < cursor) // (`at` stays at the first tag that is not taken)
                {
                    if (at + 16u > cursor)
                    {
                        why = "shape";
                        break;
                    }
                    Piece &cur = pieces[count - 1u];
                    uint64_t lo;
                    std::memcpy(&lo, rdram + (at & kRamMask), 8);
                    const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu), id = static_cast<uint32_t>((lo >> 28) & 7u);
                    const uint32_t addr = static_cast<uint32_t>(lo >> 32) & 0x7FFFFFFFu;
                    const bool ram = (addr & 0xF0000000u) == 0u && (addr & kRamMask) + qwc * 16u <= kRamMask + 1u;
                    const Draw *want = draw < g_current.size() ? &g_current[draw] : nullptr;
                    stopId = id;
                    stopQwc = qwc;
                    if ((lo >> 31) & 1u)
                        why = "irq";
                    else if (id == 1u && qwc == 0u && (rd32(rdram, at + 8u) >> 24) == 0x07u &&
                             (rd32(rdram, at + 12u) & 0xFFFF0000u) == kItemNop)
                        why = "item"; // an item's tag (never taken into another item's bytes)
                    else if (id == 1u) // CNT: its VIFcodes and payload, as the walker copies them
                    {
                        if (at + 16u + qwc * 16u > cursor)
                            why = "shape";
                        else
                        {
                            cur.inl.insert(cur.inl.end(), rdram + (at & kRamMask) + 8, rdram + (at & kRamMask) + 16u + qwc * 16u);
                            at += 16u + qwc * 16u;
                        }
                    }
                    else if ((id == 3u || id == 4u) && want && want->id == 3u && ram && (addr & kRamMask) == want->addr &&
                             qwc == want->qwc && qwc)
                    {
                        // a world draw's REF: its VIFcodes, then the geometry as a world block (recordings, step 4c)
                        cur.inl.insert(cur.inl.end(), rdram + (at & kRamMask) + 8, rdram + (at & kRamMask) + 16);
                        cur.hasBlock = true;
                        cur.d = *want;
                        newPiece();
                        runDraws.push_back(draw++);
                        at += 16u;
                    }
                    else if (id == 3u || id == 4u) // REF / REFS: a reference, its payload read at the kick
                    {
                        if (!ram)
                            why = "ram";
                        else
                        {
                            RefOp r{};
                            std::memcpy(r.code, rdram + (at & kRamMask) + 8, 8);
                            r.addr = addr & kRamMask;
                            r.qwc = qwc;
                            cur.refs.push_back({static_cast<uint32_t>(cur.inl.size()), r}); // (the payload goes in at resolve)
                            at += 16u;
                        }
                    }
                    else if (id == 5u) // CALL: its VIFcodes and payload, then the CALLed packet as a block (step 5f)
                    {
                        const bool own = want && want->id == 5u && (addr & kRamMask) == want->addr; // a model draw's CALL
                        const Draw d = own ? *want : Draw{5u, addr & kRamMask, 0u, OtherCall};
                        stopOwn = own;
                        if (!calls && !own)
                            why = "call"; // LCS_RSL_CAPTURE_CALLS=0: other CALLs end the region (as before step 5f's end)
                        else if ((addr & 0xF0000000u) != 0u || !g_flatOk.count(blockKey(d.kind, d.addr, 0u)))
                            why = "call"; // not known to be copied as flattenBlock copies it
                        else if (at + 16u + qwc * 16u > cursor)
                            why = "shape";
                        else
                        {
                            cur.inl.insert(cur.inl.end(), rdram + (at & kRamMask) + 8, rdram + (at & kRamMask) + 16u + qwc * 16u);
                            cur.hasBlock = true;
                            cur.d = d;
                            newPiece();
                            if (own)
                                runDraws.push_back(draw++);
                            at += 16u + qwc * 16u;
                        }
                    }
                    else
                        why = "tag";
                }
                if (count > 1u && pieces[count - 1u].inl.empty() && pieces[count - 1u].refs.empty())
                    --count; // (nothing after the last block)
                if (run == start && !why && at == cursor)
                {
                    whole = true; // one run, the whole region: below
                    erase.insert(erase.end(), runDraws.begin(), runDraws.end());
                    break;
                }
                if (why)
                {
                    ++(why[0] == 'i' && why[1] == 'r' ? g_im2dStats.irq : why[0] == 'r' ? g_im2dStats.notRam : g_im2dStats.notCntRef);
                    static const bool keptLog = std::getenv("LCS_RSL_KEPT_LOG") != nullptr;
                    static int logged = 0;
                    if (keptLog && logged < 4000)
                    {
                        ++logged;
                        uint64_t lo;
                        std::memcpy(&lo, rdram + (at & kRamMask), 8);
                        std::fprintf(stderr, "[rsl-kept] %s: %s%s at +0x%x of 0x%x, tag %016llx\n",
                                     e < kCapturedCount ? kCaptured[e].name : e == kDrawEmitter ? "draw" : "gap",
                                     at > run ? "run before it taken, " : "", why, at - start, cursor - start, (unsigned long long)lo);
                    }
                }
                if (at > run) // the run before the tag that stays (or before the region's end)
                {
                    if (g_itemVerify)
                    {
                        g_im2dChecks.push_back({run, std::vector<uint8_t>(rdram + (run & kRamMask), rdram + (at & kRamMask))});
                        tookAny = true;
                    }
                    else if (appendRegion(rdram, run, pieces.data(), count, at))
                    {
                        erase.insert(erase.end(), runDraws.begin(), runDraws.end());
                        tookAny = true;
                    }
                }
                if (!why || at >= cursor || !runs)
                    break;
                // a tag the walker reads as it is, and goes on after: the parse goes on there too
                uint32_t next = 0;
                if (why[0] == 'c' && stopId == 5u)
                {
                    next = at + 16u + stopQwc * 16u; // (a CALL returns after its own payload)
                    if (stopOwn)
                        ++draw; // the draw's tag stays: its entry too
                }
                else if (why[0] == 'r' || (why[0] == 'i' && why[1] == 't'))
                    next = at + 16u;
                else
                    break;
                if (next >= cursor)
                    break;
                run = next;
            }
            const auto taken = [&] { // the draws whose tags went into items: the walker will not see them
                std::sort(erase.begin(), erase.end());
                for (size_t k = erase.size(); k-- > 0;)
                    if (erase[k] < g_current.size())
                        g_current.erase(g_current.begin() + static_cast<ptrdiff_t>(erase[k]));
            };
            if (!whole)
            {
                taken();
                ++(tookAny ? g_im2dStats.partial[e] : g_im2dStats.kept[e]);
                return;
            }
            if (g_itemVerify)
            {
                g_im2dChecks.push_back({start, std::vector<uint8_t>(rdram + (start & kRamMask), rdram + (cursor & kRamMask))});
                return;
            }
            if (e == kGapEmitter)
            {
                // step 5h: a gap is taken in place - the cursor never moves back. A gap is settled at any hook point, and a
                // caller up the stack may hold the cursor across that call (FUN_00240ED8 keeps the start of the range it
                // copies into the sector cache across FUN_002409C8: a rewind there shifted the copied range)
                if (!appendRegion(rdram, start, pieces.data(), count, cursor))
                {
                    ++g_im2dStats.kept[e];
                    return;
                }
                taken();
                ++g_im2dStats.ops[e];
                return;
            }
            wr32(rdram, kPacketCursor, start);
            if (!appendRegion(rdram, start, pieces.data(), count))
            {
                wr32(rdram, kPacketCursor, cursor);
                ++g_im2dStats.kept[e];
                return;
            }
            taken();
            ++g_im2dStats.ops[e];
        }

        void checkIm2D(const uint8_t *rdram)
        {
            g_regionsWalk.clear();
            for (const Im2dCheck &c : g_im2dChecks)
                g_regionsWalk.push_back({c.at & kRamMask, (c.at & kRamMask) + static_cast<uint32_t>(c.bytes.size())});
            std::sort(g_regionsWalk.begin(), g_regionsWalk.end());
            for (const Im2dCheck &c : g_im2dChecks)
                ++(std::memcmp(rdram + (c.at & kRamMask), c.bytes.data(), c.bytes.size()) == 0 ? g_im2dStats.verifySame
                                                                                                : g_im2dStats.verifyDiffer);
            g_im2dChecks.clear();
        }

        // the draw call as an item (game thread, the draw function's whole work); false: the original runs
        bool itemDraw(uint8_t *rdram, R5900Context *ctx, Kind kind, uint32_t raw0, uint32_t raw1)
        {
            ItemStats &st = g_itemStats;
            if (g_inCapture)
            {
                ++st.caching; // inside an emitter being captured: its region takes the draw's packets
                return false;
            }
            // FUN_002F80A0 CALLs the shared state packet (*0x48830C) after the GS packet when bit 31 is set: then the flush,
            // the GS packet and that CALL stay in the chain as the original writes them, and the item is the block alone
            const bool stateCall = kind == ModelB && static_cast<int32_t>(rd32(rdram, kDirty)) < 0;
            const uint32_t addr = raw0 & kRamMask;
            if (kind == World ? ((raw0 & 0xF0000000u) != 0u || (raw1 & 0xFFFFu) == 0u || (raw1 & 0x0FFF0000u) != 0u)
                              : (raw0 & 0xF0000000u) != 0u)
            {
                ++st.shape;
                return false;
            }
            if (g_caching)
            {
                ++st.caching; // the packets are copied into the sector's cache: the packet path
                return false;
            }
            if (kind != World && !g_flatOk.count(blockKey(kind, addr, 0u)))
            {
                ++st.unknown; // not yet seen copied by the walker as flattenBlock copies it
                return false;
            }
            if (g_itemsCurrent.size() >= kMaxItems)
            {
                ++st.full;
                return false;
            }
            thread_local lcs::rslnative::Flush f;
            if (!lcs::rslnative::buildFlush(rdram, f))
            {
                ++st.flush; // callbacks / FUN_001CFAE0: the original
                return false;
            }
            // what the original would write: the flush's tags (kept in the packet buffer when they CALL / REF), the GS packet,
            // the draw's tag - here an item's CNT instead of the draw's tag
            thread_local std::vector<uint8_t> tags; // what stays in the packet buffer
            tags.clear();
            if (!f.cntOnly || stateCall)
                tags.assign(f.bytes, f.bytes + f.size);
            if (stateCall)
            {
                gsPacket(rdram, f, tags);
                const uint64_t call[2] = {(uint64_t(rd32(rdram, 0x48830Cu)) << 32) | 0x50000000u, 0u};
                const auto *c = reinterpret_cast<const uint8_t *>(call);
                tags.insert(tags.end(), c, c + 16);
            }
            const uint32_t written = static_cast<uint32_t>(tags.size()) + 16u;
            uint32_t cursor = 0;
            if (!cursorForItem(rdram, written, cursor))
            {
                ++st.noRoom; // no room, or the cursor is somewhere else (a packet built for later): the packet path
                return false;
            }
            // step 5a: the open item's ops come right before this draw - it takes the draw (no tag of its own)
            const bool merge = tags.empty() && g_openItem >= 0 && cursor == g_openEnd && size_t(g_openItem) + 1u == g_itemsCurrent.size();
            const uint32_t index = merge ? static_cast<uint32_t>(g_openItem) : static_cast<uint32_t>(g_itemsCurrent.size());
            Item &it = merge ? g_itemsCurrent[index] : g_itemsCurrent.emplace_back();
            it.d = {kind == World ? 3u : 5u, addr, kind == World ? (raw1 & 0xFFFFu) : 0u, kind};
            it.hasBlock = true;
            if (!merge)
                it.prefixAt = static_cast<uint32_t>(g_arenaCurrent.size());
            const uint32_t prefixBefore = merge ? it.prefixBytes : 0u;
            g_openItem = -1; // a draw ends the item
            if (f.cntOnly && !stateCall)
                tagsToVif(f.bytes, f.size, g_arenaCurrent);
            if (!stateCall)
                gsVif(rdram, f, g_arenaCurrent);
            it.prefixBytes = static_cast<uint32_t>(g_arenaCurrent.size()) - it.prefixAt;
            (void)prefixBefore;
            if (!tags.empty())
            {
                std::memcpy(rdram + (cursor & kRamMask), tags.data(), tags.size());
                cursor += static_cast<uint32_t>(tags.size());
            }
            if (stateCall)
                ++st.stateCall; // (made as an item all the same)
            if (merge)
                ++g_opStats.mergedDraws; // its tag is the open item's, already in the buffer
            else
                writeItemTag(rdram, cursor, index);
            if (!tags.empty())
                g_lastCursor = cursor - static_cast<uint32_t>(tags.size()); // step 5h: the next gap takes them (in place)
            wr32(rdram, kGsQueueCount, 0u); // the draw sent the queue
            wr32(rdram, kDirty, stateCall ? f.dirtyAfter & 0x7FFFFFFFu : f.dirtyAfter);
            lcs::rslnative::applyClears(rdram, f); // step 5f: the plug-in callbacks' writes
            ++st.made[kind];
            ctx->pc = getRegU32(ctx, 31);
            return true;
        }

        // LCS_RSL_ITEMS_VERIFY=1: the item is built but the original runs; what it wrote is compared with what the item
        // stands for (at the next draw call / the kick: code after the original returns is not relied on)
        struct ItemCheck
        {
            bool armed = false;
            uint32_t cursor = 0;
            std::vector<uint8_t> expect; // the flush's tags, the GS packet, the draw's tag
        } g_itemCheck;

        void checkItem(const uint8_t *rdram)
        {
            if (!g_itemCheck.armed)
                return;
            g_itemCheck.armed = false;
            const bool same = std::memcmp(rdram + (g_itemCheck.cursor & kRamMask), g_itemCheck.expect.data(), g_itemCheck.expect.size()) == 0;
            ++(same ? g_itemStats.verifySame : g_itemStats.verifyDiffer);
            static int logged = 0;
            if (!same && logged++ < 10)
                for (size_t i = 0; i < g_itemCheck.expect.size(); i += 8u)
                {
                    uint64_t want, got;
                    std::memcpy(&want, g_itemCheck.expect.data() + i, 8);
                    std::memcpy(&got, rdram + ((g_itemCheck.cursor + i) & kRamMask), 8);
                    if (want != got)
                    {
                        std::fprintf(stderr, "[rsl-items] verify: draw at 0x%08x differs at +0x%zx of %zu: item %016llx original %016llx\n",
                                     g_itemCheck.cursor, i, g_itemCheck.expect.size(), (unsigned long long)want, (unsigned long long)got);
                        break;
                    }
                }
        }

        void verifyDraw(const uint8_t *rdram, Kind kind, uint32_t raw0, uint32_t raw1)
        {
            checkItem(rdram);
            const bool stateCall = kind == ModelB && static_cast<int32_t>(rd32(rdram, kDirty)) < 0;
            thread_local lcs::rslnative::Flush f;
            if (!lcs::rslnative::buildFlush(rdram, f))
                return;
            auto &e = g_itemCheck.expect;
            e.assign(f.bytes, f.bytes + f.size);
            const size_t gsAt = e.size();
            gsPacket(rdram, f, e);
            { // the item's VIF bytes for the GS packet (gsVif) are the walker's for these tags
                thread_local std::vector<uint8_t> walker, item;
                walker.clear();
                item.clear();
                tagsToVif(e.data() + gsAt, static_cast<uint32_t>(e.size() - gsAt), walker);
                gsVif(rdram, f, item);
                if (walker != item)
                    ++g_itemStats.verifyDiffer;
            }
            if (stateCall)
            {
                const uint64_t call[2] = {(uint64_t(rd32(rdram, 0x48830Cu)) << 32) | 0x50000000u, 0u};
                const auto *c = reinterpret_cast<const uint8_t *>(call);
                e.insert(e.end(), c, c + 16);
            }
            const uint64_t drawTag[2] = {kind == World ? (uint64_t(raw0) << 32) | 0x30000000u | (raw1 & 0x0FFFFFFFu)
                                                       : (uint64_t(raw0) << 32) | 0x50000000u,
                                         0u};
            const auto *t = reinterpret_cast<const uint8_t *>(drawTag);
            e.insert(e.end(), t, t + 16);
            g_itemCheck.cursor = rd32(rdram, kPacketCursor);
            g_itemCheck.armed = true;
        }

        // at the kick, after the write watch's fold: each item's block skipped (unchanged since a confirmed copy) or copied
        // from guest RAM, as the walker would for the draw's tag
        void resolveItems(ChainTable &table)
        {
            thread_local std::vector<uint32_t> pages;
            for (Item &it : table.items) // step 5b: prefixes with references - their payloads as guest RAM holds them now
            {
                if (!it.refCount)
                    continue;
                const uint32_t self = static_cast<uint32_t>(&it - table.items.data());
                bool mine = it.refAt + it.refCount <= table.refs.size();
                for (uint32_t r = it.refAt; mine && r < it.refAt + it.refCount; ++r)
                    mine = table.refs[r].owner == self && table.refs[r].pos <= it.prefixBytes;
                if (!mine) // (never expected: counted, and the item runs its inline bytes alone)
                {
                    ++g_itemStats.badRefs;
                    it.refCount = 0;
                    continue;
                }
                // built in place at the arena's end (one copy of each payload, as the walker's)
                it.prefixRastersAt = static_cast<uint32_t>(table.itemRasters.size());
                size_t bytes = it.prefixBytes;
                for (uint32_t r = it.refAt; r < it.refAt + it.refCount; ++r)
                    bytes += 8u + size_t(table.refs[r].qwc) * 16u;
                const size_t start = table.arena.size();
                table.arena.resize(start + bytes);
                uint8_t *out = table.arena.data() + start;
                const uint8_t *inl = table.arena.data() + it.prefixAt;
                uint32_t done = 0;
                size_t at = 0;
                for (uint32_t r = it.refAt; r < it.refAt + it.refCount; ++r)
                {
                    const RefOp &op = table.refs[r];
                    std::memcpy(out + at, inl + done, op.pos - done);
                    at += op.pos - done;
                    done = op.pos;
                    std::memcpy(out + at, op.code, 8);
                    at += 8u;
                    // a raster upload (as observe names them): DIRECT / DIRECTHL of the REF's own size
                    if (g_rasters && op.qwc && ((op.code[1] >> 24) & 0x7Eu) == 0x50u && (op.code[1] & 0xFFFFu) == op.qwc)
                        table.itemRasters.push_back({static_cast<uint32_t>(at), op.addr & kRamMask, op.qwc * 16u});
                    std::memcpy(out + at, g_rdram + (op.addr & kRamMask), op.qwc * 16u);
                    at += op.qwc * 16u;
                }
                std::memcpy(out + at, inl + done, it.prefixBytes - done);
                it.prefixRasterCount = static_cast<uint32_t>(table.itemRasters.size()) - it.prefixRastersAt;
                it.prefixAt = static_cast<uint32_t>(start);
                it.prefixBytes = static_cast<uint32_t>(bytes);
            }
            for (Item &it : table.items)
            {
                if (!it.hasBlock)
                    continue; // step 5a: state ops alone
                Draw &d = it.d;
                const uint64_t snapshotKey = d.kind == World ? blockKey(World, d.addr, d.qwc) : blockKey(d.kind, d.addr, 0u);
                uint32_t units = 0;
                if (skippable(snapshotKey, d.kind, d.addr, units))
                {
                    d.units = units;
                    if (!g_skipVerify)
                    {
                        it.skip = true;
                        ++g_itemStats.skipped;
                        ++g_skipped;
                        continue;
                    }
                    d.wouldSkip = true; // copied all the same, checked by the worker
                }
                pages.clear();
                bool usable = true;
                it.blockAt = static_cast<uint32_t>(table.arena.size());
                it.rastersAt = static_cast<uint32_t>(table.itemRasters.size());
                if (d.kind == World)
                {
                    addPages(pages, usable, d.addr, d.qwc * 16u);
                    table.arena.insert(table.arena.end(), g_rdram + d.addr, g_rdram + d.addr + d.qwc * 16u);
                }
                else if (!flattenBlock(g_rdram, d.addr, table.arena, pages, usable, &table.itemRasters))
                {
                    table.arena.resize(it.blockAt);
                    table.itemRasters.resize(it.rastersAt);
                    it.lost = true; // not as the walker would copy it: drawn by the packet path from the next frame on
                    g_flatOk.erase(snapshotKey);
                    ++g_itemStats.lost;
                    continue;
                }
                it.blockBytes = static_cast<uint32_t>(table.arena.size()) - it.blockAt;
                it.rasterCount = static_cast<uint32_t>(table.itemRasters.size()) - it.rastersAt;
                closeBlock(snapshotKey, pages, usable, it.blockBytes / 8u);
                ++g_itemStats.copied;
            }
        }

        // a model block the walker copied (the packet path): may later draws of it be items?
        void noteFlattenable(uint64_t key, uint32_t units)
        {
            const Kind kind = static_cast<Kind>(key >> 56);
            if (!g_items || (kind != Model && kind != ModelB && kind != OtherCall) || g_flatOk.count(key))
                return; // (step 5f: other calls too - captured regions take them as blocks)
            thread_local std::vector<uint8_t> flat;
            thread_local std::vector<uint32_t> pages;
            flat.clear();
            pages.clear();
            bool usable = true;
            const uint32_t addr = static_cast<uint32_t>(key >> 24) & kRamMask;
            if (flattenBlock(g_rdram, addr, flat, pages, usable, nullptr) && flat.size() == size_t(units) * 8u && usable)
                g_flatOk[key] = true;
        }

        // LCS_RSL_WORLD_LOG: the game thread's time per chain from its first tag to its hand-over (walk, copy, the items'
        // resolve), and the resolve's part (FULLY_NATIVE.md 5g: what a kick without a walk could save)
        struct KickTime
        {
            uint64_t chains = 0;
            double ms = 0.0, resolveMs = 0.0, maxMs = 0.0;
        } g_kickTime;
        std::chrono::steady_clock::time_point g_chainStartAt{};
        double g_chainResolveMs = 0.0;

        void reportItems()
        {
            if (!g_items)
                return;
            ItemStats &s = g_itemStats;
            std::fprintf(stderr, "[rsl-items] made: world %llu, model %llu, model-b %llu (with the state packet %llu); packet path: flush %llu, "
                                 "not known %llu, no room %llu, shape %llu, full %llu, caching %llu; at the kick: %llu skipped, %llu copied, %llu lost; "
                                 "worker: %llu seen, %llu not placed, %llu another frame's; references not their item's: %llu\n",
                         (unsigned long long)s.made[World], (unsigned long long)s.made[Model], (unsigned long long)s.made[ModelB],
                         (unsigned long long)s.stateCall, (unsigned long long)s.flush, (unsigned long long)s.unknown,
                         (unsigned long long)s.noRoom, (unsigned long long)s.shape, (unsigned long long)s.full, (unsigned long long)s.caching,
                         (unsigned long long)s.skipped,
                         (unsigned long long)s.copied, (unsigned long long)s.lost, (unsigned long long)g_itemsSeenW.exchange(0),
                         (unsigned long long)g_itemsUnplacedW.exchange(0), (unsigned long long)g_itemsStaleW.exchange(0), (unsigned long long)s.badRefs);
            std::fprintf(stderr, "[rsl-items] state ops: %llu items of their own, %llu merged into one, %llu draws into an op item, %llu uploads; "
                                 "packet path: %llu (no room / not RAM / no header yet), %llu (a cache being filled)\n",
                         (unsigned long long)g_opStats.ops, (unsigned long long)g_opStats.merged, (unsigned long long)g_opStats.mergedDraws,
                         (unsigned long long)g_opStats.uploads,
                         (unsigned long long)g_opStats.packets, (unsigned long long)g_opStats.caching);
            g_opStats = OpStats{};
            std::fprintf(stderr, "[rsl-items] captured regions as ops / their front as ops / kept:");
            for (int e = 0; e <= kGapEmitter; ++e)
                if (g_im2dStats.ops[e] || g_im2dStats.kept[e] || g_im2dStats.partial[e])
                    std::fprintf(stderr, " %s %llu/%llu/%llu", e < kCapturedCount ? kCaptured[e].name : e == kDrawEmitter ? "draws" : "gaps", (unsigned long long)g_im2dStats.ops[e],
                                 (unsigned long long)g_im2dStats.partial[e], (unsigned long long)g_im2dStats.kept[e]);
            std::fprintf(stderr, "; empty %llu; kept for: other tags %llu, IRQ %llu, not RAM %llu%s", (unsigned long long)g_im2dStats.empty,
                         (unsigned long long)g_im2dStats.notCntRef, (unsigned long long)g_im2dStats.irq, (unsigned long long)g_im2dStats.notRam,
                         g_itemVerify ? "" : "\n");
            if (g_itemVerify)
                std::fprintf(stderr, "; verify: %llu captured regions unchanged at the kick, %llu changed after capture, %llu tags reading "
                                     "inside one\n",
                             (unsigned long long)g_im2dStats.verifySame, (unsigned long long)g_im2dStats.verifyDiffer,
                             (unsigned long long)std::exchange(g_regionReferenced, 0));
            if (g_kickTime.chains)
                std::fprintf(stderr, "[rsl-items] kick: %.3f ms a chain from its first tag to its hand-over (max %.3f), of it %.3f ms the items' resolve\n",
                             g_kickTime.ms / double(g_kickTime.chains), g_kickTime.maxMs, g_kickTime.resolveMs / double(g_kickTime.chains));
            g_kickTime = KickTime{};
            g_im2dStats = Im2dStats{};
            if (g_gaps)
                std::fprintf(stderr, "[rsl-items] gap points: %llu taken, %llu nothing new; not taken: %llu sector cache, %llu material cache, %llu no ops, %llu cursor elsewhere\n",
                             (unsigned long long)g_gapWhy[5], (unsigned long long)g_gapWhy[4], (unsigned long long)g_gapWhy[0],
                             (unsigned long long)g_gapWhy[1], (unsigned long long)g_gapWhy[2], (unsigned long long)g_gapWhy[3]);
            std::fill(std::begin(g_gapWhy), std::end(g_gapWhy), 0ull);
            if (g_itemVerify)
                std::fprintf(stderr, "[rsl-items] verify: %llu draws / ops wrote what their item stands for, %llu differ; blocks: %llu copied as the walker "
                                     "copies them, %llu differ\n",
                             (unsigned long long)s.verifySame, (unsigned long long)s.verifyDiffer,
                             (unsigned long long)g_itemBlockSameW.exchange(0), (unsigned long long)g_itemBlockDifferW.exchange(0));
            s = ItemStats{};
        }

        PS2Runtime::RecompiledFunction g_gsSendOriginal[2] = {}, g_texMatrixOriginal = nullptr, g_vuConstantsOriginal = nullptr,
                                       g_flushAOriginal = nullptr, g_textureBindOriginal = nullptr;

        // LCS_RSL_ITEMS_VERIFY: what a setter would have made an op of, compared with what its original writes (checkItem)
        void verifyOp(const uint8_t *rdram, const uint8_t *tags, uint32_t bytes)
        {
            checkItem(rdram);
            g_itemCheck.expect.assign(tags, tags + bytes);
            g_itemCheck.cursor = rd32(rdram, kPacketCursor);
            g_itemCheck.armed = bytes != 0u;
        }

        // FUN_002F7908 / FUN_002F7B30 (the same code, game-internals.md 6i): queue FOG / FOGCOL / registers 0x14 / 0x15 by
        // the dirty bits 0x80 / 0x100 / 0x20 / 0x40, clear bits 0x1E0, send the queue as CNT + DIRECTHL
        template <int Which>
        PS2_GUEST_CC void gsSendHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint32_t ra = getRegU32(ctx, 31);
            if (Which == 1 && ra == kMaterialCacheGs)
            {
                g_materialCacheEnds = true; // the material cache's own GS packet (copied with it): the packet path, and the
                                            // window's end - at the next hook point, after its bytes (step 5h: a gap there
                                            // would take this packet before the game copies it)
                ++g_opStats.caching;
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            const uint32_t dirty = rd32(rdram, kDirty);
            thread_local lcs::rslnative::Flush adds;
            adds.queued = 0;
            const struct
            {
                uint32_t bit, value, reg;
            } regs[4] = {{0x80u, 0x4882E0u, 6u}, {0x100u, 0x4882E8u, 7u}, {0x20u, 0x4882F0u, 0x14u}, {0x40u, 0x4882F8u, 0x15u}};
            for (const auto &g : regs)
                if (dirty & g.bit)
                {
                    uint64_t v;
                    std::memcpy(&v, rdram + g.value, 8);
                    adds.queue[adds.queued][0] = v;
                    adds.queue[adds.queued][1] = g.reg;
                    ++adds.queued;
                }
            if (g_itemVerify)
            {
                thread_local std::vector<uint8_t> e;
                e.clear();
                gsPacket(rdram, adds, e);
                verifyOp(rdram, e.data(), static_cast<uint32_t>(e.size()));
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            thread_local std::vector<uint8_t> vif;
            vif.clear();
            gsVif(rdram, adds, vif);
            if (!vif.empty() && (!opsAllowed() || !appendOp(rdram, vif.data(), static_cast<uint32_t>(vif.size()))))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                g_gsSendOriginal[Which](rdram, ctx, runtime);
                return;
            }
            const uint32_t count = rd32(rdram, kGsQueueCount); // the queue as the original leaves it
            for (uint32_t i = 0; i < adds.queued; ++i)
                std::memcpy(rdram + kGsQueue + (count + i) * 16u, adds.queue[i], 16);
            wr32(rdram, kGsQueueCount, vif.empty() ? count : 0u);
            wr32(rdram, kDirty, dirty & 0xFFFFFE1Fu);
            ctx->pc = ra;
        }

        // a setter's CNT (FLUSHE, UNPACK V4-32 x3 -> vu) with the three qwords at src, as an op
        bool unpack3Op(uint8_t *rdram, uint32_t code1, uint32_t src, PS2Runtime::RecompiledFunction original, R5900Context *ctx,
                       PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            uint8_t tags[16 + 48];
            const uint64_t tag[2] = {0x10000003u, 0x10000000ull | (uint64_t(code1) << 32)};
            std::memcpy(tags, tag, 16);
            if ((src & 0xF0000000u) == 0u)
                std::memcpy(tags + 16, rdram + (src & kRamMask), 48);
            if (g_itemVerify)
            {
                if ((src & 0xF0000000u) == 0u)
                    verifyOp(rdram, tags, sizeof(tags));
                original(rdram, ctx, runtime);
                return false;
            }
            if ((src & 0xF0000000u) != 0u || !opsAllowed() || !appendOp(rdram, tags + 8, sizeof(tags) - 8u))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                original(rdram, ctx, runtime);
                return false;
            }
            ctx->pc = getRegU32(ctx, 31);
            return true;
        }

        PS2_GUEST_CC void texMatrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            unpack3Op(rdram, 0x6C03002Cu, getRegU32(ctx, 4), g_texMatrixOriginal, ctx, runtime);
        }

        PS2_GUEST_CC void vuConstantsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (unpack3Op(rdram, getRegU32(ctx, 4) | 0x6C030000u, getRegU32(ctx, 5), g_vuConstantsOriginal, ctx, runtime))
                setReturnS32(ctx, 3); // its return value
        }

        PS2_GUEST_CC void flushAHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint64_t tag[2] = {0x10000000u, 0x1300000000000000ull}; // CNT 0: NOP, FLUSHA
            const auto *t = reinterpret_cast<const uint8_t *>(tag);
            if (g_itemVerify)
            {
                verifyOp(rdram, t, 16u);
                g_flushAOriginal(rdram, ctx, runtime);
                return;
            }
            if (!opsAllowed() || !appendOp(rdram, t + 8, 8u))
            {
                ++(opsAllowed() ? g_opStats.packets : g_opStats.caching);
                g_flushAOriginal(rdram, ctx, runtime);
                return;
            }
            ctx->pc = getRegU32(ctx, 31);
        }

        PS2Runtime::RecompiledFunction g_uploadOriginal = nullptr;

        // FUN_003298D8's transfer header for a destination, as its lookup finds it (game-internals.md 6i: a binary tree of
        // 0x70-byte nodes from 0x6EE3F0, key at +0x60, left +0x68, right +0x6C); 0: not there yet (the original makes it)
        uint32_t findTransferHeader(const uint8_t *rdram, uint64_t key)
        {
            uint32_t node = 0x6EE3F0u;
            for (int n = 0; n < 65536; ++n)
            {
                if ((node & 0xF0000000u) != 0u || (node & kRamMask) + 0x70u > kRamMask + 1u)
                    return 0u;
                uint64_t nodeKey;
                std::memcpy(&nodeKey, rdram + (node & kRamMask) + 0x60u, 8);
                uint32_t next;
                if (nodeKey > key)
                    next = rd32(rdram, node + 0x68u);
                else if (nodeKey == key)
                    return node;
                else
                    next = rd32(rdram, node + 0x6Cu);
                if (next == 0u)
                    return 0u;
                node = next;
            }
            return 0u;
        }

        // FUN_003298D8(slot, psm, log2 w, log2 h, bytes, source, destination): two REFs into the slot - the destination's
        // transfer header (FLUSHE, DIRECTHL 6) and the pixels (DIRECTHL bytes / 16) - as reference ops when the header exists
        PS2_GUEST_CC void uploadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint32_t at = getRegU32(ctx, 4), psm = getRegU32(ctx, 5), w = getRegU32(ctx, 6), h = getRegU32(ctx, 7);
            const uint32_t bytes = getRegU32(ctx, 8), source = getRegU32(ctx, 9), dest = getRegU32(ctx, 10);
            const uint64_t key = (uint64_t(int64_t(int32_t(dest >> 6))) << 48) | (uint64_t(psm & 0xFFFFu) << 32) | (w & 0xFFFFu) |
                                 (uint64_t(h & 0xFFFFu) << 16);
            const uint32_t qwc = bytes >> 4, node = findTransferHeader(rdram, key);
            const bool plain = node != 0u && qwc <= 0xFFFFu && (source & 0xF0000000u) == 0u && (source & kRamMask) + qwc * 16u <= kRamMask + 1u;
            if (g_itemVerify)
            {
                if (plain)
                {
                    checkItem(rdram);
                    const uint32_t tags[8] = {0x30000006u, node, 0x10000000u, 0x51000006u, qwc | 0x30000000u, source, 0u, qwc | 0x51000000u};
                    const auto *t = reinterpret_cast<const uint8_t *>(tags);
                    g_itemCheck.expect.assign(t, t + sizeof(tags));
                    g_itemCheck.cursor = at;
                    g_itemCheck.armed = true;
                }
                g_uploadOriginal(rdram, ctx, runtime);
                return;
            }
            const RefOp ops[2] = {{0u, {0x10000000u, 0x51000006u}, node, 6u}, {0u, {0u, qwc | 0x51000000u}, source, qwc}};
            if (!plain || !opsAllowed() || !appendRefs(rdram, at, ops, 2u))
            {
                ++(!plain || opsAllowed() ? g_opStats.packets : g_opStats.caching);
                g_uploadOriginal(rdram, ctx, runtime);
                return;
            }
            ++g_opStats.uploads;
            ctx->pc = getRegU32(ctx, 31);
        }

        PS2Runtime::RecompiledFunction g_capturedOriginal[kMaxEmitters] = {};

        // before the original only: the region it writes is captured at the next hook point on its stack level (settleIm2D)
        template <int E>
        PS2_GUEST_CC void captureHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t sp = getRegU32(ctx, 29);
            settleIm2D(rdram, sp, getRegU32(ctx, 31), ctx->pc);
            uint32_t cursor = 0;
            if (!g_inCapture && (opsAllowed() || (g_itemVerify && !g_caching && !g_materialCaching)) && cursorForItem(rdram, 16u, cursor))
            {
                g_im2dPending = true;
                g_im2dStart = cursor;
                g_captureSp = sp;
                g_captureEmitter = E;
                g_captureBind = kCaptured[E].addr == 0x002F62B8u || kCaptured[E].addr == 0x002F6A78u; // (game/lcs/functions.csv)
                g_captureBindFn[0] = kCaptured[E].addr;
                g_captureBindFn[1] = kCaptured[E].addr == 0x002F62B8u ? 0x002F6968u : 0x002F6DC8u;
                g_captureCurrentAt = g_current.size();
            }
            g_capturedOriginal[E](rdram, ctx, runtime);
        }

        // before the original only: the material cache's texture bind opens the window its packets are copied from
        PS2_GUEST_CC void textureBindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            g_materialCaching = getRegU32(ctx, 31) == kMaterialCacheBind;
            g_textureBindOriginal(rdram, ctx, runtime);
        }

        // FUN_002409C8 called by FUN_00240ED8 to cache the sector: its draws stay packets (until the next RSL draw call
        // after it - the cached block's own draw, or the sector drawn again when caching failed: game-internals.md 6i)
        PS2_GUEST_CC void sectorHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            g_caching = getRegU32(ctx, 31) == kSectorCaching;
            g_sectorOriginal(rdram, ctx, runtime);
        }

        // before the original only (DECISIONS.md "Rule for hooks on recompiled functions"), or the whole work (an item)
        // step 5f: a draw left to its original as a captured region (emitter index kDrawEmitter)
        void captureDraw(uint8_t *rdram, R5900Context *ctx, const Draw &d)
        {
            uint32_t cursor = 0;
            static const bool on = !(std::getenv("LCS_RSL_DRAW_CAPTURE") && *std::getenv("LCS_RSL_DRAW_CAPTURE") == '0');
            if (!on || g_im2dPending || g_inCapture || !(opsAllowed() || (g_itemVerify && !g_caching && !g_materialCaching)) ||
                !cursorForItem(rdram, 16u, cursor))
                return;
            g_im2dPending = true;
            g_im2dStart = cursor;
            g_captureSp = getRegU32(ctx, 29);
            g_captureEmitter = kDrawEmitter;
            g_captureBind = false;
            g_captureDraw = true;
            g_captureCurrentAt = g_current.size() - 1u;
            g_captureDrawFn[0] = d.kind == World ? 0x002F7EB0u : d.kind == Model ? 0x002F7D58u : 0x002F80A0u;
            g_captureDrawFn[1] = d.kind == World ? 0x002F8024u : d.kind == Model ? 0x002F7EACu : 0x002F8250u;
        }

        PS2_GUEST_CC void worldHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
            g_materialCaching = false;
            if (g_items && g_world && !g_itemVerify && itemDraw(rdram, ctx, World, a0, a1))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, World, a0, a1);
            g_current.push_back({3u, a0 & kRamMask, a1 & 0xFFFFu, World});
            if (g_items && g_world && (a0 & 0xF0000000u) == 0u && (a1 & 0x0FFF0000u) == 0u)
                captureDraw(rdram, ctx, g_current.back());
            g_worldOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint32_t a0 = getRegU32(ctx, 4);
            g_caching = false; // FUN_002409C8 draws no models: this is after the cached sector (FUN_00240ED8 draws it)
            if (g_items && !g_itemVerify && itemDraw(rdram, ctx, Model, a0, 0u))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, Model, a0, 0u);
            g_current.push_back({5u, a0 & kRamMask, 0u, Model});
            if (g_items && (a0 & 0xF0000000u) == 0u)
                captureDraw(rdram, ctx, g_current.back());
            g_modelOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void modelBHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            settleIm2D(rdram, getRegU32(ctx, 29), getRegU32(ctx, 31), ctx->pc); // steps 5c / 5d
            const uint32_t a0 = getRegU32(ctx, 4);
            g_caching = false;
            if (g_items && !g_itemVerify && itemDraw(rdram, ctx, ModelB, a0, 0u))
                return;
            if (g_itemVerify)
                verifyDraw(rdram, ModelB, a0, 0u);
            g_current.push_back({5u, a0 & kRamMask, 0u, ModelB});
            if (g_items && (a0 & 0xF0000000u) == 0u)
                captureDraw(rdram, ctx, g_current.back());
            g_modelBOriginal(rdram, ctx, runtime);
        }

        uint32_t mark(const Draw &d)
        {
            if (g_building.draws.size() >= 0xFFFFu)
                return 0u;
            g_building.draws.push_back(d);
            return ps2chain::kReplaceWithMark | static_cast<uint32_t>(g_building.draws.size() - 1u);
        }

        // LCS_RSL_CENSUS=1 (docs/FULLY_NATIVE.md step 5): what the chain still carries outside marked / item blocks - each
        // top-level tag by DMA id, the VIF command it starts with and where its data is, tags and quadwords per chain
        const char *censusWhere(uint32_t a)
        {
            a &= kRamMask;
            if (a >= kPktLo && a < kPktHi)
                return "packet buffer";
            if (a >= 0x6EDC70u && a < 0xCB7A80u)
                return "cells / heap";
            if (a >= 0x1200000u && a < 0x1800000u)
                return "model heap";
            if (a >= 0x100000u && a < 0x4D0000u)
                return "code / data";
            return "other";
        }
        const char *vifName(uint32_t code)
        {
            const uint32_t cmd = (code >> 24) & 0x7Fu;
            if (cmd >= 0x60u)
                return "UNPACK";
            switch (cmd)
            {
            case 0x00: return "NOP";
            case 0x01: return "STCYCL";
            case 0x07: return "MARK";
            case 0x10: return "FLUSHE";
            case 0x11: return "FLUSH";
            case 0x13: return "FLUSHA";
            case 0x14: return "MSCAL";
            case 0x20: return "STMASK";
            case 0x30: return "STROW";
            case 0x4A: return "MPG";
            case 0x50: return "DIRECT";
            case 0x51: return "DIRECTHL";
            default: return "other";
            }
        }
        struct CensusRow
        {
            uint64_t tags = 0, qwords = 0;
        };
        std::map<std::string, CensusRow> g_census;
        uint64_t g_censusChains = 0;
        auto g_censusAt = ps2x::hostNow();

        void census(uint32_t tagAddr, uint32_t id, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes, bool chainStart)
        {
            if (chainStart)
            {
                if (censusOrigins())
                {
                    g_originWalk.swap(g_originNow);
                    g_originNow.clear();
                    std::stable_sort(g_originWalk.begin(), g_originWalk.end(),
                                     [](const Origin &x, const Origin &y) { return x.cursor < y.cursor; });
                }
                ++g_censusChains;
                const auto now = ps2x::hostNow();
                if (now - g_censusAt > std::chrono::seconds(5) && g_censusChains > 1)
                {
                    g_censusAt = now;
                    const double c = double(g_censusChains - 1);
                    std::vector<std::pair<std::string, CensusRow>> rows(g_census.begin(), g_census.end());
                    std::sort(rows.begin(), rows.end(), [](const auto &x, const auto &y) { return x.second.tags > y.second.tags; });
                    uint64_t tags = 0, qw = 0;
                    for (const auto &[k, r] : rows)
                        tags += r.tags, qw += r.qwords;
                    std::fprintf(stderr, "[rsl-census] per chain: %.0f top-level tags, %.0f payload qwords\n", tags / c, qw / c);
                    for (size_t i = 0; i < rows.size() && i < (censusOrigins() ? 60u : 24u); ++i)
                        std::fprintf(stderr, "[rsl-census] %8.1f tags %9.1f qw  %s\n", rows[i].second.tags / c, rows[i].second.qwords / c,
                                     rows[i].first.c_str());
                    g_census.clear();
                    g_censusChains = 1;
                }
            }
            if (g_openBlock.open)
                return; // inside a marked packet (counted as that block)
            static const char *const ids[8] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
            const uint32_t code0 = static_cast<uint32_t>(vifCodes), code1 = static_cast<uint32_t>(vifCodes >> 32);
            const uint32_t first = ((code0 >> 24) & 0x7Fu) != 0u ? code0 : code1;
            std::string key = std::string(ids[id & 7u]) + " " + vifName(first);
            if (((code0 >> 24) & 0x7Fu) == 0x07u && (code1 & 0xFFFF0000u) == kItemNop)
                key = "item";
            key += std::string(" | tag in ") + censusWhere(tagAddr);
            if (qwc)
                key += std::string(", data in ") + censusWhere(dataAddr);
            if (censusOrigins() && key.compare(0, 4, "item") != 0 && (tagAddr & kRamMask) >= kPktLo && (tagAddr & kRamMask) < kPktHi)
            {
                // the hook point with the greatest cursor at or before the tag (the origins sorted by cursor at chain start)
                const uint32_t a = tagAddr & kRamMask;
                auto it = std::upper_bound(g_originWalk.begin(), g_originWalk.end(), a,
                                           [](uint32_t v, const Origin &o) { return v < o.cursor; });
                char buf[64];
                if (it == g_originWalk.begin())
                    std::snprintf(buf, sizeof(buf), " | after: (none)");
                else
                    std::snprintf(buf, sizeof(buf), " | after: %06x from %06x", (it - 1)->fn, (it - 1)->ra);
                key += buf;
            }
            CensusRow &r = g_census[key];
            ++r.tags;
            r.qwords += qwc;
        }

        uint32_t observe(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr, uint32_t qwc, uint64_t vifCodes,
                         uint32_t flatOffset, bool chainStart)
        {
            static const bool doCensus = std::getenv("LCS_RSL_CENSUS") != nullptr;
            if (doCensus)
                census(tagAddr, id, dataAddr, qwc, vifCodes, chainStart);
            if (g_itemVerify && !g_regionsWalk.empty()) // step 5h: a tag that reads inside a region an item took (REF data
            {                                           // anywhere in it, a CALL / NEXT past its start) - it would read
                uint32_t target = 0;                    // what replaced the region
                bool any = false;
                if ((id == 3u || id == 4u) && qwc)
                    target = dataAddr & kRamMask, any = true;
                else if (id == 2u || id == 5u)
                    target = tagTarget & kRamMask;
                auto it = std::upper_bound(g_regionsWalk.begin(), g_regionsWalk.end(), std::make_pair(target, ~0u));
                if (target && it != g_regionsWalk.begin())
                {
                    --it;
                    if (target < it->second && (any ? target >= it->first : target > it->first))
                    {
                        ++g_regionReferenced;
                        static int logged = 0;
                        if (logged++ < 20)
                            std::fprintf(stderr, "[rsl-items] verify: tag at 0x%08x (id %u) reads 0x%08x inside a taken region [0x%08x, 0x%08x)\n",
                                         tagAddr, id, target, it->first, it->second);
                    }
                }
            }
            if (chainStart)
            {
                g_chainStartAt = ps2x::hostNow();
                g_chainResolveMs = 0.0;
                g_building = ChainTable{};
                g_building.seq = ++g_chainSeq;
                g_openBlock.open = false;
                {
                    std::lock_guard<std::mutex> lock(g_confirmMutex); // the worker's confirmations since the last kick
                    for (const auto &[key, seq] : g_confirmPending)
                    {
                        uint64_t &c = g_confirmed[key];
                        c = std::max(c, seq);
                    }
                    g_confirmPending.clear();
                }
                if (!g_itemsKicked.empty()) // step 4c: the kicked frame's items (the fold has run: the walk starts after it)
                {
                    g_building.items = std::move(g_itemsKicked);
                    g_building.arena = std::move(g_arenaKicked);
                    g_building.refs = std::move(g_refsKicked);
                    g_refsKicked.clear();
                    g_building.itemFrame = g_itemFrameKicked;
                    g_itemsKicked.clear();
                    g_arenaKicked.clear();
                    const auto t0 = ps2x::hostNow();
                    resolveItems(g_building);
                    const double ms = std::chrono::duration<double, std::milli>(ps2x::hostNow() - t0).count();
                    g_chainResolveMs = ms;
                    static const bool slowLog = std::getenv("LCS_RSL_SLOW_LOG") != nullptr;
                    if (slowLog && ms > 3.0)
                    {
                        uint64_t refBytes = 0;
                        uint32_t maxQwc = 0, maxRefs = 0;
                        for (const RefOp &r : g_building.refs)
                        {
                            refBytes += r.qwc * 16u;
                            maxQwc = std::max(maxQwc, r.qwc);
                        }
                        for (const Item &it : g_building.items)
                            maxRefs = std::max(maxRefs, it.refCount);
                        std::fprintf(stderr, "[rsl-slow] resolve %.1f ms: %zu items, %zu refs (%llu KB, largest %u qw, most in one item %u), arena %zu KB\n",
                                     ms, g_building.items.size(), g_building.refs.size(), (unsigned long long)(refBytes >> 10), maxQwc, maxRefs,
                                     g_building.arena.size() >> 10);
                    }
                }
            }
            trackOpenBlock(tagAddr, id, dataAddr, qwc);
            tagAddr &= kRamMask;
            // a raster's pixels: a REF whose VIFcodes end in DIRECT / DIRECTHL of its own size (texture uploads,
            // FUN_003298D8: the header REF before it holds the GS registers and the IMAGE tag) - in the packet buffer or
            // in a CALLed packet (the world's cached sector setup). Not marked: the MARK would land inside the DIRECT's
            // data; found by its offset in the copy instead (onDirect, rasterAt).
            if (id == 3u && g_rasters && qwc)
            {
                const uint32_t code = static_cast<uint32_t>(vifCodes >> 32);
                if (((code >> 24) & 0x7Eu) == 0x50u && (code & 0xFFFFu) == qwc)
                {
                    g_building.rasters.push_back({flatOffset + 8u, dataAddr & kRamMask, qwc * 16u});
                    return 0u;
                }
            }
            if ((id != 3u && id != 5u) || tagAddr < kPktLo || tagAddr >= kPktHi)
                return 0u;
            const Draw *want = g_next < g_kicked.size() ? &g_kicked[g_next] : nullptr;
            if (id == 3u)
            {
                // a world draw's REF: the one the next draw call wrote
                if (g_world && qwc && want && want->id == 3u && (dataAddr & kRamMask) == want->addr && qwc == want->qwc)
                {
                    ++g_next;
                    const uint64_t snapshotKey = blockKey(World, want->addr, qwc);
                    uint32_t units = 0;
                    if (!g_openBlock.open && skippable(snapshotKey, World, want->addr, units))
                    {
                        Draw d = *want;
                        d.units = units;
                        if (g_skipVerify)
                            d.wouldSkip = true; // copied all the same (below), checked by the worker
                        else if (const uint32_t m = mark(d))
                        {
                            ++g_skipped;
                            return m | ps2chain::kSkipWithMark;
                        }
                        if (d.wouldSkip)
                        {
                            const uint32_t m = mark(d);
                            if (m)
                            {
                                std::vector<uint32_t> pages;
                                bool usable = true;
                                addPages(pages, usable, dataAddr, qwc * 16u);
                                closeBlock(snapshotKey, pages, usable, qwc * 2u);
                            }
                            return m;
                        }
                    }
                    const uint32_t m = mark(*want);
                    if (m && !g_openBlock.open)
                    {
                        std::vector<uint32_t> pages;
                        bool usable = true;
                        addPages(pages, usable, dataAddr, qwc * 16u);
                        closeBlock(snapshotKey, pages, usable, qwc * 2u);
                    }
                    return m;
                }
                return 0u;
            }
            if (!g_models)
                return 0u;
            // a CALL: a model draw's packet when it is the next draw call's, else another packet in the buffer
            Draw d{5u, tagTarget & kRamMask, 0u, OtherCall};
            if (want && want->id == 5u && want->addr == d.addr)
            {
                d.kind = want->kind;
                ++g_next;
            }
            {
                if (g_itemVerify && !g_openBlock.open) // what an item of this draw / other call would have copied
                {
                    thread_local std::vector<uint8_t> flat;
                    thread_local std::vector<uint32_t> pages;
                    flat.clear();
                    pages.clear();
                    bool usable = true;
                    if (flattenBlock(g_rdram, d.addr, flat, pages, usable, nullptr))
                        d.flatHash = hashBlock(flat.data(), static_cast<uint32_t>(flat.size()));
                }
            }
            uint32_t units = 0;
            if (!g_openBlock.open && skippable(blockKey(d.kind, d.addr, 0u), d.kind, d.addr, units))
            {
                d.units = units;
                if (g_skipVerify)
                    d.wouldSkip = true; // copied all the same, checked by the worker
                else if (const uint32_t m = mark(d))
                {
                    ++g_skipped;
                    return m | ps2chain::kSkipWithMark; // the walker goes on after the CALL
                }
            }
            const uint32_t m = mark(d);
            if (m && !g_openBlock.open) // the walker marks a CALL outside a marked packet only
            {
                g_openBlock = OpenBlock{};
                g_openBlock.open = true;
                g_openBlock.key = blockKey(d.kind, d.addr, 0u);
                g_openBlock.depth = 1;
            }
            return m;
        }

        // LCS builds sector caches inside its guest draw path. Record only the
        // final REF/CALL emitted by a completed draw, and match the actual DMA tag.
        constexpr uint32_t kLcsSectorCacheDrawReturn = 0x002037B0u;
        constexpr uint32_t kLcsPacketCursor = 0x0043EA88u;
        std::unordered_map<uint32_t, Draw> g_lcsPending, g_lcsChain;
        uint32_t g_lcsNextUploadRef = 0;

        uint32_t lcsStateItem(uint64_t codes, uint32_t addr, uint32_t qwc, bool raster)
        {
            const uint32_t bytes = qwc * 16u;
            if (g_building.items.size() >= kMaxItems ||
                (bytes && (addr > kRamMask || bytes > kRamMask + 1u - addr)))
                return 0u;
            const uint32_t index = static_cast<uint32_t>(g_building.items.size());
            Item &item = g_building.items.emplace_back();
            item.prefixAt = static_cast<uint32_t>(g_building.arena.size());
            item.prefixBytes = 8u + bytes;
            g_building.arena.resize(item.prefixAt + item.prefixBytes);
            auto *dest = g_building.arena.data() + item.prefixAt;
            std::memcpy(dest, &codes, sizeof(codes));
            if (bytes)
                std::memcpy(dest + 8u, g_rdram + addr, bytes);
            if (raster)
            {
                item.prefixRastersAt = static_cast<uint32_t>(g_building.itemRasters.size());
                item.prefixRasterCount = 1u;
                g_building.itemRasters.push_back({8u, addr, bytes});
            }
            return ps2chain::kStateItem | index;
        }

        PS2Runtime::RecompiledFunction g_lcsFlushAOriginal = nullptr;
        PS2Runtime::RecompiledFunction g_lcsConstantsOriginal = nullptr;

        PS2_GUEST_CC void lcsConstants(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            constexpr uint32_t base = kLcsPacketCursor - 0x18u;
            constexpr uint32_t packetSize = 64u;
            const uint32_t cursor = rd32(rdram, kLcsPacketCursor);
            const uint32_t source = getRegU32(ctx, 5);
            if (!cursor || (cursor & 15u) || (source & 15u) ||
                cursor > kRamMask + 1u - packetSize || source > kRamMask + 1u - 48u ||
                (source < cursor + packetSize && cursor < source + 48u))
            {
                g_lcsConstantsOriginal(rdram, ctx, runtime);
                return;
            }

            const uint32_t command = getRegU32(ctx, 4) | 0x6C030000u;
            const uint64_t tag[2] = {0x10000003ull, (uint64_t(command) << 32) | 0x10000000ull};
            const uint32_t next = cursor + packetSize;
            uint8_t packet[packetSize];
            std::memcpy(packet, tag, sizeof(tag));
            std::memcpy(packet + sizeof(tag), rdram + source, 48u);
            if (g_verify)
            {
                const uint32_t returnPc = getRegU32(ctx, 31);
                g_lcsConstantsOriginal(rdram, ctx, runtime);
                const bool exact = ctx->pc == returnPc && rd32(rdram, kLcsPacketCursor) == next &&
                                   getRegU32(ctx, 2) == 3u && std::memcmp(rdram + cursor, packet, packetSize) == 0;
                static uint64_t same = 0, different = 0;
                exact ? ++same : ++different;
                if (same + different == 1u || ((same + different) & 0xFFFu) == 0u || (!exact && different <= 4u))
                    std::fprintf(stderr, "[lcs-rsl] VU constants: %llu exact, %llu differ\n",
                                 (unsigned long long)same, (unsigned long long)different);
                return;
            }

            std::memcpy(rdram + cursor + 8u, packet + 8u, packetSize - 8u);
            if (!submitLcsState(rdram, cursor, tag[0]))
                std::memcpy(rdram + cursor, tag, 8u);
            std::memcpy(rdram + kLcsPacketCursor, &next, sizeof(next));
            SET_GPR_S32(ctx, 2, 3u);
            SET_GPR_VEC(ctx, 3, _mm_loadu_si128(reinterpret_cast<const __m128i *>(packet + 48u)));
            SET_GPR_S32(ctx, 4, cursor + 32u);
            SET_GPR_S32(ctx, 6, cursor + 8u);
            SET_GPR_S32(ctx, 7, base);
            SET_GPR_S32(ctx, 8, cursor + 48u);
            SET_GPR_S32(ctx, 9, next);
            ctx->pc = getRegU32(ctx, 31);
        }

        PS2Runtime::RecompiledFunction g_lcsFlushOriginal = nullptr;

        // Texture callbacks can update the dirty flags and the GS queue. Compare
        // the GS-only cases first, where the flush has no earlier side effects.

        constexpr uint32_t kLcsFlushEntry = 0x002A02F0u;
        constexpr uint32_t kLcsMatrixReturn = 0x002A0410u;
        constexpr uint32_t kLcsFlushPacketBase = kLcsPacketCursor - 0x18u;

        template <uint32_t Flag>
        auto lcsFlushSetupPacket(const uint8_t *rdram)
        {
            if constexpr (Flag == 0x0800u)
            {
                std::array<uint64_t, 28> packet{
                    0x10000000ull, 0x10000000ull, 0x10000005ull, 0x6C04000801000101ull,
                };
                std::memcpy(packet.data() + 4, rdram + 0x0043DA30u, 64u);
                packet[12] = 0x1400008Full;
                packet[14] = 0x10000006ull;
                packet[15] = 0x6C06003B00000000ull;
                std::memcpy(packet.data() + 16, rdram + 0x0043DAF0u, 96u);
                return packet;
            }
            else
            {
                static_assert(Flag == 0x0400u);
                std::array<uint64_t, 18> packet{
                    0x10000001ull, 0x6C01002010000000ull, 0, 0,
                    0x10000000ull, 0x10000000ull, 0x10000005ull, 0x6C04000401000101ull,
                };
                std::memcpy(packet.data() + 2, rdram + 0x0043DAE0u, 16u);
                std::memcpy(packet.data() + 8, rdram + 0x0043D9F0u, 64u);
                packet[16] = 0x14000083ull;
                return packet;
            }
        }

        bool lcsFlushPacketFits(uint32_t at, uint32_t bytes)
        {
            return at && !(at & 15u) && at <= kRamMask + 1u - bytes;
        }

        void writeLcsInlineState(uint8_t *rdram, uint32_t start, const uint64_t *packet, uint32_t bytes)
        {
            const auto *source = reinterpret_cast<const uint8_t *>(packet);
            for (uint32_t offset = 0; offset < bytes;)
            {
                const uint64_t cnt = packet[offset / 8u];
                const uint32_t length = 16u + static_cast<uint32_t>(cnt & 0xFFFFu) * 16u;
                if ((cnt & ~0xFFFFull) != 0x10000000ull || length > bytes - offset)
                {
                    std::memcpy(rdram + start, source, bytes);
                    return;
                }
                const uint32_t at = start + offset;
                std::memcpy(rdram + at + 8u, source + offset + 8u, length - 8u);
                if (!submitLcsState(rdram, at, cnt))
                    std::memcpy(rdram + at, &cnt, 8u);
                offset += length;
            }
        }

        PS2_GUEST_CC void lcsFlushMaterial(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto dirty = [&] { return READ32(GPR_U32(ctx, 28) + 0x188u); };
            const auto material = [&] { return READ32(GPR_U32(ctx, 28) + 0x18Cu); };
            const uint32_t entry = ctx->pc;

            // Each pass keeps its cursor in the guest's saved registers. The runtime can
            // resume either at the callback return or at the next loop iteration.
            const auto walk = [&](uint32_t offset, uint32_t resume, uint32_t callPc,
                                  bool firstPass, bool advance) {
                const uint32_t returnPc = callPc + 8u;
                while (true)
                {
                    uint32_t node = GPR_U32(ctx, 16);
                    if (advance)
                    {
                        ctx->pc = returnPc;
                        node = READ32(node + 0x64u);
                        SET_GPR_S32(ctx, 16, node);
                        const uint64_t more = GPR_U64(ctx, 16) ^ GPR_U64(ctx, 17);
                        SET_GPR_U64(ctx, 3, more);
                        if (!more)
                        {
                            SET_GPR_S32(ctx, 2, 1);
                            return true;
                        }
                        SET_GPR_VEC(ctx, 2, GPR_VEC(ctx, 0));
                        SET_GPR_S32(ctx, 2, READ32(node + offset));
                        ctx->pc = resume;
                        if (runtime->eeCheckpointDue())
                            return false;
                    }
                    else if (entry != resume)
                    {
                        SET_GPR_S32(ctx, 2, READ32(node + offset));
                    }

                    const uint32_t target = GPR_U32(ctx, 2);
                    if (firstPass)
                        SET_GPR_U64(ctx, 4, GPR_U64(ctx, 18));
                    if (target)
                    {
                        if (!firstPass)
                            SET_GPR_S32(ctx, 4, READ32(node + 0x34u));
                        SET_GPR_S32(ctx, 5, READ32(node + 0x54u));
                        SET_GPR_S32(ctx, 6, READ32(node + 0x58u));
                        SET_GPR_U32(ctx, 31, returnPc);
                        ctx->branch_pc = callPc;
                        ctx->pc = target;
                        if (!runtime->dispatchGuestBranch(rdram, ctx, target, callPc, returnPc,
                                                          PS2Runtime::GuestBranchKind::IndirectCall, "JALR"))
                            return false;
                    }
                    advance = true;
                }
            };

            if (entry == kLcsFlushEntry)
            {
                const uint32_t cursor = READ32(kLcsPacketCursor);
                if (!lcsFlushPacketFits(cursor, 16u))
                {
                    g_lcsFlushOriginal(rdram, ctx, runtime);
                    return;
                }
                const uint32_t sp = GPR_U32(ctx, 29) - 0x70u;
                SET_GPR_S32(ctx, 29, sp);
                for (uint32_t reg = 16; reg <= 19; ++reg)
                    WRITE64(sp + 0x40u + (reg - 16u) * 8u, GPR_U64(ctx, reg));
                WRITE64(sp + 0x60u, GPR_U64(ctx, 31));

                if (dirty() & 1u)
                {
                    const uint32_t object = material();
                    WRITE64(cursor, (uint64_t(object + 0x10u) << 32) | 0x50000000ull);
                    WRITE64(cursor + 8u, 0x1000000000000000ull);
                    WRITE32(kLcsPacketCursor, cursor + 16u);
                    SET_GPR_S32(ctx, 4, kLcsFlushPacketBase);
                    SET_GPR_S32(ctx, 5, 0x50000000u);
                    SET_GPR_U64(ctx, 6, 0x1000000000000000ull);
                    SET_GPR_S32(ctx, 3, cursor + 8u);
                    SET_GPR_S32(ctx, 18, object);
                    SET_GPR_S32(ctx, 17, object + 0xB0u);
                    SET_GPR_S32(ctx, 16, READ32(object + 0x114u));
                    if (GPR_U32(ctx, 16) != GPR_U32(ctx, 17))
                    {
                        SET_GPR_S32(ctx, 19, 1);
                        if (!walk(0x48u, 0x2A0378u, 0x2A0384u, true, false))
                            return;
                    }
                }
            }
            else if (entry == 0x2A0378u || entry == 0x2A038Cu)
            {
                if (!walk(0x48u, 0x2A0378u, 0x2A0384u, true, entry == 0x2A038Cu))
                    return;
            }

            if (entry <= 0x2A038Cu)
            {
                // Re-read the flags after the first callback pass.
                const uint32_t program = READ32(GPR_U32(ctx, 28) + 0x190u);
                SET_GPR_S32(ctx, 3, program);
                if (dirty() & 2u)
                {
                    const uint32_t cursor = READ32(kLcsPacketCursor);
                    if (!lcsFlushPacketFits(cursor, 16u))
                    {
                        SET_GPR_S32(ctx, 2, 2);
                        ctx->pc = 0x2A03ACu;
                        g_lcsFlushOriginal(rdram, ctx, runtime);
                        return;
                    }
                    WRITE64(cursor, (uint64_t(program) << 32) | 0x50000000ull);
                    WRITE64(cursor + 8u, 0x1000000000000000ull);
                    WRITE32(kLcsPacketCursor, cursor + 16u);
                    SET_GPR_S32(ctx, 3, cursor + 16u);
                    SET_GPR_S32(ctx, 4, kLcsFlushPacketBase);
                    SET_GPR_S32(ctx, 5, 0x50000000u);
                    SET_GPR_U64(ctx, 6, 0x1000000000000000ull);
                }
                if (dirty() & 4u)
                {
                    constexpr uint32_t callPc = 0x2A0408u;
                    constexpr uint32_t target = 0x1BA000u;
                    const uint32_t object = material();
                    SET_GPR_S32(ctx, 2, object);
                    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 29));
                    SET_GPR_S32(ctx, 5, READ32(object + 0x134u));
                    SET_GPR_S32(ctx, 6, READ32(GPR_U32(ctx, 28) + 0x194u));
                    SET_GPR_S32(ctx, 7, READ32(GPR_U32(ctx, 28) + 0x198u));
                    SET_GPR_U32(ctx, 31, kLcsMatrixReturn);
                    ctx->branch_pc = callPc;
                    ctx->pc = target;
                    if (!runtime->dispatchGuestBranch(rdram, ctx, target, callPc, kLcsMatrixReturn,
                                                      PS2Runtime::GuestBranchKind::DirectCall, "JAL"))
                        return;
                }
            }

            if (ctx->pc == kLcsMatrixReturn)
            {
                const uint32_t cursor = READ32(kLcsPacketCursor);
                if (!lcsFlushPacketFits(cursor, 64u))
                {
                    g_lcsFlushOriginal(rdram, ctx, runtime);
                    return;
                }
                uint64_t data[6];
                const uint32_t sp = GPR_U32(ctx, 29);
                for (uint32_t i = 0; i < 6u; ++i)
                    data[i] = ps2Read<uint64_t>(rdram, ctx, runtime, sp + i * 8u, kLcsMatrixReturn);
                if (!submitLcsState(rdram, cursor, 0x10000003ull))
                    WRITE64(cursor, 0x10000003ull);
                WRITE64(cursor + 8u, 0x1000000000000000ull);
                for (uint32_t i = 0; i < 6u; ++i)
                    WRITE64(cursor + 16u + i * 8u, data[i]);
                WRITE32(kLcsPacketCursor, cursor + 64u);
                SET_GPR_S32(ctx, 2, cursor + 8u);
                SET_GPR_U64(ctx, 4, data[5]);
                SET_GPR_U64(ctx, 5, data[2]);
                SET_GPR_U64(ctx, 6, data[3]);
            }

            if (entry <= kLcsMatrixReturn)
            {
                const uint32_t flags = dirty();
                if (flags & 0x11u)
                {
                    const uint32_t object = material();
                    const uint32_t head = object + 0xB0u;
                    SET_GPR_S32(ctx, 2, head);
                    SET_GPR_S32(ctx, 16, READ32(head + 0x64u));
                    SET_GPR_S32(ctx, 17, head);
                    SET_GPR_S32(ctx, 5, flags);
                    if (GPR_U32(ctx, 16) != head)
                    {
                        SET_GPR_S32(ctx, 18, 1);
                        const bool passOne = (flags & 1u) != 0u;
                        if (!walk(passOne ? 0x4Cu : 0x50u, passOne ? 0x2A04D0u : 0x2A0530u,
                                  passOne ? 0x2A04E0u : 0x2A0540u, false, false))
                            return;
                    }
                }
            }
            else
            {
                const bool passOne = entry <= 0x2A04E8u;
                if (!walk(passOne ? 0x4Cu : 0x50u, passOne ? 0x2A04D0u : 0x2A0530u,
                          passOne ? 0x2A04E0u : 0x2A0540u, false,
                          entry == 0x2A04E8u || entry == 0x2A0548u))
                    return;
            }

            const uint32_t flags = dirty();
            SET_GPR_S32(ctx, 5, flags);
            if (flags & 0x11u)
            {
                const uint32_t first = (flags & 0x0400u) ? 0u : 0x14000075u;
                const uint32_t second = (flags & 0x0200u) ? 0u : 0x1400004Au;
                SET_GPR_S32(ctx, 7, first);
                if (first || second)
                {
                    const uint32_t cursor = READ32(kLcsPacketCursor);
                    if (!lcsFlushPacketFits(cursor, 16u))
                    {
                        ctx->pc = 0x2A0564u;
                        g_lcsFlushOriginal(rdram, ctx, runtime);
                        return;
                    }
                    const uint64_t vif = (uint64_t(second) << 32) | first;
                    if (!submitLcsState(rdram, cursor, 0x10000000ull))
                        WRITE64(cursor, 0x10000000ull);
                    WRITE64(cursor + 8u, vif);
                    WRITE32(kLcsPacketCursor, cursor + 16u);
                    SET_GPR_U64(ctx, 4, vif);
                    SET_GPR_S32(ctx, 5, kLcsFlushPacketBase);
                    SET_GPR_S32(ctx, 6, 0x10000000u);
                    SET_GPR_S32(ctx, 8, second);
                }
            }

            ctx->pc = 0x2A0608u;
            g_lcsFlushOriginal(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void lcsRenderFlush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            constexpr uint32_t queueBase = 0x0065EFD8u;
            constexpr uint32_t dirtyMask = 0x80000000u;
            constexpr uint32_t gsMask = 0x1E0u;
            constexpr struct { uint32_t bit; uint32_t value; uint64_t reg; } state[] = {
                {0x080u, 0x003939D8u, 6u},
                {0x100u, 0x003939E0u, 7u},
                {0x020u, 0x003939E8u, 20u},
                {0x040u, 0x003939F0u, 21u},
            };
            const uint32_t gp = getRegU32(ctx, 28);
            if (ctx->pc != 0x002A02F0u || gp > kRamMask - 0x1D3u)
            {
                g_lcsFlushOriginal(rdram, ctx, runtime);
                return;
            }
            const uint32_t dirty = rd32(rdram, gp + 0x188u);
            if (!g_verify && (dirty & 0x0015u))
            {
                lcsFlushMaterial(rdram, ctx, runtime);
                return;
            }
            const uint32_t queued = rd32(rdram, gp + 0x1D0u);
            constexpr uint32_t constantsBit = 0x8u;
            constexpr uint32_t constantsSource = 0x0066AC90u;
            constexpr uint32_t constantsBytes = 80u;
            constexpr uint32_t programCallBit = 0x2u;
            constexpr uint32_t imageBit = 0x2000u;
            constexpr uint32_t setupBit = 0x1000u;
            constexpr uint32_t setupBytes = 144u;
            constexpr uint32_t supported = gsMask | constantsBit | programCallBit | imageBit | setupBit | 0x0800u | 0x0400u | 0x0200u | 0x0011u;
            const bool setup200 = (dirty & 0x0200u) != 0 && rd32(rdram, gp + 0x198u) == 0u;
            const uint32_t setupReference = setup200 ? rd32(rdram, gp + 0x1A0u) : 0u;
            const uint32_t setup200Bytes = setup200 ? (setupReference ? 32u : 96u) : 0u;
            const bool setup800 = (dirty & 0x0800u) != 0;
            const bool setup400 = (dirty & 0x0400u) != 0;
            const bool setup = (dirty & setupBit) != 0;
            const bool image = (dirty & imageBit) != 0;
            constexpr uint32_t packetBase = kLcsPacketCursor - 0x18u;
            const bool programCall = (dirty & programCallBit) != 0;
            const bool constants = (dirty & constantsBit) != 0u;
            const bool materialCall = (dirty & 0x0001u) != 0;
            const uint32_t material = (dirty & 0x0011u) ? rd32(rdram, gp + 0x18Cu) : 0u;
            if ((dirty & 0x0011u) &&
                (!material || (material & 3u) || material > kRamMask - 0x117u ||
                 rd32(rdram, material + 0x114u) != material + 0xB0u))
            {
                // Callbacks can emit packets and modify flags. Keep the guest continuation
                // when the material has a callback list.
                g_lcsFlushOriginal(rdram, ctx, runtime);
                return;
            }
            const bool materialMscal = (dirty & 0x0011u) && (dirty & 0x0600u) != 0x0600u;
            const uint64_t materialTag[2] = {
                (uint64_t(material + 0x10u) << 32) | 0x50000000ull, 0x1000000000000000ull,
            };
            const uint64_t mscalTag[2] = {
                0x10000000ull,
                (dirty & 0x0400u ? 0ull : 0x14000075ull) |
                    (dirty & 0x0200u ? 0ull : 0x1400004A00000000ull),
            };
            const uint32_t cursor = rd32(rdram, kLcsPacketCursor);
            const uint32_t callBytes = programCall ? 16u : 0u;
            const uint32_t callAt = cursor + (materialCall ? 16u : 0u);
            const uint32_t mscalAt = callAt + callBytes;
            const uint32_t imageAt = mscalAt + (materialMscal ? 16u : 0u);
            const uint32_t constantsAt = imageAt + (image ? 48u : 0u);
            const uint32_t setupAt = constantsAt + (constants ? 96u : 0u);
            const uint32_t setup800At = setupAt + (setup ? setupBytes : 0u);
            const uint32_t setup400At = setup800At + (setup800 ? 224u : 0u);
            const uint32_t setup200At = setup400At + (setup400 ? 144u : 0u);
            const uint32_t packetBytes = setup200At - cursor + setup200Bytes;
            if ((dirty & supported) == 0u || (dirty & ~(supported | dirtyMask)) != 0u ||
                queued > (kRamMask + 1u - queueBase) / 16u - std::size(state) ||
                (packetBytes && (!cursor || (cursor & 15u) || cursor > kRamMask + 1u - packetBytes)) ||
                (constants && cursor < constantsSource + constantsBytes && constantsSource < cursor + packetBytes))
            {
                g_lcsFlushOriginal(rdram, ctx, runtime);
                return;
            }

            uint64_t writes[std::size(state)][2]{};
            uint32_t count = 0;
            for (const auto &entry : state)
            {
                if ((dirty & entry.bit) == 0u)
                    continue;
                std::memcpy(&writes[count][0], rdram + entry.value, sizeof(uint64_t));
                writes[count++][1] = entry.reg;
            }
            const uint32_t returnPc = getRegU32(ctx, 31);
            const uint64_t call[2] = {(uint64_t(rd32(rdram, gp + 0x190u)) << 32) | 0x50000000ull,
                                      0x1000000000000000ull};
            constexpr uint32_t imageTable = 0x0066AC60u;
            uint64_t imageTags[6]{};
            uint32_t lastImageLink = 0;
            if (image)
            {
                const uint32_t slot = rd32(rdram, 0x003DA0CCu);
                const auto readable = [](uint32_t addr, uint32_t size) {
                    return addr && !(addr & 3u) && addr <= kRamMask + 1u - size;
                };
                if (slot > (kRamMask - imageTable - 3u) / 4u)
                {
                    g_lcsFlushOriginal(rdram, ctx, runtime);
                    return;
                }
                const uint32_t links = rd32(rdram, imageTable + slot * 4u);
                const uint32_t fixed = rd32(rdram, imageTable + 0x8000u);
                if (!readable(links, 12u) || !readable(fixed, 4u))
                {
                    g_lcsFlushOriginal(rdram, ctx, runtime);
                    return;
                }
                const uint32_t first = rd32(rdram, fixed);
                const uint32_t second = rd32(rdram, links + 4u);
                lastImageLink = rd32(rdram, links + 8u);
                if (!readable(first, 4u) || !readable(second, 4u) || !readable(lastImageLink, 4u))
                {
                    g_lcsFlushOriginal(rdram, ctx, runtime);
                    return;
                }
                imageTags[0] = (uint64_t(rd32(rdram, first)) << 32) | 0x50000000ull;
                imageTags[1] = 0x10000000ull;
                imageTags[2] = (uint64_t(rd32(rdram, second)) << 32) | 0x50000000ull;
                imageTags[4] = (uint64_t(rd32(rdram, lastImageLink)) << 32) | 0x50000000ull;
            }
            uint64_t packet[12] = {0x10000005ull, 0x6C05003110000000ull};
            if (constants)
                std::memcpy(packet + 2, rdram + constantsSource, constantsBytes);
            uint64_t setupPacket[setupBytes / 8u] = {
                0x10000000ull, 0x10000000ull, 0x10000005ull, 0x6C04000C01000101ull,
            };
            if (setup)
            {
                std::memcpy(setupPacket + 4, rdram + 0x0043DA70u, 64u);
                setupPacket[12] = 0x1400008Full;
                setupPacket[14] = 0x10000001ull;
                setupPacket[15] = 0x6C01004110000000ull;
                std::memcpy(setupPacket + 16, rdram + 0x0043DC10u, 16u);
            }
            const auto packet800 = setup800 ? lcsFlushSetupPacket<0x0800u>(rdram) : std::array<uint64_t, 28>{};
            const auto packet400 = setup400 ? lcsFlushSetupPacket<0x0400u>(rdram) : std::array<uint64_t, 18>{};
            uint64_t packet200[12]{};
            if (setup200 && setupReference)
            {
                // REF four qwords with UNPACK, followed by a CNT carrying MSCAL.
                packet200[0] = (uint64_t(setupReference) << 32) | 0x30000004ull;
                packet200[1] = uint64_t(0xD809u) << 47;
                packet200[2] = 0x10000000u;
                packet200[3] = 0x1400003Cu;
            }
            else if (setup200)
            {
                packet200[0] = 0x10000005u;
                packet200[1] = (((uint64_t(0xD809u) << 38) | 0x8000u) << 9) | 0x101u;
                std::memcpy(packet200 + 2, rdram + 0x0043D9B0u, 64u);
                packet200[10] = 0x1400003Cu;
            }
            if (!g_verify && (setup800 || setup400 || (dirty & 0x0200u)))
            {
                g_lcsFlushOriginal(rdram, ctx, runtime);
                return;
            }
            if (!g_verify)
            {
                if (programCall)
                {
                    std::memcpy(rdram + callAt, call, sizeof(call));
                    SET_GPR_S32(ctx, 4, packetBase);
                    SET_GPR_S32(ctx, 5, 0x50000000u);
                }
                if (image)
                {
                    std::memcpy(rdram + imageAt, imageTags, sizeof(imageTags));
                    SET_GPR_S32(ctx, 4, lastImageLink);
                    SET_GPR_S32(ctx, 5, imageAt + 8u);
                    SET_GPR_S32(ctx, 7, imageTable);
                    SET_GPR_S32(ctx, 8, 0x50000000u);
                    SET_GPR_S32(ctx, 9, imageAt + 32u);
                    SET_GPR_S32(ctx, 10, imageAt + 24u);
                    SET_GPR_S32(ctx, 11, imageAt + 40u);
                    SET_GPR_S32(ctx, 12, imageAt + 48u);
                }
                if (constants)
                {
                    std::memcpy(rdram + constantsAt + 8u, packet + 1, sizeof(packet) - 8u);
                    if (!submitLcsState(rdram, constantsAt, packet[0]))
                        std::memcpy(rdram + constantsAt, packet, 8u);
                    SET_GPR_S32(ctx, 4, constantsAt + 80u);
                    SET_GPR_U64(ctx, 5, packet[8]);
                    SET_GPR_U64(ctx, 7, packet[10]);
                    SET_GPR_U64(ctx, 8, packet[11]);
                }
                if (setup)
                {
                    writeLcsInlineState(rdram, setupAt, setupPacket, sizeof(setupPacket));
                    SET_GPR_S32(ctx, 4, 0x0043DC10u);
                    SET_GPR_S32(ctx, 5, setupAt + 128u);
                    SET_GPR_S32(ctx, 7, setupAt + 144u);
                    SET_GPR_S32(ctx, 8, setupAt + 120u);
                    SET_GPR_S32(ctx, 9, 0x10000001u);
                    SET_GPR_S32(ctx, 10, setupAt + 112u);
                    SET_GPR_S32(ctx, 11, 0x10000000u);
                    SET_GPR_S32(ctx, 12, setupAt + 100u);
                    SET_GPR_S32(ctx, 13, setupAt + 96u);
                    SET_GPR_S32(ctx, 14, setupAt + 104u);
                    SET_GPR_S32(ctx, 15, setupAt + 108u);
                }
                if (packetBytes)
                    wr32(rdram, kLcsPacketCursor, cursor + packetBytes);
                if (count)
                {
                    const uint32_t address = queueBase + queued * 16u;
                    std::memcpy(rdram + address, writes, count * 16u);
                    wr32(rdram, gp + 0x1D0u, queued + count);
                    SET_GPR_S32(ctx, 4, queueBase);
                    SET_GPR_U64(ctx, 5, writes[count - 1u][1]);
                    SET_GPR_U64(ctx, 7, writes[count - 1u][0]);
                }
                SET_GPR_S32(ctx, 6, queued + count);
                wr32(rdram, gp + 0x188u, dirty & dirtyMask);
                SET_GPR_S32(ctx, 2, dirty & dirtyMask);
                SET_GPR_S32(ctx, 3, dirtyMask);
                ctx->pc = returnPc;
                return;
            }

            g_lcsFlushOriginal(rdram, ctx, runtime);
            const bool exact = ctx->pc == returnPc &&
                               rd32(rdram, kLcsPacketCursor) == cursor + packetBytes &&
                               (!setup200 || std::memcmp(rdram + setup200At, packet200, setup200Bytes) == 0) &&
                               (!setup800 || std::memcmp(rdram + setup800At, packet800.data(), sizeof(packet800)) == 0) &&
                               (!setup400 || std::memcmp(rdram + setup400At, packet400.data(), sizeof(packet400)) == 0) &&
                               (!setup || std::memcmp(rdram + setupAt, setupPacket, sizeof(setupPacket)) == 0) &&
                               (!image || std::memcmp(rdram + imageAt, imageTags, sizeof(imageTags)) == 0) &&
                               (!materialCall || std::memcmp(rdram + cursor, materialTag, sizeof(materialTag)) == 0) &&
                               (!materialMscal || std::memcmp(rdram + mscalAt, mscalTag, sizeof(mscalTag)) == 0) &&
                               (!programCall || std::memcmp(rdram + callAt, call, sizeof(call)) == 0) &&
                               (!constants || std::memcmp(rdram + constantsAt, packet, sizeof(packet)) == 0) &&
                               rd32(rdram, gp + 0x188u) == (dirty & dirtyMask) &&
                               rd32(rdram, gp + 0x1D0u) == queued + count &&
                               std::memcmp(rdram + queueBase + queued * 16u, writes, count * 16u) == 0 &&
                               getRegU32(ctx, 2) == (dirty & dirtyMask) && getRegU32(ctx, 3) == dirtyMask &&
                               getRegU32(ctx, 6) == queued + count &&
                               ((setup800 || setup400 || (dirty & 0x0211u)) || (count ? getRegU32(ctx, 4) == queueBase && GPR_U64(ctx, 5) == writes[count - 1u][1] &&
                                        GPR_U64(ctx, 7) == writes[count - 1u][0]
                                      : setup ? getRegU32(ctx, 4) == 0x0043DC10u &&
                                                getRegU32(ctx, 5) == setupAt + 128u && getRegU32(ctx, 7) == setupAt + 144u
                                              : constants ? getRegU32(ctx, 4) == constantsAt + 80u && GPR_U64(ctx, 5) == packet[8] &&
                                                    GPR_U64(ctx, 7) == packet[10]
                                                  : image ? getRegU32(ctx, 4) == lastImageLink && getRegU32(ctx, 5) == imageAt + 8u &&
                                                            getRegU32(ctx, 7) == imageTable
                                                          : getRegU32(ctx, 4) == packetBase && getRegU32(ctx, 5) == 0x50000000u)) &&
                               ((setup800 || setup400 || (dirty & 0x0211u)) || (setup ? getRegU32(ctx, 8) == setupAt + 120u
                                      : !constants || GPR_U64(ctx, 8) == packet[11]));
            static uint64_t same = 0, different = 0;
            if (exact)
                ++same;
            else
                ++different;
            if (!exact && different <= 4u)
                std::fprintf(stderr, "[lcs-rsl] flush mismatch dirty=%08x cursor=%08x->%08x (expected %08x) queue=%u->%u (expected %u)\n",
                             dirty, cursor, rd32(rdram, kLcsPacketCursor), cursor + packetBytes,
                             queued, rd32(rdram, gp + 0x1D0u), queued + count);
            if (same + different == 1u || ((same + different) & 0xFFFu) == 0u || (!exact && different <= 4u))
                std::fprintf(stderr, "[lcs-rsl] GS/constants flush: %llu exact, %llu differ\n",
                             (unsigned long long)same, (unsigned long long)different);
        }


        PS2_GUEST_CC void lcsFlushA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            constexpr uint64_t tag[2] = {0x10000000ull, 0x1300000000000000ull};
            const uint32_t cursor = rd32(rdram, kLcsPacketCursor);
            if ((cursor & 15u) || !cursor || cursor > kRamMask + 1u - sizeof(tag))
            {
                g_lcsFlushAOriginal(rdram, ctx, runtime);
                return;
            }

            const uint32_t next = cursor + sizeof(tag);
            if (g_verify)
            {
                const uint32_t returnPc = getRegU32(ctx, 31);
                g_lcsFlushAOriginal(rdram, ctx, runtime);
                const bool exact = ctx->pc == returnPc && rd32(rdram, kLcsPacketCursor) == next &&
                                   std::memcmp(rdram + cursor, tag, sizeof(tag)) == 0 &&
                                   getRegU32(ctx, 2) == cursor + 8u && getRegU32(ctx, 3) == kLcsPacketCursor - 0x18u &&
                                   getRegU32(ctx, 4) == next && GPR_U64(ctx, 5) == tag[1];
                static uint64_t same = 0, different = 0;
                if (exact)
                    ++same;
                else
                    ++different;
                if (same + different == 1u || ((same + different) & 0xFFFu) == 0u || (!exact && different <= 4u))
                    std::fprintf(stderr, "[lcs-rsl] FLUSHA: %llu exact, %llu differ\n",
                                 (unsigned long long)same, (unsigned long long)different);
                return;
            }

            std::memcpy(rdram + cursor + 8u, tag + 1, 8u);
            if (!submitLcsState(rdram, cursor, tag[0]))
                std::memcpy(rdram + cursor, tag, 8u);
            std::memcpy(rdram + kLcsPacketCursor, &next, sizeof(next));
            SET_GPR_S32(ctx, 2, cursor + 8u);
            SET_GPR_S32(ctx, 3, kLcsPacketCursor - 0x18u);
            SET_GPR_S32(ctx, 4, next);
            SET_GPR_U64(ctx, 5, tag[1]);
            ctx->pc = getRegU32(ctx, 31);
        }


        // Called only after skippable confirmed the write-watch snapshot. Queued
        // chains retain their immutable fallback even if this cache is cleared.
        std::shared_ptr<const ConfirmedBlock> lcsConfirmedBlock(uint64_t key, const Draw &draw, uint32_t units)
        {
            struct Entry
            {
                uint64_t stableSince;
                std::shared_ptr<const ConfirmedBlock> bytes;
            };
            constexpr size_t cacheLimit = 16u * 1024u * 1024u;
            static std::unordered_map<uint64_t, Entry> cache;
            static size_t cachedBytes = 0;
            const uint64_t stableSince = g_snapshots.find(key)->stableSince;
            const auto it = cache.find(key);
            if (it != cache.end())
            {
                if (it->second.stableSince == stableSince)
                    return it->second.bytes;
                cachedBytes -= it->second.bytes->bytes.size();
                cache.erase(it);
            }

            auto bytes = std::make_shared<ConfirmedBlock>();
            if (draw.kind == World)
            {
                const size_t length = size_t(draw.qwc) * 16u;
                if (length > kRamMask + 1u - draw.addr)
                    return {};
                bytes->bytes.assign(g_rdram + draw.addr, g_rdram + draw.addr + length);
            }
            else
            {
                std::vector<uint32_t> pages;
                bool usable = true;
                if (!flattenBlock(g_rdram, draw.addr, bytes->bytes, pages, usable, &bytes->rasters, true) || !usable)
                    return {};
            }
            if (bytes->bytes.empty() || bytes->bytes.size() != size_t(units) * 8u)
                return {};
            bytes->hash = hashBlock(bytes->bytes.data(), static_cast<uint32_t>(bytes->bytes.size()));
            g_lcsPreparedBytes.fetch_add(bytes->bytes.size(), std::memory_order_relaxed);
            if (bytes->bytes.size() <= cacheLimit)
            {
                if (cachedBytes + bytes->bytes.size() > cacheLimit)
                {
                    cache.clear();
                    cachedBytes = 0;
                }
                cachedBytes += bytes->bytes.size();
                cache.emplace(key, Entry{stableSince, bytes});
            }
            return bytes;
        }

        uint32_t observeLcs(uint32_t tagAddr, uint32_t id, uint32_t tagTarget, uint32_t dataAddr,
                            uint32_t qwc, uint64_t vifCodes, uint32_t flatOffset, bool chainStart)
        {
            if (chainStart)
            {
                g_lcsChain.clear();
                g_lcsChain.swap(g_lcsPending);
                g_lcsNextUploadRef = 0;
                g_building = ChainTable{};
                g_building.seq = ++g_chainSeq;
                g_openBlock.open = false;
                if (g_skip)
                {
                    std::lock_guard<std::mutex> lock(g_confirmMutex);
                    for (const auto &[key, seq] : g_confirmPending)
                    {
                        auto &confirmed = g_confirmed[key];
                        confirmed = std::max(confirmed, seq);
                    }
                    g_confirmPending.clear();
                }
            }
            const bool insideCall = g_openBlock.open;
            if (g_skip)
                trackOpenBlock(tagAddr, id, dataAddr, qwc);
            // LCS's texture upload emits a DIRECTHL header REF followed by its pixel REF.
            // Keep the source identity at the offset at which the GS receives the pixels.
            const uint32_t command = static_cast<uint32_t>(vifCodes >> 32);
            const bool directHl = qwc > 0u && command == (0x51000000u | qwc);
            const bool flushDirectHl = directHl && static_cast<uint32_t>(vifCodes) == 0x10000000u;
            const bool uploadHeader = id == 3u && qwc == 6u && flushDirectHl;
            const bool uploadFollowsHeader = g_lcsNextUploadRef && tagAddr == g_lcsNextUploadRef;
            g_lcsNextUploadRef = uploadHeader ? tagAddr + 16u : 0u;
            if (!insideCall && !g_verify && !g_skipVerify)
            {
                if (uploadHeader)
                    return lcsStateItem(vifCodes, dataAddr, qwc, false);
                if (id == 1u && (tagTarget == ps2chain::kLcsStateSlotTarget || flushDirectHl))
                    return lcsStateItem(vifCodes, dataAddr, qwc, false);
            }
            if (id == 3u && qwc && static_cast<uint32_t>(vifCodes) == 0u &&
                ((command >> 24) & 0x7Eu) == 0x50u && (command & 0xFFFFu) == qwc)
            {
                g_building.rasters.push_back({flatOffset + 8u, dataAddr & kRamMask, qwc * 16u});
                if (uploadFollowsHeader && !insideCall && !g_verify && !g_skipVerify)
                    return lcsStateItem(vifCodes, dataAddr, qwc, true);
                return 0u;
            }
            if (id != 3u && id != 5u)
                return 0u;
            const auto found = g_lcsChain.find(tagAddr);
            if (found == g_lcsChain.end())
                return 0u;
            Draw draw = found->second;
            g_lcsChain.erase(found);
            if (draw.id != id || draw.qwc != qwc || draw.addr != (id == 3u ? dataAddr : tagTarget))
                return 0u;
            // Reuse the host recording of a complete sector display list only while
            // all its tag and payload pages are still at the recorded generations.
            if (!g_skip || insideCall || (id == 3u && !qwc))
                return mark(draw);

            const uint64_t key = blockKey(draw.kind, draw.addr, id == 3u ? qwc : 0u);
            uint32_t units = 0;
            if ((vifCodes & 0xFF000000FF000000ull) != 0u)
                ++g_lcsTagCommands;
            else if (skippable(key, draw.kind, draw.addr, units))
            {
                if (auto bytes = lcsConfirmedBlock(key, draw, units))
                {
                    draw.units = units;
                    if (g_skipVerify)
                        draw.wouldSkip = true;
                    else
                    {
                        const uint32_t index = static_cast<uint32_t>(g_building.draws.size());
                        if (const uint32_t marker = mark(draw))
                        {
                            const uint32_t itemIndex = static_cast<uint32_t>(g_building.items.size());
                            Item &item = g_building.items.emplace_back();
                            item.d = draw;
                            item.confirmed = std::move(bytes);
                            g_building.lcsDrawItems.emplace(index, itemIndex);
                            ++g_skipped;
                            return marker | ps2chain::kSkipWithMark;
                        }
                        return 0u;
                    }
                }
                else
                    ++g_lcsUnsupported;
            }
            else if (const PageSnapshot *snapshot = g_snapshots.find(key))
                ++(unchanged(*snapshot) ? g_lcsUnconfirmed : g_lcsDirty);
            else
                ++g_lcsNoSnapshot;

            const uint32_t marker = mark(draw);
            if (marker && id == 3u)
            {
                std::vector<uint32_t> pages;
                bool usable = true;
                addPages(pages, usable, dataAddr, qwc * 16u);
                closeBlock(key, pages, usable, qwc * 2u);
            }
            else if (marker)
            {
                g_openBlock = OpenBlock{};
                g_openBlock.open = true;
                g_openBlock.key = key;
                g_openBlock.depth = 1;
            }
            return marker;
        }

        void queued(const void *key, uint32_t sizeBytes)
        {
            if (g_chainStartAt != std::chrono::steady_clock::time_point{})
            {
                const double ms = std::chrono::duration<double, std::milli>(ps2x::hostNow() - g_chainStartAt).count();
                ++g_kickTime.chains;
                g_kickTime.ms += ms;
                g_kickTime.resolveMs += g_chainResolveMs;
                g_kickTime.maxMs = std::max(g_kickTime.maxMs, ms);
                g_chainStartAt = {};
            }
            g_lastChainBytes.store(sizeBytes, std::memory_order_relaxed);
            if (!g_lcsInstalled && g_building.draws.empty() && g_building.rasters.empty() && g_building.items.empty())
                return;
            auto table = std::make_shared<ChainTable>(std::move(g_building));
            table->chainBytes = sizeBytes;
            g_building = ChainTable{};
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            g_tables[key] = std::move(table);
            // An LCS chain can contain markers whose payload lives only in its
            // table. The worker removes that table when it starts the chain.
            if (!g_lcsInstalled && g_tables.size() > 8u)
                g_tables.erase(g_tables.begin());
        }

        thread_local std::chrono::steady_clock::time_point t_chainStart;
        void processing(const void *key, bool begin)
        {
            static const bool slowLog = std::getenv("LCS_RSL_SLOW_LOG") != nullptr;
            if (slowLog && !begin)
            {
                const double ms = std::chrono::duration<double, std::milli>(ps2x::hostNow() - t_chainStart).count();
                if (ms > 15.0)
                    std::fprintf(stderr, "[rsl-slow] chain processed in %.1f ms: %zu items, arena %zu KB, %zu refs\n", ms,
                                 t_table ? t_table->items.size() : size_t(0), t_table ? t_table->arena.size() >> 10 : size_t(0),
                                 t_table ? t_table->refs.size() : size_t(0));
            }
            if (!begin)
            {
                if (t_table && t_table.use_count() == 1 && (t_table->arena.capacity() || t_table->items.capacity()))
                {
                    std::lock_guard<std::mutex> lock(g_poolMutex); // step 4c: the arena and the item list for a later frame
                    if (g_arenaPool.size() < 4u)
                        g_arenaPool.push_back(std::move(t_table->arena));
                    if (g_itemPool.size() < 4u)
                        g_itemPool.push_back(std::move(t_table->items));
                }
                t_table.reset();
                t_chain = nullptr;
                if (!t_confirm.empty())
                {
                    std::lock_guard<std::mutex> lock(g_confirmMutex);
                    g_confirmPending.insert(g_confirmPending.end(), t_confirm.begin(), t_confirm.end());
                    t_confirm.clear();
                }
                return;
            }
            t_chain = static_cast<const uint8_t *>(key);
            t_seen.clear();
            t_chainStart = ps2x::hostNow();
            ++g_measure.chains;
            std::lock_guard<std::mutex> lock(g_tablesMutex);
            const auto it = g_tables.find(key);
            if (it == g_tables.end())
            {
                g_measure.chainBytes += g_lastChainBytes.load(std::memory_order_relaxed);
                return;
            }
            g_measure.chainBytes += it->second->chainBytes;
            t_table = std::move(it->second);
            g_tables.erase(it);
        }

        bool off(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e == '0';
        }

        bool hook(PS2Runtime &runtime, uint32_t addr, PS2Runtime::RecompiledFunction &original, PS2Runtime::RecompiledFunction replacement)
        {
            original = runtime.lookupFunction(addr);
            if (!original)
                return false;
            runtime.replaceFunction(addr, replacement);
            return true;
        }

        template <int... I>
        void hookCaptured(PS2Runtime &runtime, std::integer_sequence<int, I...>)
        {
            // LCS_RSL_CAPTURE_MASK: a bit per kCaptured entry (default all)
            const char *m = std::getenv("LCS_RSL_CAPTURE_MASK");
            const unsigned long mask = m ? std::strtoul(m, nullptr, 0) : ~0ul;
            ((((mask >> I) & 1ul) ? (void)hook(runtime, kCaptured[I].addr, g_capturedOriginal[I], &captureHook<I>) : (void)0), ...);
        }
    }


    uint32_t captureLcsFlushSetup(uint8_t *rdram, uint32_t flag, uint64_t *expected)
    {
        if (!g_lcsFlushOriginal || !g_verify)
            return 0u;
        const uint32_t at = rd32(rdram, kLcsPacketCursor);
        if (flag == 0x0800u && lcsFlushPacketFits(at, 224u))
        {
            const auto packet = lcsFlushSetupPacket<0x0800u>(rdram);
            std::memcpy(expected, packet.data(), sizeof(packet));
            return at;
        }
        if (flag == 0x0400u && lcsFlushPacketFits(at, 144u))
        {
            const auto packet = lcsFlushSetupPacket<0x0400u>(rdram);
            std::memcpy(expected, packet.data(), sizeof(packet));
            return at;
        }
        return 0u;
    }

    void checkLcsFlushSetup(uint8_t *rdram, uint32_t flag, uint32_t at, const uint64_t *expected)
    {
        const uint32_t bytes = flag == 0x0800u ? 224u : 144u;
        const bool match = rd32(rdram, kLcsPacketCursor) == at + bytes &&
                           std::memcmp(rdram + at, expected, bytes) == 0;
        struct Counts { uint64_t total = 0, mismatches = 0; };
        static Counts counts[2];
        auto &count = counts[flag == 0x0800u];
        ++count.total;
        count.mismatches += !match;
        g_setupsCompared.fetch_add(1, std::memory_order_relaxed);
        if (!match)
            g_setupsDiffer.fetch_add(1, std::memory_order_relaxed);
        if (count.total == 1u || (count.total & 0xFFFu) == 0u || (!match && count.mismatches <= 4u))
            std::fprintf(stderr, "[lcs-rsl] setup %04x: %llu compared, %llu differ\n", flag,
                         static_cast<unsigned long long>(count.total),
                         static_cast<unsigned long long>(count.mismatches));
    }

    bool emitLcsFlushSetup(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t flag)
    {
        if (!g_lcsFlushOriginal || g_verify)
            return false;

        const bool setup800 = flag == 0x0800u;
        const uint32_t at = READ32(kLcsPacketCursor);
        const uint32_t bytes = setup800 ? 224u : 144u;
        if (!lcsFlushPacketFits(at, bytes) || (at < 0x0043DB50u && at + bytes > 0x0043D9F0u))
            return false;

        if (setup800)
        {
            const auto packet = lcsFlushSetupPacket<0x0800u>(rdram);
            writeLcsInlineState(rdram, at, packet.data(), bytes);
            SET_GPR_S32(ctx, 2, at + 8u);
            SET_GPR_VEC(ctx, 3, READ128(0x0043DA40u));
            SET_GPR_U64(ctx, 3, packet[15]);
            SET_GPR_VEC(ctx, 4, READ128(0x0043DA60u));
            SET_GPR_S32(ctx, 4, 0x0043DB50u);
            SET_GPR_VEC(ctx, 5, READ128(0x0043DA50u));
            SET_GPR_S32(ctx, 5, at + 224u);
            SET_GPR_VEC(ctx, 6, READ128(0x0043DB30u));
            SET_GPR_S32(ctx, 7, at + 224u);
            SET_GPR_VEC(ctx, 8, READ128(0x0043DB40u));
            SET_GPR_S32(ctx, 9, 0x0043DB50u);
            SET_GPR_S32(ctx, 10, packet[14]);
            SET_GPR_S32(ctx, 11, at + 96u);
            SET_GPR_S32(ctx, 12, at + 104u);
            SET_GPR_S32(ctx, 13, at + 112u);
            SET_GPR_S32(ctx, 14, at + 108u);
            ctx->branch_pc = 0x2A0A80u;
        }
        else
        {
            const auto packet = lcsFlushSetupPacket<0x0400u>(rdram);
            writeLcsInlineState(rdram, at, packet.data(), bytes);
            SET_GPR_VEC(ctx, 2, READ128(0x0043DA20u));
            SET_GPR_S32(ctx, 3, packet[16]);
            SET_GPR_S32(ctx, 4, at + 8u);
            SET_GPR_S32(ctx, 5, at + 132u);
            SET_GPR_S32(ctx, 6, kLcsFlushPacketBase);
            SET_GPR_S32(ctx, 7, 0x0043D9F0u);
            SET_GPR_S32(ctx, 8, at + 136u);
            SET_GPR_S32(ctx, 9, at + 140u);
            SET_GPR_S32(ctx, 10, at + 144u);
            SET_GPR_S32(ctx, 11, at + 128u);
            SET_GPR_S32(ctx, 12, at + 112u);
            ctx->branch_pc = 0x2A0A90u;
        }

        WRITE32(kLcsPacketCursor, at + bytes);
        g_setupsEmitted.fetch_add(1, std::memory_order_relaxed);
        static bool reported[2]{};
        if (!reported[setup800])
        {
            reported[setup800] = true;
            std::fprintf(stderr, "[lcs-rsl] native setup %04x emitted\n", flag);
        }
        return true;
    }

    // Called at the tail of the EE flush, after all callback and setup work.
    bool emitLcsFlushSetup200(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_lcsFlushOriginal || g_verify)
            return false;

        const uint32_t gp = GPR_U32(ctx, 28);
        if (!(READ32(gp + 0x188u) & 0x0200u) || READ32(gp + 0x198u))
            return false;

        const uint32_t reference = READ32(gp + 0x1A0u);
        const uint32_t cursor = READ32(kLcsPacketCursor);
        const uint32_t bytes = reference ? 32u : 96u;
        if (!lcsFlushPacketFits(cursor, bytes))
            return false;

        if (reference)
        {
            WRITE64(cursor, (uint64_t(reference) << 32) | 0x30000004ull);
            WRITE64(cursor + 8u, uint64_t(0xD809u) << 47);
            if (!submitLcsState(rdram, cursor + 16u, 0x10000000ull))
                WRITE64(cursor + 16u, 0x10000000ull);
            WRITE64(cursor + 24u, 0x1400003Cull);
            SET_GPR_S32(ctx, 4, cursor + 8u);
            SET_GPR_S32(ctx, 5, cursor + 32u);
            SET_GPR_S32(ctx, 7, kLcsFlushPacketBase);
        }
        else
        {
            constexpr uint32_t source = 0x0043D9B0u;
            if (cursor < source + 64u && source < cursor + bytes)
                return false;
            if (!submitLcsState(rdram, cursor, 0x10000005ull))
                WRITE64(cursor, 0x10000005ull);
            WRITE64(cursor + 8u, (uint64_t(0xD809u) << 47) | 0x01000101ull);
            for (uint32_t offset = 0; offset < 64u; offset += 16u)
            {
                const __m128i data = READ128(source + offset);
                WRITE128(cursor + 16u + offset, data);
                SET_GPR_VEC(ctx, 2, data);
            }
            WRITE64(cursor + 80u, 0x1400003Cull);
            WRITE64(cursor + 88u, 0u);
            SET_GPR_S32(ctx, 4, cursor + 84u);
            SET_GPR_S32(ctx, 5, kLcsFlushPacketBase);
            SET_GPR_S32(ctx, 7, source);
            SET_GPR_S32(ctx, 8, cursor + 88u);
            SET_GPR_S32(ctx, 9, cursor + 48u);
            SET_GPR_S32(ctx, 10, cursor + 92u);
            SET_GPR_S32(ctx, 11, cursor + 96u);
            SET_GPR_S32(ctx, 12, cursor + 64u);
            SET_GPR_S32(ctx, 13, cursor + 80u);
        }
        WRITE32(kLcsPacketCursor, cursor + bytes);
        ctx->branch_pc = reference ? 0x2A0C18u : 0x2A0CF4u;
        return true;
    }

    bool submitLcsDraw(uint8_t *rdram, uint32_t cursor, uint64_t fallback, uint32_t returnPc, bool modelB)
    {
        if (!g_lcsInstalled || !lcsFlushPacketFits(cursor, 16u))
            return false;

        const uint32_t id = static_cast<uint32_t>((fallback >> 28) & 7u);
        const uint32_t addr = static_cast<uint32_t>(fallback >> 32);
        const uint32_t qwc = static_cast<uint32_t>(fallback & 0xFFFFu);
        const uint32_t low = static_cast<uint32_t>(fallback);
        if (!addr || (addr & 15u) || addr > kRamMask ||
            (id == 3u ? !qwc || (low & 0xFFFF0000u) != 0x30000000u : low != 0x50000000u))
            return false;

        const Kind kind = id == 3u ? World : modelB ? ModelB :
            returnPc == kLcsSectorCacheDrawReturn ? OtherCall : Model;
        Draw draw{id, addr, qwc, kind};
        uint32_t units = 0;
        if (g_skipVerify || !skippable(blockKey(kind, addr, qwc), kind, addr, units))
        {
            // Record even when an emitter resumes after a guest checkpoint.
            // The generated caller still writes the canonical REF/CALL.
            g_lcsPending.insert_or_assign(cursor, draw);
            return false;
        }

        // The kick folds writes and checks the snapshot again. If it changed,
        // the walker can recover the equivalent REF/CALL directly from the slot.
        const uint64_t slot[2] = {ps2chain::kLcsDrawSlot, fallback};
        std::memcpy(rdram + cursor, slot, sizeof(slot));
        draw.producerSlot = true;
        g_lcsPending.insert_or_assign(cursor, draw);
        ++g_lcsProducerSlots;
        return true;
    }

    bool submitLcsState(uint8_t *rdram, uint32_t cursor, uint64_t cnt)
    {
        const uint32_t bytes = 16u + static_cast<uint32_t>(cnt & 0xFFFFu) * 16u;
        if (!g_lcsInstalled || g_verify || g_skipVerify || (cnt & ~0xFFFFull) != 0x10000000ull ||
            !lcsFlushPacketFits(cursor, bytes))
            return false;
        const uint64_t slot = cnt | (uint64_t(ps2chain::kLcsStateSlotTarget) << 32);
        std::memcpy(rdram + cursor, &slot, sizeof(slot));
        return true;
    }

    bool submitLcsUpload(uint8_t *rdram, uint32_t cursor, uint64_t header, uint64_t pixels)
    {
        if (!g_lcsInstalled || g_verify || g_skipVerify || !lcsFlushPacketFits(cursor, 32u))
            return false;
        uint64_t headerTag = ps2chain::kLcsUploadHeaderSlot;
        uint64_t pixelTag = ps2chain::kLcsUploadPixelsSlot;
        uint64_t headerCodes = header;
        uint64_t pixelCodes = pixels;
        if (!ps2chain::decodeLcsSubmissionSlot(headerTag, headerCodes) ||
            !ps2chain::decodeLcsSubmissionSlot(pixelTag, pixelCodes))
            return false;

        // Payloads are resolved at the kick, just as the original REFs are.
        // The two slots keep the transfer setup before the pixel/CLUT data.
        const uint64_t slots[4] = {ps2chain::kLcsUploadHeaderSlot, header, ps2chain::kLcsUploadPixelsSlot, pixels};
        std::memcpy(rdram + cursor, slots, sizeof(slots));
        return true;
    }

    void installLcs(PS2Runtime &runtime)
    {
        const char *mode = std::getenv("LCS_RSL");
        if (!mode || (std::strcmp(mode, "verify") != 0 && std::strcmp(mode, "replay") != 0) ||
            off("PS2X_HLE") || ps2hle::surveyEnabled())
            return;

        g_verify = std::strcmp(mode, "verify") == 0 || std::getenv("PS2X_HLE_VERIFY");
        g_log = g_verify || lcs_diag::wanted("LCS_RSL_WORLD_LOG"); // release: only with LCS_RSL_WORLD_LOG=1
        g_rdram = runtime.memory().getRDRAM();
        g_rasters = true;
        g_skip = !g_verify && ps2writewatch::enabled() && !off("LCS_RSL_SKIP");
        g_skipVerify = g_skip && std::getenv("LCS_RSL_SKIP_VERIFY") != nullptr;
        constexpr uint32_t submissionEntries[] = {
            0x0029FBC0u, 0x0029FAB8u, 0x0029FD40u, 0x0029F708u, 0x0029F8E0u, 0x00321CB0u,
        };
        for (const uint32_t address : submissionEntries)
            if (!runtime.lookupFunction(address))
                throw std::runtime_error("LCS RSL submission entry missing");

        if (!hook(runtime, 0x002A17A8u, g_lcsFlushAOriginal, &lcsFlushA))
            throw std::runtime_error("LCS RSL FLUSHA hook failed");

        if (!hook(runtime, 0x0023E5B8u, g_lcsConstantsOriginal, &lcsConstants))
            throw std::runtime_error("LCS RSL VU constants hook failed");

        if (!hook(runtime, 0x002A02F0u, g_lcsFlushOriginal, &lcsRenderFlush))
            throw std::runtime_error("LCS RSL flush hook failed");

        if (!g_verify)
        {
            constexpr uint32_t resumes[] = {
                0x2A0378u, 0x2A038Cu, kLcsMatrixReturn, 0x2A04D0u, 0x2A04E8u, 0x2A0530u, 0x2A0548u,
            };
            for (const uint32_t address : resumes)
                if (!runtime.replaceFunction(address, &lcsFlushMaterial))
                    throw std::runtime_error("LCS RSL flush continuation hook failed");
        }

        ps2chain::addTagObserver(&observeLcs);
        ps2chain::addChainObservers(&queued, &processing);
        ps2chain::setMarkHandler(&onMark);
        ps2chain::setDirectHandler(&onDirect);
        g_lcsInstalled = true;
        std::fprintf(stderr, "[lcs-rsl] draw, state and texture submission active (%s)\n",
                     g_verify ? "verify" : "replay");
    }

    void install(PS2Runtime &runtime)
    {
        // needs the native world handler (not the VU1 path, not its checker, not the survey)
        if (off("PS2X_HLE") || std::getenv("PS2X_HLE_VERIFY") || ps2hle::surveyEnabled())
            return;
        g_world = !off("LCS_RSL_WORLD") && hook(runtime, kWorldDraw, g_worldOriginal, &worldHook);
        g_models = !off("LCS_RSL_MODELS") && hook(runtime, kModelDraw, g_modelOriginal, &modelHook) &&
                   hook(runtime, kModelDrawB, g_modelBOriginal, &modelBHook);
        g_rasters = !off("LCS_RSL_RASTERS"); // textures named by raster (docs/FULLY_NATIVE.md step 3d)
        if (!g_world && !g_models && !g_rasters)
            return;
        g_verify = std::getenv("LCS_RSL_WORLD_VERIFY") != nullptr;
        // step 4: blocks known unchanged replayed without being copied (needs the write watch; never while verifying)
        g_skip = !off("LCS_RSL_SKIP") && !g_verify && ps2writewatch::enabled();
        g_skipVerify = g_skip && std::getenv("LCS_RSL_SKIP_VERIFY") != nullptr; // decide, copy anyway, compare
        // step 4c: world / model draws as items (needs the native flush; not with the RSL debug tools, which read the draws'
        // packets)
        g_rdram = runtime.memory().getRDRAM();
        g_items = (g_world || g_models) && lcs::rslnative::enabled() && !off("LCS_RSL_ITEMS") && !std::getenv("LCS_RSL_SHADOW") &&
                  !std::getenv("LCS_TRACE_RSL");
        if (g_items && g_world && !hook(runtime, kSectorDraw, g_sectorOriginal, &sectorHook))
            g_items = false; // world items need to know when a sector is cached
        // step 5a: the state setters as ops (LCS_RSL_OPS=0: their packets)
        if (g_items && !off("LCS_RSL_OPS") && hook(runtime, kTextureBind, g_textureBindOriginal, &textureBindHook))
        {
            g_ops = true;
            hook(runtime, kGsSendA, g_gsSendOriginal[0], &gsSendHook<0>);
            hook(runtime, kGsSendB, g_gsSendOriginal[1], &gsSendHook<1>);
            hook(runtime, kTexMatrix, g_texMatrixOriginal, &texMatrixHook);
            hook(runtime, kVuConstants, g_vuConstantsOriginal, &vuConstantsHook);
            hook(runtime, kFlushA, g_flushAOriginal, &flushAHook);
            if (!off("LCS_RSL_UPLOADS")) // step 5b: texture uploads (LCS_RSL_UPLOADS=0: their REFs)
                hook(runtime, kUpload, g_uploadOriginal, &uploadHook);
            if (!off("LCS_RSL_IM2D")) // steps 5c / 5d: captured emitters (LCS_RSL_IM2D=0: their packets as written)
            {
                hookCaptured(runtime, std::make_integer_sequence<int, kCapturedCount>{});
                // step 5h: gaps need the binds captured (else their half-filled upload slots would be read)
                const unsigned long mask = std::getenv("LCS_RSL_CAPTURE_MASK") ? std::strtoul(std::getenv("LCS_RSL_CAPTURE_MASK"), nullptr, 0) : ~0ul;
                g_gaps = !off("LCS_RSL_GAPS") && ((mask >> (kCapturedCount - 2)) & 3ul) == 3ul;
            }
        }
        g_itemVerify = g_items && std::getenv("LCS_RSL_ITEMS_VERIFY") != nullptr;
        g_log = g_verify || g_itemVerify || std::getenv("LCS_RSL_WORLD_LOG") != nullptr;
        ps2chain::addTagObserver(&observe);
        ps2chain::addChainObservers(&queued, &processing);
        ps2chain::setMarkHandler(&onMark);
        if (g_rasters)
            ps2chain::setDirectHandler(&onDirect);
    }

    void settlePending(uint8_t *rdram, uint32_t sp, uint32_t ra, uint32_t fn) { settleIm2D(rdram, sp, ra, fn); }

    OverlayStats overlayStats()
    {
        std::lock_guard<std::mutex> lock(g_overlayMutex);
        OverlayStats o = g_overlay;
        o.setupsCompared = g_setupsCompared.load(std::memory_order_relaxed); // live
        o.setupsDiffer = g_setupsDiffer.load(std::memory_order_relaxed);
        o.setupsEmitted = g_setupsEmitted.load(std::memory_order_relaxed);
        o.active = g_lcsInstalled;
        o.replay = !g_verify;
        return o;
    }

    bool flushAsOp(uint8_t *rdram, const uint8_t *tags, uint32_t bytes)
    {
        if (!g_ops || !opsAllowed())
        {
            if (g_ops)
                ++g_opStats.caching;
            return false;
        }
        thread_local std::vector<uint8_t> vif;
        vif.clear();
        tagsToVif(tags, bytes, vif);
        return vif.empty() || appendOp(rdram, vif.data(), static_cast<uint32_t>(vif.size()));
    }

    void onFrameKick()
    {
        if (!g_world && !g_models && !g_rasters)
            return;
        settleIm2D(const_cast<uint8_t *>(g_rdram), ~0u, 0u, 0u); // steps 5c / 5d: before the chain goes out (and before the
        g_inCapture = false;                                      // draw list is handed over: a region's draws leave it)
        g_kicked.swap(g_current);
        g_current.clear();
        g_currentAtSettle = 0;
        g_next = 0;
        if (g_itemVerify)
        {
            checkItem(g_rdram);
            checkIm2D(g_rdram);
        }
        g_itemsKicked.swap(g_itemsCurrent); // step 4c (a frame whose chain was not observed loses its items with it)
        g_itemFrameKicked = g_itemFrame++;
        g_caching = false;
        g_materialCaching = false;
        g_materialCacheEnds = false;
        g_openItem = -1;
        g_itemsCurrent.clear();
        g_arenaKicked.swap(g_arenaCurrent);
        g_arenaCurrent.clear();
        g_refsKicked.swap(g_refsCurrent);
        g_refsCurrent.clear();
        takePooled(g_arenaPool, g_arenaCurrent);
        takePooled(g_itemPool, g_itemsCurrent);
    }
}
