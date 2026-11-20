#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string_view>

namespace ps2x::iop::cdvd_debug
{
    enum class Status { None, Pending, Complete, Failed };

    struct Read
    {
        Status status = Status::None;
        bool discImage = false;
        uint32_t lsn = 0, sectors = 0, destination = 0, error = 0;
        std::array<char, 96> path{};
    };

    struct Snapshot { Read pending, last; };

    namespace detail
    {
        struct WatchedFile { uint32_t first, end; const char* name; };
        // Sector ranges verified against the project's SLUS-214.23 ISO and CUTS.DIR.
        inline constexpr WatchedFile watched[] = {
            {1062056u, 1062714u, "MAIN.SCM"},
            {1870312u, 1870793u, "intro.ifp"},
            {1870793u, 1870794u, "intro.cut"},
            {1870794u, 1870797u, "intro.dat"},
            {1901456u, 1902528u, "AUDIO/CUTSCENE/INTRO.VB"},
        };

        struct Extent
        {
            uint32_t lsn = 0, size = 0;
            std::array<char, 96> path{};
        };

        inline std::mutex mutex;
        inline Snapshot state;
        inline std::array<Extent, 256> extents;
        inline size_t nextExtent = 0;
    }

    inline std::atomic<bool> enabled{false};

    inline bool introTraceEnabled()
    {
        static const bool active = [] {
            const char* setting = std::getenv("LCS_TRACE_INTRO_IO");
            return setting && std::string_view(setting) == "1";
        }();
        return active;
    }

    inline void traceIntroRead(const char* path, const char* state, uint32_t lsn, uint64_t bytes,
                               uint32_t destination = 0u)
    {
        if (!introTraceEnabled() || !bytes) return;
        const uint64_t start = uint64_t(lsn) * 2048u;
        const uint64_t end = start + bytes;
        for (const auto& file : detail::watched) {
            const uint64_t overlapStart = std::max(start, uint64_t(file.first) * 2048u);
            const uint64_t overlapEnd = std::min(end, uint64_t(file.end) * 2048u);
            if (overlapStart < overlapEnd)
                std::fprintf(stderr, "[LCS intro io] file=%s path=%s status=%s lsn=%u bytes=%llu file_offset=%llu overlap=%llu dst=0x%08X\n",
                    file.name, path, state, lsn, static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(overlapStart - uint64_t(file.first) * 2048u),
                    static_cast<unsigned long long>(overlapEnd - overlapStart), destination);
        }
    }

    inline void reset()
    {
        if (!enabled.load(std::memory_order_relaxed) && !introTraceEnabled()) return;
        std::lock_guard lock(detail::mutex);
        detail::state = {};
        detail::extents = {};
        detail::nextExtent = 0;
    }

    inline void remember(std::string_view path, uint32_t lsn, uint32_t size)
    {
        if (!enabled.load(std::memory_order_relaxed) || size == 0u) return;
        std::lock_guard lock(detail::mutex);
        auto it = std::find_if(detail::extents.begin(), detail::extents.end(),
            [lsn, size](const auto& entry) { return entry.lsn == lsn && entry.size == size; });
        if (it != detail::extents.end()) return;
        auto& entry = detail::extents[detail::nextExtent++ % detail::extents.size()];
        entry = {lsn, size, {}};
        std::copy_n(path.data(), std::min(path.size(), entry.path.size() - 1), entry.path.data());
    }

    inline void begin(bool discImage, uint32_t lsn, uint32_t sectors, uint32_t destination)
    {
        if ((!enabled.load(std::memory_order_relaxed) && !introTraceEnabled()) || sectors == 0u) return;
        std::lock_guard lock(detail::mutex);
        Read& read = detail::state.pending;
        read = {Status::Pending, discImage, lsn, sectors, destination};
        const auto match = std::find_if(detail::extents.begin(), detail::extents.end(),
            [lsn, sectors](const auto& entry) {
                const uint64_t end = uint64_t(entry.lsn) + (uint64_t(entry.size) + 2047u) / 2048u;
                return entry.size && lsn >= entry.lsn && uint64_t(lsn) + sectors <= end;
            });
        if (match != detail::extents.end()) read.path = match->path;
        if (discImage) traceIntroRead("IOP-CDVD", "requested", lsn, uint64_t(sectors) * 2048u, destination);
    }

    inline void finish(uint32_t error = 0)
    {
        if (!enabled.load(std::memory_order_relaxed) && !introTraceEnabled()) return;
        std::lock_guard lock(detail::mutex);
        auto& read = detail::state.pending;
        if (read.status != Status::Pending) return;
        read.status = error ? Status::Failed : Status::Complete;
        read.error = error;
        if (read.discImage)
            traceIntroRead("IOP-CDVD", error ? "failed" : "completed", read.lsn,
                uint64_t(read.sectors) * 2048u, read.destination);
        detail::state.last = read;
        read = {};
    }

    inline Snapshot snapshot()
    {
        std::lock_guard lock(detail::mutex);
        return detail::state;
    }

    inline void traceIntroDma(uint32_t source, uint32_t destination, const uint8_t* bytes, uint32_t size)
    {
        if (!introTraceEnabled() || !size) return;
        const auto reads = snapshot();
        const uint32_t sourcePhysical = source & 0x1FFFFFFFu;
        const auto overlaps = [&](const Read& read) {
            const uint32_t base = read.destination & 0x1FFFFFFFu;
            return read.discImage && uint64_t(sourcePhysical) + size > base &&
                   uint64_t(base) + uint64_t(read.sectors) * 2048u > sourcePhysical;
        };
        const Read& read = overlaps(reads.pending) ? reads.pending : reads.last;
        if (read.status == Status::Failed || !overlaps(read)) return;

        const uint64_t base = read.destination & 0x1FFFFFFFu;
        const uint64_t overlapStart = std::max<uint64_t>(sourcePhysical, base);
        const uint64_t overlapEnd = std::min(uint64_t(sourcePhysical) + size, base + uint64_t(read.sectors) * 2048u);
        const uint64_t discStart = uint64_t(read.lsn) * 2048u + overlapStart - base;
        const uint64_t discEnd = discStart + overlapEnd - overlapStart;
        for (const auto& file : detail::watched)
        {
            const uint64_t fileStart = uint64_t(file.first) * 2048u;
            const uint64_t start = std::max(discStart, fileStart);
            const uint64_t end = std::min(discEnd, uint64_t(file.end) * 2048u);
            if (start >= end) continue;

            const size_t offset = static_cast<size_t>(overlapStart - sourcePhysical + start - discStart);
            const size_t count = static_cast<size_t>(end - start);
            uint64_t hash = 14695981039346656037ull;
            for (size_t i = 0; i < count; ++i)
                hash = (hash ^ bytes[offset + i]) * 1099511628211ull;
            std::fprintf(stderr, "[LCS intro dma] file=%s file_offset=%llu bytes=%zu iop=0x%08X ee=0x%08X fnv1a=%016llx\n",
                file.name, static_cast<unsigned long long>(start - fileStart), count,
                source + static_cast<uint32_t>(offset), destination + static_cast<uint32_t>(offset),
                static_cast<unsigned long long>(hash));
        }
    }
}
