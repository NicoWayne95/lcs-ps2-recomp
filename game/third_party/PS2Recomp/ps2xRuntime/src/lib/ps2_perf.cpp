// [lcs-recomp] Performance instrumentation: see runtime/ps2_perf.h.
#include "runtime/ps2_perf.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"

#include "platform/platform.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "runtime/ps2_host_std.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ps2perf
{
#if defined(_WIN32)
    namespace
    {
        std::atomic<DWORD> g_hostThreadIds[static_cast<size_t>(HostThread::Count)]{};
    }
#endif

    void registerHostThread(HostThread thread)
    {
#if defined(_WIN32)
        g_hostThreadIds[static_cast<size_t>(thread)].store(GetCurrentThreadId(), std::memory_order_relaxed);
#else
        (void)thread;
#endif
    }

    bool hostThreadCpuTime(HostThread thread, uint64_t &time)
    {
#if defined(_WIN32)
        const DWORD id = g_hostThreadIds[static_cast<size_t>(thread)].load(std::memory_order_relaxed);
        if (!id)
            return false;
        HANDLE handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!handle)
            return false;
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool ok = GetThreadTimes(handle, &created, &exited, &kernel, &user) != 0;
        CloseHandle(handle);
        if (ok)
            time = (uint64_t{kernel.dwHighDateTime} << 32) + kernel.dwLowDateTime +
                   (uint64_t{user.dwHighDateTime} << 32) + user.dwLowDateTime;
        return ok;
#else
        (void)thread;
        (void)time;
        return false;
#endif
    }

    int hostThreadStack(HostThread thread, uint64_t *offsets, int max)
    {
#if defined(_WIN32) && defined(_M_X64) || defined(_WIN32) && defined(__x86_64__)
        const DWORD id = g_hostThreadIds[static_cast<size_t>(thread)].load(std::memory_order_relaxed);
        if (!id || max <= 0 || id == GetCurrentThreadId())
            return 0;
        HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
        if (!handle)
            return 0;
        int n = 0;
        uint64_t pcs[64];
        if (SuspendThread(handle) != static_cast<DWORD>(-1))
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(handle, &ctx))
            {
                // only unwinding here (no allocation, no locks the suspended thread may hold)
                while (n < max && n < 64 && ctx.Rip)
                {
                    pcs[n++] = ctx.Rip;
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                    if (!fn)
                    {
                        ctx.Rip = *reinterpret_cast<DWORD64 *>(ctx.Rsp); // a leaf: the return address is at RSP
                        ctx.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                }
            }
            ResumeThread(handle);
        }
        CloseHandle(handle);
        const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
        for (int i = 0; i < n; ++i)
            offsets[i] = pcs[i] - base;
        return n;
#else
        (void)thread;
        (void)offsets;
        (void)max;
        return 0;
#endif
    }

    namespace
    {
        constexpr size_t kPhases = static_cast<size_t>(Phase::Count);
        constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
        constexpr size_t kHistory = 600;     // samples kept for the overlay
        constexpr size_t kGraphSamples = 240; // samples shown in the graph
        constexpr const char *kPhaseNames[kPhases] = {"game", "render", "iop", "wait"};
        constexpr const char *kCounterNames[kCounters] = {"draws", "verts", "tex_uploads", "dma_tags", "vu1_programs",
                                                          "gs_draws", "gs_verts", "gs_vu1", "gs_hle", "gs_path2", "gs_path3",
                                                          "native_draws", "hud_draws", "post_draws", "packed_draws",
                                                          "packed_verts", "tex_create_us", "tex_kb", "tex_decode_us"};
        constexpr size_t kFirstSourceCounter = static_cast<size_t>(Counter::GsDraws); // [lcs-recomp] summaryLine

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow().time_since_epoch()).count());
        }

        struct Sample
        {
            double t = 0.0;          // seconds since the first sample
            float frameMs = 0.0f;    // wall time of this game frame
            float phaseMs[kPhases]{};
            uint32_t vblanks = 0;
            uint32_t counters[kCounters]{};
            float hostFps = 0.0f, hostMs = 0.0f, glMs = 0.0f; // host frames presented during this game frame
            float hostMaxMs = 0.0f; // [lcs-recomp] the longest host frame among them (a presentation stall)
            float audioMs = 0.0f;
            uint32_t underruns = 0;
            float workerMs = 0.0f; // [lcs-recomp] render submission worker busy
            float gsMs = 0.0f;     // [lcs-recomp] its GS stage busy
        };

        std::atomic<uint64_t> g_phaseNs[kPhases];
        std::atomic<uint64_t> g_workerNs{0};
        std::atomic<uint64_t> g_gsStageNs{0}; // [lcs-recomp]
        std::atomic<uint64_t> g_counters[kCounters];
        std::atomic<uint64_t> g_hostFrames{0}, g_hostNs{0}, g_glNs{0};
        std::atomic<uint64_t> g_hostMaxNs{0}; // [lcs-recomp]
        thread_local PhaseScope *t_current = nullptr;

        // EE thread
        uint64_t g_firstMarkNs = 0, g_lastMarkNs = 0, g_lastGameMarkNs = 0;
        uint32_t g_vblanks = 0;
        uint64_t g_lastUnderruns = 0;

        // shared (g_mutex)
        std::mutex g_mutex;
        std::vector<Sample> g_ring(kHistory);
        size_t g_head = 0, g_size = 0;
        FILE *g_csv = nullptr;
        std::string g_csvName;
        bool g_logFromEnv = false;
        double g_lastSummaryT = 0.0;

        // main thread
        uint64_t g_lastHostNs = 0;

        void openCsv(const std::string &name)
        {
            g_csv = std::fopen(name.c_str(), "w");
            if (!g_csv)
            {
                std::fprintf(stderr, "[perf] cannot write %s\n", name.c_str());
                return;
            }
            g_csvName = name;
            std::fprintf(g_csv, "t_s,frame_ms,vblanks");
            for (const char *p : kPhaseNames)
                std::fprintf(g_csv, ",%s_ms", p);
            for (const char *c : kCounterNames)
                std::fprintf(g_csv, ",%s", c);
            std::fprintf(g_csv, ",host_fps,host_ms,gl_ms,audio_ms,underruns,host_max_ms\n"); // [lcs-recomp] host_max_ms
            std::fprintf(stderr, "[perf] recording to %s\n", name.c_str());
        }

        void closeCsv()
        {
            if (!g_csv)
                return;
            std::fclose(g_csv);
            g_csv = nullptr;
            std::fprintf(stderr, "[perf] recording saved: %s (summarise with: python tools/perf_report.py %s)\n", g_csvName.c_str(),
                         g_csvName.c_str());
        }

        std::string timestampedName()
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), "perf_%Y%m%d_%H%M%S.csv", &tm);
            return buf;
        }

        // Last `n` samples, oldest first. Caller holds g_mutex.
        std::vector<Sample> recent(size_t n)
        {
            n = std::min(n, g_size);
            std::vector<Sample> out;
            out.reserve(n);
            for (size_t i = 0; i < n; ++i)
                out.push_back(g_ring[(g_head + kHistory - n + i) % kHistory]);
            return out;
        }

        struct Summary
        {
            size_t frames = 0;
            double seconds = 0.0, fps = 0.0, avgMs = 0.0, p50 = 0.0, p95 = 0.0, p99 = 0.0, maxMs = 0.0;
            double phaseMs[kPhases]{};
            double counters[kCounters]{};
            double vblanksPerS = 0.0, hostFps = 0.0, hostMs = 0.0, glMs = 0.0, audioMs = 0.0;
            uint64_t underruns = 0;
            double workerMs = 0.0;
            double gsMs = 0.0;
        };

        Summary summarize(const std::vector<Sample> &s)
        {
            Summary r;
            r.frames = s.size();
            if (s.empty())
                return r;
            std::vector<float> ms;
            ms.reserve(s.size());
            double vblanks = 0.0, hostW = 0.0;
            for (const Sample &x : s)
            {
                ms.push_back(x.frameMs);
                r.seconds += x.frameMs / 1000.0;
                vblanks += x.vblanks;
                for (size_t p = 0; p < kPhases; ++p)
                    r.phaseMs[p] += x.phaseMs[p];
                for (size_t c = 0; c < kCounters; ++c)
                    r.counters[c] += x.counters[c];
                if (x.hostFps > 0.0f)
                {
                    r.hostFps += x.hostFps;
                    r.hostMs += x.hostMs;
                    r.glMs += x.glMs;
                    hostW += 1.0;
                }
                r.underruns += x.underruns;
                r.workerMs += x.workerMs;
                r.gsMs += x.gsMs;
            }
            const double n = static_cast<double>(s.size());
            for (double &p : r.phaseMs)
                p /= n;
            for (double &c : r.counters)
                c /= n;
            r.workerMs /= n;
            r.gsMs /= n;
            if (hostW > 0.0)
            {
                r.hostFps /= hostW;
                r.hostMs /= hostW;
                r.glMs /= hostW;
            }
            r.audioMs = s.back().audioMs;
            r.avgMs = r.seconds * 1000.0 / n;
            r.fps = r.seconds > 0.0 ? n / r.seconds : 0.0;
            r.vblanksPerS = r.seconds > 0.0 ? vblanks / r.seconds : 0.0;
            std::sort(ms.begin(), ms.end());
            auto pct = [&](double q) { return static_cast<double>(ms[std::min(ms.size() - 1, static_cast<size_t>(q * (ms.size() - 1) + 0.5))]); };
            r.p50 = pct(0.50);
            r.p95 = pct(0.95);
            r.p99 = pct(0.99);
            r.maxMs = ms.back();
            return r;
        }

        void closeFrame(uint64_t now)
        {
            if (!g_firstMarkNs)
            {
                g_firstMarkNs = g_lastMarkNs = now;
                for (auto &p : g_phaseNs)
                    p.store(0, std::memory_order_relaxed);
                for (auto &c : g_counters)
                    c.store(0, std::memory_order_relaxed);
                g_vblanks = 0;
                return;
            }
            Sample s;
            const uint64_t frameNs = now - g_lastMarkNs;
            g_lastMarkNs = now;
            s.t = (now - g_firstMarkNs) / 1e9;
            s.frameMs = static_cast<float>(frameNs / 1e6);
            uint64_t claimed = 0;
            for (size_t p = 1; p < kPhases; ++p)
            {
                const uint64_t ns = g_phaseNs[p].exchange(0, std::memory_order_relaxed);
                claimed += ns;
                s.phaseMs[p] = static_cast<float>(ns / 1e6);
            }
            g_phaseNs[0].store(0, std::memory_order_relaxed);
            s.phaseMs[0] = static_cast<float>((frameNs > claimed ? frameNs - claimed : 0) / 1e6);
            for (size_t c = 0; c < kCounters; ++c)
                s.counters[c] = static_cast<uint32_t>(g_counters[c].exchange(0, std::memory_order_relaxed));
            s.vblanks = g_vblanks;
            g_vblanks = 0;
            s.workerMs = static_cast<float>(g_workerNs.exchange(0, std::memory_order_relaxed) / 1e6);
            s.gsMs = static_cast<float>(g_gsStageNs.exchange(0, std::memory_order_relaxed) / 1e6);
            const uint64_t hostFrames = g_hostFrames.exchange(0, std::memory_order_relaxed);
            const uint64_t hostNs = g_hostNs.exchange(0, std::memory_order_relaxed);
            const uint64_t glNs = g_glNs.exchange(0, std::memory_order_relaxed);
            s.hostMaxMs = static_cast<float>(g_hostMaxNs.exchange(0, std::memory_order_relaxed) / 1e6);
            if (hostFrames)
            {
                s.hostMs = static_cast<float>(hostNs / 1e6 / hostFrames);
                s.hostFps = s.hostMs > 0.0f ? 1000.0f / s.hostMs : 0.0f;
                s.glMs = static_cast<float>(glNs / 1e6 / hostFrames);
            }
            s.audioMs = ps2AudioQueuedMs();
            const uint64_t underruns = ps2AudioUnderrunCount();
            s.underruns = static_cast<uint32_t>(underruns - g_lastUnderruns);
            g_lastUnderruns = underruns;

            std::lock_guard<std::mutex> lock(g_mutex);
            g_ring[g_head] = s;
            g_head = (g_head + 1) % kHistory;
            g_size = std::min(g_size + 1, kHistory);
            if (!g_csv && !g_logFromEnv)
            {
                g_logFromEnv = true;
                if (const char *path = std::getenv("PS2X_PERF_LOG"))
                    openCsv(path);
            }
            if (!g_csv)
                return;
            std::fprintf(g_csv, "%.4f,%.3f,%u", s.t, s.frameMs, s.vblanks);
            for (float p : s.phaseMs)
                std::fprintf(g_csv, ",%.3f", p);
            for (uint32_t c : s.counters)
                std::fprintf(g_csv, ",%u", c);
            std::fprintf(g_csv, ",%.1f,%.3f,%.3f,%.1f,%u,%.3f\n", s.hostFps, s.hostMs, s.glMs, s.audioMs, s.underruns, s.hostMaxMs);
            if (s.t - g_lastSummaryT >= 5.0)
            {
                // one line per 5 s on stderr, for logs of scripted runs
                std::vector<Sample> window;
                for (size_t i = 0; i < g_size; ++i)
                {
                    const Sample &x = g_ring[(g_head + kHistory - 1 - i) % kHistory];
                    if (x.t <= g_lastSummaryT)
                        break;
                    window.push_back(x);
                }
                g_lastSummaryT = s.t;
                const Summary r = summarize(window);
                std::fprintf(stderr,
                             "[perf] t=%.0fs game %.1f fps (avg %.1f p95 %.1f max %.1f ms) vbl %.1f/s | ee game %.1f render %.1f iop %.1f wait %.1f ms"
                             " | %.0f draws %.0fk verts %.0f tex %.0f tags %.0f vu1 | host %.0f fps gl %.1f ms | audio %.0f ms %llu underruns\n",
                             s.t, r.fps, r.avgMs, r.p95, r.maxMs, r.vblanksPerS, r.phaseMs[0], r.phaseMs[1], r.phaseMs[2], r.phaseMs[3],
                             r.counters[0], r.counters[1] / 1000.0, r.counters[2], r.counters[3], r.counters[4], r.hostFps, r.glMs,
                             r.audioMs, static_cast<unsigned long long>(r.underruns));
                std::fflush(g_csv);
            }
        }

        // ---- the "REC" mark while a CSV recording runs (main thread): Dear ImGui background draw list ------------
        using Color = ImU32;
        constexpr Color rgba(int r, int g, int b, int a) { return IM_COL32(r, g, b, a); }
        int screenHeight() { return static_cast<int>(ImGui::GetIO().DisplaySize.y); }
        void text(const char *t, float x, float y, float size, Color c)
        {
            ImGui::GetBackgroundDrawList()->AddText(ImGui::GetFont(), size, ImVec2(x, y), c, t);
        }

        std::string fmt(const char *f, ...)
        {
            char buf[256];
            va_list ap;
            va_start(ap, f);
            std::vsnprintf(buf, sizeof(buf), f, ap);
            va_end(ap);
            return buf;
        }
    }

    void add(Counter counter, uint64_t amount)
    {
        g_counters[static_cast<size_t>(counter)].fetch_add(amount, std::memory_order_relaxed);
    }

    void addWorkerTime(uint64_t ns)
    {
        g_workerNs.fetch_add(ns, std::memory_order_relaxed);
    }

    void addGsStageTime(uint64_t ns)
    {
        g_gsStageNs.fetch_add(ns, std::memory_order_relaxed);
    }

    PhaseScope::PhaseScope(Phase phase) noexcept : m_parent(t_current), m_start(nowNs()), m_childNs(0), m_phase(phase)
    {
        t_current = this;
    }

    PhaseScope::~PhaseScope()
    {
        const uint64_t elapsed = nowNs() - m_start;
        t_current = m_parent;
        g_phaseNs[static_cast<size_t>(m_phase)].fetch_add(elapsed > m_childNs ? elapsed - m_childNs : 0, std::memory_order_relaxed);
        if (m_parent)
            m_parent->m_childNs += elapsed;
    }

    void markGameFrame()
    {
        const uint64_t now = nowNs();
        g_lastGameMarkNs = now;
        closeFrame(now);
    }

    void markVBlank()
    {
        ++g_vblanks;
        const uint64_t now = nowNs();
        if (!g_lastGameMarkNs || now - g_lastGameMarkNs > 1000000000ull) // the game marks no frames: VBlank = frame
            closeFrame(now);
    }

    void hostFrame(double glMs)
    {
        const uint64_t now = nowNs();
        if (g_lastHostNs)
        {
            g_hostNs.fetch_add(now - g_lastHostNs, std::memory_order_relaxed);
            for (uint64_t gap = now - g_lastHostNs, seen = g_hostMaxNs.load(std::memory_order_relaxed);
                 gap > seen && !g_hostMaxNs.compare_exchange_weak(seen, gap, std::memory_order_relaxed);)
            {
            }
            g_glNs.fetch_add(static_cast<uint64_t>(glMs * 1e6), std::memory_order_relaxed);
            g_hostFrames.fetch_add(1, std::memory_order_relaxed);
        }
        g_lastHostNs = now;
    }

    void drawOverlay(bool handleKeys)
    {
        bool recording;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (handleKeys && ps2control::keyPressed(platform::KEY_F4))
            {
                g_logFromEnv = true; // a manual toggle wins over PS2X_PERF_LOG
                if (g_csv)
                    closeCsv();
                else
                    openCsv(timestampedName());
            }
            recording = g_csv != nullptr;
        }
        if (recording)
            text("REC", 10, 10, std::max(10, screenHeight() / 46), rgba(245, 90, 90, 255));
    }

    namespace
    {
        std::atomic<uint64_t> g_probedFrames{0}, g_blackFrames{0}, g_dipFrames{0};
        uint64_t g_blackRunStart = 0, g_blackRunLength = 0; // main thread
        float g_lumaBeforeRun = 0.0f, g_lastLuma = 0.0f;
        float g_lumaAverage = 0.0f;                           // slow average: the scene's normal brightness
        uint64_t g_dipStart = 0, g_dipLength = 0;
        float g_dipReference = 0.0f, g_dipMin = 0.0f;
    }

    bool noteFrameLuma(float mean, uint64_t hostFrame)
    {
        bool started = false;
        constexpr float kBlack = 6.0f; // the middle of the picture all but black
        g_probedFrames.fetch_add(1, std::memory_order_relaxed);
        if (mean < kBlack)
        {
            g_blackFrames.fetch_add(1, std::memory_order_relaxed);
            if (g_blackRunLength++ == 0)
            {
                started = true;
                g_blackRunStart = hostFrame;
                g_lumaBeforeRun = g_lastLuma;
            }
        }
        else if (g_blackRunLength)
        {
            std::fprintf(stderr, "[frames] black run of %llu host frames from %llu (luma before %.1f, after %.1f)\n",
                         static_cast<unsigned long long>(g_blackRunLength), static_cast<unsigned long long>(g_blackRunStart),
                         g_lumaBeforeRun, mean);
            g_blackRunLength = 0;
        }
        // Dips: a short run of frames much darker than the scene was (objects / parts of the world drawn black for
        // a few frames, which a whole-screen black test misses). Runs longer than 2 s are scene changes, not dips.
        const bool dark = g_lumaAverage > 25.0f && mean < g_lumaAverage * 0.55f;
        if (dark)
        {
            if (g_dipLength++ == 0)
            {
                started = true;
                g_dipStart = hostFrame;
                g_dipReference = g_lumaAverage;
                g_dipMin = mean;
            }
            g_dipMin = std::min(g_dipMin, mean);
        }
        else if (g_dipLength)
        {
            if (g_dipLength <= 120)
            {
                g_dipFrames.fetch_add(g_dipLength, std::memory_order_relaxed);
                std::fprintf(stderr, "[frames] dip of %llu host frames from %llu (scene luma %.1f, lowest %.1f, after %.1f)\n",
                             static_cast<unsigned long long>(g_dipLength), static_cast<unsigned long long>(g_dipStart),
                             g_dipReference, g_dipMin, mean);
            }
            g_dipLength = 0;
        }
        if (!g_dipLength || g_dipLength > 120)
            g_lumaAverage = g_lumaAverage <= 0.0f ? mean : g_lumaAverage * 0.95f + mean * 0.05f;
        g_lastLuma = mean;
        return started;
    }


    std::string summaryLine()
    {
        std::vector<Sample> lastSecond;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const Sample &x : recent(kHistory))
                lastSecond.push_back(x);
        }
        while (lastSecond.size() > 1 && lastSecond.back().t - lastSecond.front().t > 1.0)
            lastSecond.erase(lastSecond.begin());
        const Summary r = summarize(lastSecond);
        return fmt("game_fps=%.1f frame_ms=%.1f vblank_per_s=%.1f game_ms=%.1f render_ms=%.1f iop_ms=%.1f wait_ms=%.1f draws=%.0f "
                   "verts=%.0f tex_uploads=%.0f dma_tags=%.0f vu1_programs=%.0f host_fps=%.0f gl_ms=%.1f audio_ms=%.0f",
                   r.fps, r.avgMs, r.vblanksPerS, r.phaseMs[0], r.phaseMs[1], r.phaseMs[2], r.phaseMs[3], r.counters[0], r.counters[1],
                   r.counters[2], r.counters[3], r.counters[4], r.hostFps, r.glMs, r.audioMs) +
               fmt(" black_frames=%llu dip_frames=%llu probed_frames=%llu", static_cast<unsigned long long>(g_blackFrames.load()),
                   static_cast<unsigned long long>(g_dipFrames.load()),
                   static_cast<unsigned long long>(g_probedFrames.load())) + // [lcs-recomp] brightness probe
               fmt(" worker_ms=%.1f gs_ms=%.1f", r.workerMs, r.gsMs) + // [lcs-recomp] render submission worker, its GS stage
               [&] { // [lcs-recomp] draw sources
                   std::string s;
                   for (size_t c = kFirstSourceCounter; c < kCounters; ++c)
                       s += fmt(" %s=%.1f", kCounterNames[c], r.counters[c]);
                   return s;
               }();
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        closeCsv();
    }
}
