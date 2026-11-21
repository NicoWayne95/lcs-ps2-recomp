// [lcs-stream] LCS disc streaming without the IOP's stream server - the port of the the reference port project's src/game/ref_stream.cpp
// (its docs/NATIVE_PORT_ROADMAP.md milestone 6 step 1, docs/game-internals.md 9).
//
// LCS reads everything after boot through CDSTREAM.IRX's stream server (SIF RPC 0x65686577, EE client 0x428040; bound
// with fn 0 by 0x1F6A08), from these EE functions (generated/ee, all end in 0x1F6AD8(the reply's error word)):
//   0x1F6CA0(channel, EE dest, LSN, size)  fn 1: start a read - size in sectors, or -bytes; returns 1 (accepted)
//   0x1F6D78(channel)                      fn 2: 0 when the channel's read is done, 0xFA while busy
//   0x1F6EA0(channel)                      fn 3: (streaming code 0x18F648 / 0x18F918) - left to the IOP, after this
//                                          channel's native reads are done (so no buffer is let go while one lands)
//   0x1F6DE8()                             fn 4: the drive's position (an LSN: 0x18F108 picks the next file to stream
//                                          after it) - here the end of the last read
//   0x1F6E50()                             fn 5: error state (0x1F6C78) - left to the IOP
// Through the emulated IOP the game streamed into blocks that were still in use or relocated files half-written
// (run_175135: a model block given out twice, the frame's CALLs into it ran into IOP data; run_175701: a texture
// dictionary with entries left unrelocated - the frame list at 0x2BB338 looped). the reference port had the same server replaced:
// reads are queued for a host thread that copies from the disc image into EE RAM; a poll says busy until the copy is
// done. The game never touches a buffer before its poll says done.
//
// LCS_NATIVE_STREAM=0: through the IOP as before. LCS_STREAM_LOG=1: reads per 5 s.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_disc_image.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include "runtime/ps2_host_std.h"

void ps2xRecordGuestWrite(uint32_t address, uint32_t size, const uint8_t *data); // [lcs-write] ps2_iop_host.cpp

#include "lcs/lcs_diag.h"

namespace lcs_stream
{
    // [lcs] the first reads / polls / positions: release only with LCS_STREAM_LOG=1 (errors are always logged)
    static const bool streamLog = lcs_diag::wanted("LCS_STREAM_LOG");

    namespace
    {
        constexpr uint32_t kRead = 0x001F6CA0u;
        constexpr uint32_t kPoll = 0x001F6D78u;
        constexpr uint32_t kFn3 = 0x001F6EA0u;
        constexpr uint32_t kPosition = 0x001F6DE8u;
        constexpr uint32_t kFn5 = 0x001F6E50u;
        constexpr uint32_t kReply = 0x004280C0u;      // the client's receive buffer: result, error word
        constexpr uint32_t kErrorShown = 0x003D7BECu; // 0x1F6AD8(0) clears these two (no disc error)
        constexpr uint32_t kErrorGp = 0xD00u;
        constexpr uint32_t kBusy = 0xFAu;
        constexpr uint32_t kChannels = 16u;

        struct Request
        {
            uint32_t channel, dest, lsn, bytes;
        };

        struct Reader
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::condition_variable doneCv;
            std::deque<Request> queue;
            std::atomic<uint32_t> pending[kChannels]{};
            // a channel is taken from an accepted read until a poll / fn 3 has seen it done (CDSTREAM's busy flag: the
            // synchronous loader 0x18F794 submits until a read is *not* accepted - "busy" - and then waits with fn 3;
            // the IOP's reads took long enough for the second submit to find the channel busy, the host's do not)
            bool taken[kChannels]{};
            std::chrono::steady_clock::time_point takenAt[kChannels]{};
            std::atomic<uint32_t> position{0}; // LSN after the last read queued
            uint8_t *rdram = nullptr;
            bool started = false;
            std::atomic<uint64_t> reads{0}, bytes{0}, nanoseconds{0}, failed{0}, fn3Waits{0};
        };

        std::atomic<uint64_t> g_iopCalls{0}, g_iopNs{0}, g_iopMaxNs{0}, g_iopSlow{0}; // noteIopCall

        Reader &reader()
        {
            static Reader *r = new Reader(); // never destroyed: the thread may still wait on it at exit
            return *r;
        }

        // LCS mounts no ps2_disc image (the reference port does): the IOP's cdvdman reads the ISO named by the runtime's I/O paths
        // (src/main.cpp: config/local_cd_image.txt, LCS_ISO, else the default). Read the same file; the reader thread only.
        std::FILE *g_iso = nullptr;
        bool openDisc()
        {
            if (g_iso || ps2_disc::image())
                return true;
            const std::filesystem::path path = PS2Runtime::getIoPaths().cdImage;
            if (path.empty())
                return false;
#if defined(_WIN32)
            g_iso = _wfopen(path.wstring().c_str(), L"rb");
#else
            g_iso = std::fopen(path.string().c_str(), "rb");
#endif
            return g_iso != nullptr;
        }
        bool readDisc(uint64_t offset, uint32_t bytes, uint8_t *dst)
        {
            if (IPS2DiscImage *disc = ps2_disc::image())
                return disc->readBytes(offset, bytes, dst);
            if (!g_iso)
                return false;
#if defined(_WIN32)
            if (_fseeki64(g_iso, static_cast<int64_t>(offset), SEEK_SET) != 0)
#else
            if (fseeko(g_iso, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
                return false;
            const size_t n = std::fread(dst, 1u, bytes, g_iso);
            if (n < bytes) // past the image's end: zeros, as IsoImage::readBytes
                std::memset(dst + n, 0, bytes - n);
            return n == bytes;
        }

        // [lcs-model] the last reads done (destination, LSN, bytes): readFor() names the disc bytes a block was read from
        constexpr uint32_t kDone = 4096u;
        Request g_done[kDone]{};
        uint64_t g_doneCount = 0;
        std::mutex g_doneMutex;

        void run()
        {
            Reader &r = reader();
            if (!openDisc())
                std::fprintf(stderr, "[lcs-stream] the disc image %s does not open: every read fails\n",
                             PS2Runtime::getIoPaths().cdImage.string().c_str());
            for (;;)
            {
                Request request;
                {
                    std::unique_lock<std::mutex> lock(r.mutex);
                    r.cv.wait(lock, [&] { return !r.queue.empty(); });
                    request = r.queue.front();
                    r.queue.pop_front();
                }
                const auto start = ps2x::hostNow();
                const uint32_t dest = request.dest & PS2_RAM_MASK;
                const uint32_t bytes = std::min<uint32_t>(request.bytes, PS2_RAM_SIZE - dest);
                if (!readDisc(static_cast<uint64_t>(request.lsn) * 2048u, bytes, r.rdram + dest))
                {
                    r.failed.fetch_add(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[lcs-stream] read of %u bytes at LSN 0x%x to %08x failed\n", bytes, request.lsn, dest);
                }
                else if (bytes)
                {
                    ps2xRecordGuestWrite(dest, bytes, r.rdram + dest); // the [lcs-write] ring, as for the IOP's writes
                    std::lock_guard<std::mutex> lock(g_doneMutex); // [lcs-model] which disc sectors landed where
                    g_done[g_doneCount++ % kDone] = {request.channel, dest, request.lsn, bytes};
                }
                r.reads.fetch_add(1, std::memory_order_relaxed);
                r.bytes.fetch_add(bytes, std::memory_order_relaxed);
                r.nanoseconds.fetch_add(static_cast<uint64_t>(
                                            std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow() - start).count()),
                                        std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(r.mutex);
                    r.pending[request.channel % kChannels].fetch_sub(1, std::memory_order_release); // the data before "done"
                }
                r.doneCv.notify_all();
            }
        }

        void report()
        {
            static const bool log = std::getenv("LCS_STREAM_LOG") != nullptr;
            static auto last = ps2x::hostNow();
            if (!log || ps2x::hostNow() - last < std::chrono::seconds(5))
                return;
            last = ps2x::hostNow();
            Reader &r = reader();
            std::fprintf(stderr, "[lcs-stream] %llu reads, %.1f MB, %.1f ms reading, %llu failed, %llu fn 3 waits (last 5 s)\n",
                         static_cast<unsigned long long>(r.reads.exchange(0)), r.bytes.exchange(0) / 1048576.0,
                         r.nanoseconds.exchange(0) / 1e6, static_cast<unsigned long long>(r.failed.exchange(0)),
                         static_cast<unsigned long long>(r.fn3Waits.exchange(0)));
            const uint64_t calls = g_iopCalls.exchange(0);
            std::fprintf(stderr, "[lcs-stream] IOP fn 3 / 5: %llu calls, %.1f ms in all, longest %.1f ms (last 5 s)\n",
                         static_cast<unsigned long long>(calls), g_iopNs.exchange(0) / 1e6, g_iopMaxNs.exchange(0) / 1e6);
        }

        // what 0x1F6AD8 leaves behind for error word 0, and the reply words the game may read back (0x1F6C78)
        void reply(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t result)
        {
            WRITE32(kReply, result);
            WRITE32(kReply + 4u, 0u);
            WRITE32(kErrorShown, 0u);
            WRITE32(GPR_U32(ctx, 28) + kErrorGp, 0u);
            SET_GPR_U64(ctx, 2, result);
            ctx->pc = GPR_U32(ctx, 31);
        }

        void waitChannel(Reader &r, uint32_t channel)
        {
            std::atomic<uint32_t> &pending = r.pending[channel % kChannels];
            if (pending.load(std::memory_order_acquire) == 0u)
                return;
            std::unique_lock<std::mutex> lock(r.mutex);
            r.doneCv.wait(lock, [&] { return pending.load(std::memory_order_acquire) == 0u; });
        }

        // 0x1F6CA0(channel, dest, lsn, size)
        PS2_GUEST_CC void streamRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Reader &r = reader();
            const int32_t size = static_cast<int32_t>(GPR_U32(ctx, 7));
            Request request{GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6),
                            size < 0 ? static_cast<uint32_t>(-size) : static_cast<uint32_t>(size) * 2048u};
            const uint32_t ch = request.channel % kChannels;
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                if (!r.started)
                {
                    r.rdram = rdram;
                    r.started = true;
                    std::thread(run).detach();
                    std::fprintf(stderr, "[lcs-stream] native disc streaming active (LCS_NATIVE_STREAM=0: through the IOP)\n");
                }
                // the same request again on a channel still taken: CDSTREAM's "busy, not accepted" (the loader's
                // submit loop 0x18F794 ends on it). Only for the identical request - any other read is queued (the
                // streamer 0x18F108 takes 0 for a failed read and the async path 0x2E6458 ignores the reply).
                static Request last[kChannels]{};
                const bool same = last[ch].lsn == request.lsn && last[ch].dest == request.dest && last[ch].bytes == request.bytes;
                if (r.taken[ch] && same && ps2x::hostNow() - r.takenAt[ch] < std::chrono::milliseconds(200))
                {
                    static uint32_t rejected = 0;
                    if (++rejected <= 8u)
                        std::fprintf(stderr, "[lcs-stream] read ch %u LSN 0x%x again while taken: not accepted (busy) | ra %08x\n",
                                     request.channel, request.lsn, GPR_U32(ctx, 31));
                    reply(rdram, ctx, runtime, 0u);
                    return;
                }
                last[ch] = request;
                r.taken[ch] = true;
                r.takenAt[ch] = ps2x::hostNow();
                r.pending[ch].fetch_add(1, std::memory_order_relaxed);
                r.queue.push_back(request);
            }
            { // the first reads, and reads repeated (same LSN, destination and size as the one before on that channel)
                static uint32_t logged = 0, repeats = 0;
                static Request last[kChannels]{};
                Request &prev = last[request.channel % kChannels];
                const bool repeat = prev.lsn == request.lsn && prev.dest == request.dest && prev.bytes == request.bytes;
                prev = request;
                repeats += repeat;
                if (streamLog && (logged < 48u || (repeat && (repeats < 16u || (repeats & 1023u) == 0u))))
                {
                    ++logged;
                    std::fprintf(stderr, "[lcs-stream] read ch %u LSN 0x%x %u bytes to %08x | ra %08x%s (%u repeats)\n", request.channel,
                                 request.lsn, request.bytes, request.dest, GPR_U32(ctx, 31), repeat ? " REPEAT" : "", repeats);
                }
            }
            r.position.store(request.lsn + (request.bytes + 2047u) / 2048u, std::memory_order_relaxed);
            r.cv.notify_one();
            report();
            reply(rdram, ctx, runtime, 1u); // accepted
        }

        // 0x1F6D78(channel): a read in flight is waited for up to 20 ms (the reference port: the game polls right after starting most
        // reads and otherwise loses a frame per read to "busy")
        PS2_GUEST_CC void streamPoll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Reader &r = reader();
            std::atomic<uint32_t> &pending = r.pending[GPR_U32(ctx, 4) % kChannels];
            bool busy = pending.load(std::memory_order_acquire) != 0u;
            if (busy)
            {
                std::unique_lock<std::mutex> lock(r.mutex);
                busy = !ps2x::waitFor(r.doneCv, lock, std::chrono::milliseconds(20),
                                      [&] { return pending.load(std::memory_order_acquire) == 0u; });
            }
            if (!busy)
            {
                std::lock_guard<std::mutex> lock(r.mutex);
                r.taken[GPR_U32(ctx, 4) % kChannels] = false; // seen done: free for the next read
            }
            static uint32_t polls = 0;
            if (streamLog && ++polls <= 48u)
                std::fprintf(stderr, "[lcs-stream] poll ch %u -> %s | ra %08x\n", GPR_U32(ctx, 4), busy ? "busy" : "done", GPR_U32(ctx, 31));
            reply(rdram, ctx, runtime, busy ? kBusy : 0u);
        }

        // 0x1F6DE8(): the drive's position
        PS2_GUEST_CC void streamPosition(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static uint32_t asked = 0;
            if (streamLog && ++asked <= 16u)
                std::fprintf(stderr, "[lcs-stream] position -> 0x%x | ra %08x\n", reader().position.load(std::memory_order_relaxed),
                             GPR_U32(ctx, 31));
            reply(rdram, ctx, runtime, reader().position.load(std::memory_order_relaxed));
        }

        // [lcs-stream] host time of the calls still going to the emulated IOP (fn 3 / fn 5): a call that blocks the
        // EE thread is a hitch (run_185917: ~180 ms stalls while the guest moved one frame). Logged above 20 ms, the
        // first 24 and every 64th after; totals with LCS_STREAM_LOG.
        void noteIopCall(uint32_t fn, std::chrono::steady_clock::time_point t0, uint32_t ra, bool returned)
        {
            const uint64_t ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow() - t0).count());
            g_iopCalls.fetch_add(1, std::memory_order_relaxed);
            g_iopNs.fetch_add(ns, std::memory_order_relaxed);
            uint64_t prev = g_iopMaxNs.load(std::memory_order_relaxed);
            while (ns > prev && !g_iopMaxNs.compare_exchange_weak(prev, ns))
            {
            }
            if (ns > 20'000'000ull)
            {
                const uint64_t n = g_iopSlow.fetch_add(1, std::memory_order_relaxed) + 1u;
                if (n <= 24u || (n & 63u) == 0u)
                    std::fprintf(stderr, "[lcs-stream] IOP fn %u took %.1f ms (ra %08x%s) - %llu slow calls\n", fn, ns / 1e6, ra,
                                 returned ? "" : ", left before its return", static_cast<unsigned long long>(n));
            }
        }

        // 0x1F6E50(): fn 5, the error state, through the IOP - timed
        PS2Runtime::RecompiledFunction g_fn5Original = nullptr;
        PS2_GUEST_CC void streamFn5(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kFn5;
            const uint32_t ra = GPR_U32(ctx, 31);
            const auto t0 = ps2x::hostNow();
            g_fn5Original(rdram, ctx, runtime);
            if (entry)
                noteIopCall(5u, t0, ra, ctx->pc == ra);
        }

        // 0x1F6EA0(channel): the channel's state - the loader waits for 0 after its submit loop (0x18F7C0). This
        // channel's native reads finish first; the IOP's fn 3 still runs (0x1F4B58 after it), but the IOP read nothing
        // on the channel, so its answer is replaced by "done" (0).
        PS2Runtime::RecompiledFunction g_fn3Original = nullptr;
        PS2_GUEST_CC void streamFn3(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool entry = ctx->pc == kFn3;
            const uint32_t channel = GPR_U32(ctx, 4) % kChannels, ra = GPR_U32(ctx, 31);
            if (entry)
            {
                Reader &r = reader();
                if (r.pending[channel].load(std::memory_order_acquire) != 0u)
                {
                    r.fn3Waits.fetch_add(1, std::memory_order_relaxed);
                    waitChannel(r, channel);
                }
                std::lock_guard<std::mutex> lock(r.mutex);
                r.taken[channel] = false;
            }
            const auto t0 = ps2x::hostNow();
            g_fn3Original(rdram, ctx, runtime);
            noteIopCall(3u, t0, ra, entry && ctx->pc == ra);
            if (entry && ctx->pc == ra)
            {
                static uint32_t logged = 0;
                if (GPR_U32(ctx, 2) != 0u && ++logged <= 8u)
                    std::fprintf(stderr, "[lcs-stream] fn 3 ch %u: the IOP said %u, the host's read is done: 0\n", channel,
                                 GPR_U32(ctx, 2));
                WRITE32(kReply, 0u);
                SET_GPR_U64(ctx, 2, 0u);
            }
        }
    }

    // [lcs-model] the newest native read whose destination range holds addr: its destination, LSN and size
    bool readFor(uint32_t addr, uint32_t &dest, uint32_t &lsn, uint32_t &bytes)
    {
        addr &= PS2_RAM_MASK;
        std::lock_guard<std::mutex> lock(g_doneMutex);
        const uint64_t first = g_doneCount > kDone ? g_doneCount - kDone : 0u;
        for (uint64_t i = g_doneCount; i-- > first;)
        {
            const Request &d = g_done[i % kDone];
            if (addr >= d.dest && addr < d.dest + d.bytes)
            {
                dest = d.dest;
                lsn = d.lsn;
                bytes = d.bytes;
                return true;
            }
        }
        return false;
    }

    // [lcs-model] the disc's bytes at an LSN, read through a file of its own (the reader thread keeps the other)
    bool readDiscCopy(uint32_t lsn, uint32_t bytes, uint8_t *dst)
    {
        if (IPS2DiscImage *disc = ps2_disc::image())
            return disc->readBytes(static_cast<uint64_t>(lsn) * 2048u, bytes, dst);
        const std::filesystem::path path = PS2Runtime::getIoPaths().cdImage;
#if defined(_WIN32)
        std::FILE *f = _wfopen(path.wstring().c_str(), L"rb");
#else
        std::FILE *f = std::fopen(path.string().c_str(), "rb");
#endif
        if (!f)
            return false;
#if defined(_WIN32)
        const bool sought = _fseeki64(f, static_cast<int64_t>(lsn) * 2048, SEEK_SET) == 0;
#else
        const bool sought = fseeko(f, static_cast<off_t>(lsn) * 2048, SEEK_SET) == 0;
#endif
        const size_t n = sought ? std::fread(dst, 1u, bytes, f) : 0u;
        std::fclose(f);
        return n == bytes;
    }

    bool install(PS2Runtime &runtime)
    {
        const char *v = std::getenv("LCS_NATIVE_STREAM");
        if (v && *v == '0')
        {
            std::fprintf(stderr, "[lcs-stream] through the IOP (LCS_NATIVE_STREAM=0)\n");
            return false;
        }
        // the I/O paths may be set after this (src/main.cpp): then the image is opened at the first read
        if (!PS2Runtime::getIoPaths().cdImage.empty() && !openDisc())
        {
            std::fprintf(stderr, "[lcs-stream] the disc image does not open: through the IOP\n");
            return false;
        }
        g_fn3Original = runtime.lookupFunction(kFn3);
        if (!g_fn3Original || !runtime.lookupFunction(kRead) || !runtime.lookupFunction(kPoll) ||
            !runtime.lookupFunction(kPosition))
        {
            std::fprintf(stderr, "[lcs-stream] stream client functions missing: through the IOP\n");
            return false;
        }
        runtime.replaceFunction(kRead, &streamRead);
        runtime.replaceFunction(kPoll, &streamPoll);
        runtime.replaceFunction(kPosition, &streamPosition);
        runtime.replaceFunction(kFn3, &streamFn3);
        g_fn5Original = runtime.lookupFunction(kFn5);
        if (g_fn5Original)
            runtime.replaceFunction(kFn5, &streamFn5);
        std::fprintf(stderr, "[lcs-stream] stream reads / polls / position on the host (fn 0, 3, 5 through the IOP)\n");
        return true;
    }
}
