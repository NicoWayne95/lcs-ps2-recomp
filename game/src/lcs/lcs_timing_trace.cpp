#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <share.h>

void lcsReadAudioTiming(uint64_t (&out)[7]);

void lcsTraceTiming(PS2Runtime *runtime, uint32_t gameMs, int32_t deltaTicks,
                    float step, float unclippedStep, float timeScale)
{
    using Clock = std::chrono::steady_clock;
    static FILE *file = [] {
        FILE *trace = nullptr;
        if (const char *path = std::getenv("LCS_TIMING_CSV"))
            trace = _fsopen(path, "w", _SH_DENYNO);
        if (trace)
            std::fputs("frame,host_ms,game_ms,bus_ticks,step,unclipped_step,time_scale,vsync,spu_frames,host_audio_frames,queued_frames,dropped_frames,underrun_frames,spu_signal_frames,host_signal_frames\n", trace);
        return trace;
    }();
    if (!file)
        return;

    static const auto started = Clock::now();
    static unsigned frame = 0;
    uint64_t audio[7]{};
    lcsReadAudioTiming(audio);
    const double hostMs = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto vsync = runtime->eeScheduler().currentVSyncTick();
    std::fprintf(file, "%u,%.3f,%u,%d,%.6f,%.6f,%.6f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                 ++frame, hostMs, gameMs, deltaTicks, step, unclippedStep, timeScale,
                 static_cast<unsigned long long>(vsync),
                 static_cast<unsigned long long>(audio[0]), static_cast<unsigned long long>(audio[1]),
                 static_cast<unsigned long long>(audio[2]), static_cast<unsigned long long>(audio[3]),
                 static_cast<unsigned long long>(audio[4]), static_cast<unsigned long long>(audio[5]),
                 static_cast<unsigned long long>(audio[6]));
    if (frame % 30u == 0)
        std::fflush(file);
}

uint64_t lcsSelectCTimerTicks(uint64_t guestTicks)
{
    static const bool enabled = [] {
        const char *value = std::getenv("LCS_CTIMER_HOST");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    if (!enabled)
        return guestTicks;

    using Clock = std::chrono::steady_clock;
    static const auto origin = Clock::now();
    static const uint64_t base = guestTicks;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count();
    constexpr uint64_t busTicksPerMs = 147456u;
    return base + static_cast<uint64_t>(elapsedMs) * busTicksPerMs;
}
