// [lcs] LCS host main on the lcs-recomp the reference port runtime (third_party/PS2Recomp from the the reference port project): its D3D11 hardware
// GS, render worker / GS stage, RSL-capable chain walker, presenter. Based on the reference port's main
// without the the reference port game layer (trainer, HUD, display, input bindings, disc image): LCS runs from its extracted ISO folder,
// the ELF path is the first argument.
#include "lcs/lcs_diag.h"
#include "ps2_runtime.h"
#include "runtime/ps2_present.h"
#include "runtime/ps2_perf.h"
#include "runtime/ee_scheduler.h"
#include <thread>
#include <array>
#include "lcs/lcs_bootstrap.h"
#include "lcs/lcs_sound.h"
#include "lcs/lcs_menu.h"
#include "lcs/lcs_world_detail.h"
#include <cmath>
#include "game/lcs_rsl_world.h"
#include "disc/iso_image.h"
#include "runtime/ps2_disc_image.h"
#include <memory>
#include <cstring>
#include <cctype>
#include "debug/profiler.h"
#include <atomic>

namespace lcs_hud { void setEdges(bool on); } // [lcs-hud] src/lcs/lcs_hud.cpp
namespace lcs_input { void hostFrame(bool pageOpen); } // [lcs-input] src/lcs/lcs_input.cpp (menu build)
namespace lcs_freecam // [lcs-freecam] src/lcs/lcs_freecam.cpp
{
    bool installed();
    bool enabled();
    void setEnabled(bool on);
    float speed();
    void setSpeed(float metresPerSecond);
    void hostFrame(bool menuWantsMouse);
}
// [lcs] render diagnosis counters for the F2 overlay (ps2_vif1_interpreter.cpp, vu/ps2_vu1_core.cpp)
extern std::atomic<uint64_t> g_ps2xVifGarbageCount, g_ps2xVifGarbageLastMs;
extern std::atomic<uint64_t> g_ps2xVu1CutBudget, g_ps2xVu1CutStop;
#include "runtime/gs/gs_hw.h"
#include "platform/platform.h"
#include "render/presenter.h"
#include <string_view>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <imgui.h>

#include <chrono>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int g_lcsPresentMode = 2;       // [lcs] choosePresentMode / the video page: 0 immediate, 1 vsync, 2 tearing (VRR)
constexpr int kDefaultReflectionFilter = 0;
int g_lcsReflectionFilter = kDefaultReflectionFilter;  // [lcs] the reflection map's filter (gsHwSetTargetFilter 420): 0 GS, 1 smooth, 2 smooth above 1x

namespace
{
    // [lcs] the display's shape and the HUD (LCS_ASPECT / LCS_HUD / LCS_HUD_SQUEEZE at start, the F2 video page live)
    struct DisplayState
    {
        bool wide = true;        // 16:9 (the default) or 4:3, the PS2's TV
        int hudMode = 1;         // 0 squeeze, 1 edges, 2 stretch
        float squeeze = 0.875f;  // squeeze mode: the HUD's width factor (0.75 = 4:3 shape .. 1 = as drawn)
    };
    DisplayState g_display;

    // 16:9: 3D frames shown at 16:9 (present::setDisplayAspect; menus and loading screens stay 4:3), the game's view
    // window widened Hor+ (lcs_bootstrap); as the reference port's applyAspect: post effects keep their 4:3 proportions, the HUD its
    // shape by mode: "edges" (default) anchors each element at its screen edge, "squeeze" centres the HUD
    // around the middle, and "stretch" uses the drawn proportions. The 4:3 mode uses native HUD coordinates.
    void applyDisplay()
    {
        const DisplayState &d = g_display;
        present::setDisplayAspect(d.wide ? 16.0f / 9.0f : 4.0f / 3.0f);
        const float r = d.wide ? (16.0f / 9.0f) / (4.0f / 3.0f) : 1.0f;
        lcs_bootstrap::setHorPlusWidening(r);
        // no post-effect widening: the frame is drawn 16:9 in the game's 640 units already (native frame), the trails
        // with the widening came out shifted (green bands, run 21:34)
        gsHwSetAspectWidening(1.0f);
        const float squeeze = d.hudMode == 0 ? std::clamp(d.squeeze, 1.0f / r, 1.0f) : d.hudMode == 2 ? 1.0f : 1.0f / r;
        gsHwSetHudSqueeze(d.wide ? squeeze : 1.0f);
        lcs_hud::setEdges(d.wide && d.hudMode == 1);
        std::cerr << "[lcs] display aspect " << (d.wide ? "16:9" : "4:3") << ", HUD "
                  << (d.hudMode == 0 ? "squeeze" : d.hudMode == 1 ? "edges" : "stretch") << std::endl;
    }

    struct Preset { const char *name; uint32_t w43, w169, h; };
    constexpr Preset kPresets[] = {{"Original (640x448)", 640, 640, 448}, {"1080p", 1440, 1920, 1080},
                                   {"1440p", 1920, 2560, 1440}, {"4K", 2880, 3840, 2160}, {"Window", 0, 0, 0}};
    int g_preset = 2;
    void applyPreset(int p)
    {
        g_preset = p;
        gsHwSetFrameSizeOverride(g_display.wide ? kPresets[p].w169 : kPresets[p].w43, kPresets[p].h);
    }

    // [lcs-menu] the pause menu's video page (src/lcs/lcs_menu.cpp, the menu build only; the user's layout 2026-11-16:
    // BRIGHTNESS and SUBTITLES are the game's, these are set as the F2 video page sets them)
    // 0.5x + 0.25x a step (9 bars), 2 = 1.0x = the PS2; lcs_world (src/lcs/lcs_world_detail.cpp) applies it
    int drawDistanceIndex()
    {
        return std::clamp(static_cast<int>(std::lround((lcs_world::drawDistance() - 0.5f) / 0.25f)), 0, 8);
    }
    namespace menu
    {
        const char *const kOnOff[] = {"OFF", "ON"};
        const char *const kLimiter[] = {"30", "60", "120", "OFF"};
        // the presets' sizes as the frame is drawn (kPresets: 16:9 or 4:3 widths), set again when WIDE SCREEN changes
        const char *kResolution[] = {"ORIGINAL", "2560 X 1440", "2560 X 1440", "3840 X 2160", "WINDOW"};
        void resolutionNames()
        {
            const bool wide = g_display.wide;
            kResolution[1] = wide ? "1920 X 1080" : "1440 X 1080";
            kResolution[2] = wide ? "2560 X 1440" : "1920 X 1440";
            kResolution[3] = wide ? "3840 X 2160" : "2880 X 2160";
        }
        const char *const kMsaa[] = {"OFF", "2X", "4X", "8X"};
        const char *const kAniso[] = {"OFF", "2X", "4X", "8X", "16X"};
        int msaaIndex()
        {
            const uint32_t n = gsHwMsaa();
            return n >= 8u ? 3 : n >= 4u ? 2 : n >= 2u ? 1 : 0;
        }
        int anisoIndex()
        {
            const uint32_t n = gsHwAnisotropy();
            return n >= 16u ? 4 : n >= 8u ? 3 : n >= 4u ? 2 : n >= 2u ? 1 : 0;
        }
        const lcs_menu::Row kRows[] = {
            {"SCREEN RESOLUTION:", kResolution, 5, [] { return g_preset; }, [](int i) { applyPreset(i); }},
            {"WIDE SCREEN:", kOnOff, 2, [] { return g_display.wide ? 1 : 0; },
             [](int i) {
                 g_display.wide = i != 0; // ours (16:9, Hor+), not the game's anamorphic one
                 applyDisplay();
                 applyPreset(g_preset);
                 resolutionNames();
             }},
            {"FRAME SYNC:", kOnOff, 2, [] { return g_lcsPresentMode == 1 ? 1 : 0; },
             [](int i) {
                 g_lcsPresentMode = i ? 1 : 2; // on: VSync; off: tearing / VRR
                 present::setPresentMode(g_lcsPresentMode);
             }},
            {"FRAME LIMITER:", kLimiter, 4, [] { return lcs_bootstrap::frameRateModeIndex(); },
             [](int i) { lcs_bootstrap::setFrameRateMode(i); }},
            // (the game's BRIGHTNESS comes here: lcs_menu's layout)
            {"DRAW DISTANCE:", nullptr, 9, drawDistanceIndex,
             [](int i) {
                 // both: the LOD scale (peds, cars, entities) and the far plane / fog (buildings, roads, props)
                 lcs_world::setDrawDistance(0.5f + 0.25f * static_cast<float>(i));
                 lcs_world::setWorldDistance(0.5f + 0.25f * static_cast<float>(i));
             },
             true},
            {"MSAA:", kMsaa, 4, msaaIndex,
             [](int i) {
                 const uint32_t v[] = {1, 2, 4, 8};
                 gsHwSetMsaa(v[i]);
             }},
            {"TEXTURE FILTERING:", kAniso, 5, anisoIndex,
             [](int i) {
                 const uint32_t v[] = {0, 2, 4, 8, 16};
                 gsHwSetAnisotropy(v[i]);
             }},
            {"TRAILS:", kOnOff, 2, [] { return gsHwTrailsEnabled() ? 1 : 0; }, [](int i) { gsHwSetTrailsEnabled(i != 0); }},
            {"BETA POLICE CARS:", kOnOff, 2, [] { return lcs_menu::betaPolice() ? 1 : 0; },
             [](int i) { lcs_menu::setBetaPolice(i != 0); }},
            {"RESTORE DEFAULTS", nullptr, 0, nullptr, nullptr},
        };
    }

    void drawVideoPage(); // [lcs] F2, second page

    void drawLcsDebugOverlay(PS2Runtime &runtime, void *)
    {
        if (!lcs_bootstrap::bootMoviesActive() && platform::keyPressed(platform::KEY_F1))
            lcs_bootstrap::requestMaverick();

        if (platform::keyPressed(platform::KEY_F10)) // [lcs] LCS_WORLD_DUMP=1: guest RAM dump (lcs_world_detail.cpp)
            lcs_world::requestWorldDump();

        if (platform::keyPressed(platform::KEY_F11))
            platform::setFullscreen(!platform::fullscreen());

        if (platform::keyPressed(platform::KEY_F6))
            lcs_bootstrap::cycleFrameRateMode();

        // [lcs] F5: scaler presets for the picture's render targets (native-frame mode, PS2X_HW_SCALE=0): the PS2's 448
        // lines, 1080p, 1440p, 4K - 4:3 frames of that height (the presenter letterboxes them); "Window" follows the window
        // [lcs] 16:9 (LCS_ASPECT=16:9, the default; 4:3 = the PS2's TV): 3D frames shown at 16:9 (present::setDisplayAspect;
        // menus and loading screens stay 4:3) and the presets drawn 16:9 wide - with the game's own widescreen option
        // (its display menu) the picture it renders for that is wider, not stretched
        static const bool displayInit = [] {
            const char *e = std::getenv("LCS_ASPECT");
            g_display.wide = !(e && std::string_view(e) == "4:3");
            const char *hud = std::getenv("LCS_HUD");
            const std::string_view mode = hud ? hud : "";
            if (mode == "squeeze")
                g_display.hudMode = 0;
            else if (mode == "stretch")
                g_display.hudMode = 2;
            if (const char *s = std::getenv("LCS_HUD_SQUEEZE"); s && *s)
                g_display.squeeze = std::clamp(static_cast<float>(std::atof(s)), 0.75f, 1.0f);
            applyDisplay();
            const char *sc = std::getenv("LCS_SCALER"); // 0..4 (kPresets), default 1440p
            applyPreset(sc && *sc >= '0' && *sc <= '4' ? *sc - '0' : 2);
            menu::resolutionNames();
            lcs_menu::setRows(menu::kRows, static_cast<int>(std::size(menu::kRows))); // their start values: the defaults
            return true;
        }();
        (void)displayInit;
        const bool wide = g_display.wide;
        const auto &presets = kPresets;
        const int preset = g_preset;
        static const bool nativeFrame = [] { const char *s = std::getenv("PS2X_HW_SCALE"); return s && s[0] == '0'; }();
        if (platform::keyPressed(platform::KEY_F5))
        {
            applyPreset((g_preset + 1) % static_cast<int>(std::size(kPresets)));
            std::cerr << "[lcs] F5 scaler: " << kPresets[g_preset].name << (nativeFrame ? "" : " (needs PS2X_HW_SCALE=0)") << std::endl;
        }
        // [lcs] F9: hide / show the game's HUD (gs_hw.cpp: its 2D draws are left out)
        if (platform::keyPressed(platform::KEY_F9))
        {
            gsHwSetHudHidden(!gsHwHudHidden());
            std::cerr << "[lcs] F9 HUD " << (gsHwHudHidden() ? "hidden" : "shown") << std::endl;
        }
        // [lcs] F7: the trails (LCS's 512x512 motion-blur pass, at the picture's scale)
        if (platform::keyPressed(platform::KEY_F7))
        {
            gsHwSetTrailsEnabled(!gsHwTrailsEnabled());
            std::cerr << "[lcs] F7 trails " << (gsHwTrailsEnabled() ? "on" : "off") << std::endl;
        }

        { // [lcs] hang diagnosis (LCS_EE_TRACE=0: off): the guest PC sampled every host frame, counted; every 2 s the
          // most frequent ones and each EE thread's state - a loop the game no longer leaves shows as a few PCs
            static const bool trace = lcs_diag::wanted("LCS_EE_TRACE"); // release: only with LCS_EE_TRACE=1
            if (trace)
            {
                static std::unordered_map<uint32_t, uint32_t> pcs;
                static auto at = std::chrono::steady_clock::now();
                const auto snap = runtime.eeScheduler().snapshot();
                ++pcs[snap.runningThreadId ? snap.runningPc : 0u];
                if (std::chrono::steady_clock::now() - at >= std::chrono::seconds(2))
                {
                    at = std::chrono::steady_clock::now();
                    std::vector<std::pair<uint32_t, uint32_t>> top(pcs.begin(), pcs.end());
                    std::sort(top.begin(), top.end(), [](auto &a, auto &b) { return a.second > b.second; });
                    std::fprintf(stderr, "[ee-trace] flips %llu cycle %llu pcs:", (unsigned long long)lcs_bootstrap::guestFramesPresented(),
                                 (unsigned long long)snap.eeCycle);
                    for (size_t i = 0; i < top.size() && i < 6; ++i)
                        std::fprintf(stderr, " %08x x%u", top[i].first, top[i].second);
                    std::fprintf(stderr, " | threads:");
                    for (const auto &t : snap.threads)
                        std::fprintf(stderr, " [%d pc %08x ra %08x st %d wait %d/%d]", t.id, t.pc, t.ra, (int)t.status,
                                     (int)t.waitReason, t.waitId);
                    std::fprintf(stderr, "\n");
                    pcs.clear();
                    // [lcs] the EE cycle stands still while the game thread is busy: a host-side loop. Its host stacks
                    // (exe-relative; llvm-symbolizer --obj=<exe> 0x...), a few samples, at most 4 times a run
                    static uint64_t lastCycle = ~0ull;
                    static int stillReports = 0, reports = 0;
                    stillReports = snap.eeCycle == lastCycle ? stillReports + 1 : 0;
                    lastCycle = snap.eeCycle;
                    if (stillReports == 2 && reports < 4)
                    {
                        ++reports;
                        for (int sample = 0; sample < 6; ++sample)
                        {
                            uint64_t stack[32];
                            const int n = ps2perf::hostThreadStack(ps2perf::HostThread::Ee, stack, 32);
                            std::fprintf(stderr, "[ee-hang] cycle %llu still, host stack %d:", (unsigned long long)snap.eeCycle, sample);
                            for (int i = 0; i < n; ++i)
                                std::fprintf(stderr, " 0x%llx", (unsigned long long)stack[i]);
                            std::fprintf(stderr, "\n");
                            std::this_thread::sleep_for(std::chrono::milliseconds(30));
                        }
                        std::fflush(stderr);
                    }
                }
            }
        }
        // [lcs] F2 cycles: off -> debug (read only) -> video (switches and sliders, mouse) -> off
        static int page = 0;
        if (platform::keyPressed(platform::KEY_F2))
            page = (page + 1) % 3;
        const bool visible = page == 1;
        lcs_freecam::hostFrame(page == 2); // [lcs-freecam] F8; the video page keeps the cursor
        lcs_menu::hostFrame();             // [lcs-menu] the pause menu's video page: its changes applied
        lcs_input::hostFrame(page != 0);   // [lcs-input] the mouse held while playing on keyboard and mouse
        menu::resolutionNames();           // (the aspect may have changed on the F2 page too)

        using Clock = std::chrono::steady_clock;
        const auto now = Clock::now();
        const auto frames = lcs_bootstrap::guestFramesPresented();
        static auto sampledAt = now;
        static auto sampledFrames = frames;
        static double fps = 0.0;
        const double elapsed = std::chrono::duration<double>(now - sampledAt).count();
        if (elapsed >= 0.5)
        {
            fps = static_cast<double>(frames - sampledFrames) / elapsed;
            sampledAt = now;
            sampledFrames = frames;
        }
        if (page == 2)
        {
            drawVideoPage();
            return;
        }
        if (!visible)
            return;

        static constexpr std::array threadNames{"Main / D3D11", "EE / Game", "Render Worker", "GS Stage"};
        static std::array<uint64_t, threadNames.size()> cpuTimes{};
        static std::array<double, threadNames.size()> cpuPercent{-1.0, -1.0, -1.0, -1.0};
        static std::array<bool, threadNames.size()> cpuReady{};
        static Clock::time_point debugAt{};
        static EeKernelSnapshot ee{};
        static double eeMHz = 0.0;
        const double debugElapsed = std::chrono::duration<double>(now - debugAt).count();
        const bool firstSample = debugAt == Clock::time_point{};
        if (firstSample || debugElapsed >= 0.5)
        {
            auto next = runtime.eeScheduler().snapshot();
            if (!firstSample && next.eeCycle >= ee.eeCycle)
                eeMHz = static_cast<double>(next.eeCycle - ee.eeCycle) / debugElapsed / 1e6;
            ee = std::move(next);

            for (size_t i = 0; i < threadNames.size(); ++i)
            {
                uint64_t cpuTime = 0;
                const bool available = ps2perf::hostThreadCpuTime(static_cast<ps2perf::HostThread>(i), cpuTime);
                cpuPercent[i] = available && cpuReady[i] && cpuTime >= cpuTimes[i]
                                    ? static_cast<double>(cpuTime - cpuTimes[i]) / debugElapsed / 1e5
                                    : -1.0;
                cpuReady[i] = available;
                cpuTimes[i] = cpuTime;
            }
            debugAt = now;
        }

        int width = 0, height = 0;
        platform::windowSize(width, height);
        const char *scale = std::getenv("PS2X_HW_SCALE");
        ImGui::PushFont(nullptr, 15.0f); // [lcs] 2 px above the default 13 px
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.8f);
        if (ImGui::Begin("LCS Debug", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
        {
            if (fps > 0.0)
                ImGui::Text("Game %.1f FPS | %.1f ms/frame", fps, 1000.0 / fps);
            else
                ImGui::TextUnformatted("Waiting for game frames");
            ImGui::Text("Frames: %llu", frames);
            ImGui::Text("F5  Scaler: %s %s%s", presets[preset].name, wide ? "16:9" : "4:3",
                        nativeFrame ? "" : " (inactive: PS2X_HW_SCALE != 0)");
            ImGui::Text("F6  Frame rate: %s", lcs_bootstrap::frameRateModeLabel());
            ImGui::Text("F7  Trails: %s", gsHwTrailsEnabled() ? "on" : "off");
            ImGui::Text("F9  HUD: %s", gsHwHudHidden() ? "hidden" : "shown");
            if (lcs_freecam::installed())
                ImGui::Text("F8  Free camera: %s", lcs_freecam::enabled() ? "flying (game pad blocked)" : "off");
            ImGui::TextDisabled("F2  again: video page (switches, sliders)");
            ImGui::Text("EE cycle: %llu | %.1f MHz (%.0f%% of PS2)",
                        static_cast<unsigned long long>(ee.eeCycle), eeMHz, 100.0 * eeMHz / 294.912);
            if (ee.runningThreadId)
                ImGui::Text("EE guest thread %d | sampled PC 0x%08X", ee.runningThreadId,
                            static_cast<unsigned>(ee.runningPc));
            else
                ImGui::TextUnformatted("EE scheduler idle");

            ImGui::Separator();
            ImGui::TextUnformatted("Thread CPU (100% = one logical core)");
            for (size_t i = 0; i < threadNames.size(); ++i)
            {
                if (cpuPercent[i] >= 0.0)
                    ImGui::Text("%s: %.1f%%", threadNames[i], cpuPercent[i]);
                else
                    ImGui::Text("%s: --", threadNames[i]);
            }
            ImGui::Separator();
            { // [lcs-heap] the game's heaps: in use / in all, free (the game's out-of-memory handler runs when one is full)
                unsigned heaps[8][3];
                int n = lcs_bootstrap::heapStats(heaps, 8);
                if (n == 0) // the release build has no free hook: the two heaps read directly (lcs_world)
                    for (int h = 0; h < 2; ++h)
                    {
                        uint32_t used = 0, size = 0;
                        if (lcs_world::heapUse(h, used, size))
                        {
                            heaps[n][0] = h == 0 ? 0x406D50u : 0x634140u;
                            heaps[n][1] = used;
                            heaps[n][2] = size;
                            ++n;
                        }
                    }
                ImGui::TextUnformatted("Game memory (heap: used / size, free)");
                for (int i = 0; i < n; ++i)
                {
                    const double used = heaps[i][1] / 1048576.0, size = heaps[i][2] / 1048576.0;
                    const double freeMb = size > used ? size - used : 0.0;
                    const ImVec4 colour = freeMb < 0.5 ? ImVec4(1.0f, 0.35f, 0.3f, 1.0f) : freeMb < 1.5 ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                                                                                         : ImVec4(0.8f, 0.9f, 0.8f, 1.0f);
                    ImGui::TextColored(colour, "%s %08X: %.1f / %.1f MB, %.2f MB free (%.0f%%)",
                                       heaps[i][0] == 0x406D50u ? "Main  " : heaps[i][0] == 0x634140u ? "Second" : "Heap  ", heaps[i][0],
                                       used, size, freeMb, size > 0.0 ? 100.0 * used / size : 0.0);
                }
                if (n == 0)
                    ImGui::TextUnformatted("  (no heap seen yet)");
            }
            ImGui::Separator();
            { // [lcs] render diagnosis: garbage VIFcodes (stale DMA references), VU1 programs cut short, the RSL path
                const ImVec4 bad(1.0f, 0.35f, 0.3f, 1.0f), warn(1.0f, 0.8f, 0.3f, 1.0f), ok(0.8f, 0.9f, 0.8f, 1.0f);
                const uint64_t garbage = g_ps2xVifGarbageCount.load(std::memory_order_relaxed);
                if (garbage)
                {
                    const uint64_t nowMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                     Clock::now().time_since_epoch()).count());
                    const uint64_t lastMs = g_ps2xVifGarbageLastMs.load(std::memory_order_relaxed);
                    const double ago = nowMs > lastMs ? (nowMs - lastMs) / 1000.0 : 0.0;
                    ImGui::TextColored(ago < 10.0 ? bad : warn, "VIF garbage: %llu, last %.0f s ago", (unsigned long long)garbage, ago);
                }
                else
                    ImGui::TextColored(ok, "VIF garbage: none");
                const uint64_t cutBudget = g_ps2xVu1CutBudget.load(std::memory_order_relaxed);
                const uint64_t cutStop = g_ps2xVu1CutStop.load(std::memory_order_relaxed);
                ImGui::TextColored(cutBudget ? bad : cutStop ? warn : ok, "VU1 cut short: %llu by budget, %llu by a stop",
                                   (unsigned long long)cutBudget, (unsigned long long)cutStop);
                const auto rsl = lcs::rslworld::overlayStats();
                if (!rsl.active)
                    ImGui::TextUnformatted("RSL: off");
                else
                {
                    if (rsl.replay)
                        ImGui::Text("RSL replay: native setups %llu", (unsigned long long)rsl.setupsEmitted);
                    else
                        ImGui::TextColored(rsl.setupsDiffer ? bad : ok, "RSL verify: setups %llu compared, %llu differ",
                                           (unsigned long long)rsl.setupsCompared, (unsigned long long)rsl.setupsDiffer);
                    ImGui::Text("Sector lists (10 s): %llu replayed, %llu recorded, %llu refused",
                                (unsigned long long)rsl.sectorReplayed, (unsigned long long)rsl.sectorRecorded,
                                (unsigned long long)rsl.sectorNotReplayable);
                    ImGui::Text("Not copied: %llu (%llu from guest RAM) | %.0f KB copied per chain",
                                (unsigned long long)rsl.skipped, (unsigned long long)rsl.skipFallback, rsl.chainKb);
                }
            }
            ImGui::Separator();
            ImGui::Text("Output %d x %d", width, height);
            ImGui::Text("Internal resolution: %s", !scale ? "1x" : scale[0] == '0' ? "Match window" : scale);
        }
        ImGui::End();
        ImGui::PopFont();
    }

    // [lcs] F2, second page: every video setting as a switch or slider, live, for testing (not saved yet: the next start
    // takes the environment / launcher switches again). Mouse; the F keys keep working.
    void drawVideoPage()
    {
        static const bool nativeFrame = [] { const char *s = std::getenv("PS2X_HW_SCALE"); return s && s[0] == '0'; }();
        ImGui::PushFont(nullptr, 15.0f);
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.9f);
        if (ImGui::Begin("Video (F2: close)", nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::PushItemWidth(240.0f);
            ImGui::SeparatorText("Display");
            {
                int p = g_preset;
                const char *names[std::size(kPresets)];
                for (size_t i = 0; i < std::size(kPresets); ++i)
                    names[i] = kPresets[i].name;
                if (ImGui::Combo("Internal resolution (F5)", &p, names, static_cast<int>(std::size(kPresets))))
                    applyPreset(p);
                if (!nativeFrame)
                    ImGui::TextDisabled("  only with PS2X_HW_SCALE=0");
            }
            {
                int a = g_display.wide ? 0 : 1;
                const char *names[] = {"16:9", "4:3 (PS2)"};
                if (ImGui::Combo("Aspect ratio", &a, names, 2))
                {
                    g_display.wide = a == 0;
                    applyDisplay();
                    applyPreset(g_preset);
                }
            }
            {
                bool full = platform::fullscreen();
                if (ImGui::Checkbox("Fullscreen (F11)", &full))
                    platform::setFullscreen(full);
            }
            {
                const int modes[] = {2, 1, 0};
                const char *names[] = {"Tearing / VRR (G-Sync, FreeSync)", "VSync", "Immediate (no VSync)"};
                int i = g_lcsPresentMode == 2 ? 0 : g_lcsPresentMode == 1 ? 1 : 2;
                if (ImGui::Combo("Present mode", &i, names, 3))
                {
                    g_lcsPresentMode = modes[i];
                    present::setPresentMode(g_lcsPresentMode);
                    std::fprintf(stderr, "[present] video page: mode %d\n", g_lcsPresentMode);
                }
            }
            {
                const uint32_t values[] = {1, 2, 4, 8};
                const char *names[] = {"Off", "2x", "4x", "8x"};
                const uint32_t now = gsHwMsaa();
                int i = now >= 8u ? 3 : now >= 4u ? 2 : now >= 2u ? 1 : 0;
                if (ImGui::Combo("Anti-aliasing (MSAA)", &i, names, 4))
                    gsHwSetMsaa(values[i]);
            }
            {
                const uint32_t values[] = {0, 2, 4, 8, 16};
                const char *names[] = {"Off", "2x", "4x", "8x", "16x"};
                const uint32_t now = gsHwAnisotropy();
                int i = now >= 16u ? 4 : now >= 8u ? 3 : now >= 4u ? 2 : now >= 2u ? 1 : 0;
                if (ImGui::Combo("Anisotropic filtering", &i, names, 5))
                    gsHwSetAnisotropy(values[i]);
                ImGui::TextDisabled("  switching re-creates every texture (short hitch)");
                // which textured draws took it over the last second (gsHwAnisoStats): why AF may show no effect
                static uint64_t last[6] = {}, delta[6] = {};
                static auto at = std::chrono::steady_clock::now();
                if (std::chrono::steady_clock::now() - at > std::chrono::seconds(1))
                {
                    uint64_t now6[6];
                    gsHwAnisoStats(now6);
                    for (int k = 0; k < 6; ++k)
                    {
                        delta[k] = now6[k] - last[k];
                        last[k] = now6[k];
                    }
                    at = std::chrono::steady_clock::now();
                }
                uint64_t total = 0;
                for (uint64_t v : delta)
                    total += v;
                if (total)
                {
                    auto pc = [&](int k) { return 100.0 * static_cast<double>(delta[k]) / static_cast<double>(total); };
                    ImGui::TextDisabled("  textured draws/s %llu: AF %.0f%%, nearest %.0f%%, region wrap %.0f%%,",
                                        (unsigned long long)total, pc(0), pc(1), pc(2));
                    ImGui::TextDisabled("  no mips %.0f%%, native buffer %.0f%%, other %.0f%%", pc(3), pc(4), pc(5));
                }
            }

            ImGui::SeparatorText("Frame rate");
            {
                int i = lcs_bootstrap::frameRateModeIndex();
                const char *names[] = {"30 FPS (PS2)", "60 FPS", "120 FPS", "Uncapped"};
                if (ImGui::Combo("FPS mode (F6)", &i, names, 4))
                    lcs_bootstrap::setFrameRateMode(i);
            }

            ImGui::SeparatorText("World");
            if (lcs_world::installed())
            {
                float dd = lcs_world::drawDistance() * 100.0f;
                if (ImGui::SliderFloat("LOD distance", &dd, 50.0f, 300.0f, "%.0f %%"))
                    lcs_world::setDrawDistance(dd / 100.0f);
                ImGui::SameLine();
                if (ImGui::SmallButton("PS2"))
                    lcs_world::setDrawDistance(1.0f);
                if (lcs_world::worldDistanceInstalled())
                {
                    float wd = lcs_world::worldDistance() * 100.0f;
                    if (ImGui::SliderFloat("World distance", &wd, 50.0f, 300.0f, "%.0f %%"))
                        lcs_world::setWorldDistance(wd / 100.0f);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("PS2##world"))
                        lcs_world::setWorldDistance(1.0f);
                    ImGui::TextDisabled("  far plane %.0f m (timecycle %.0f m), fog from %.0f m: buildings, roads, props",
                                        lcs_world::appliedFar(), lcs_world::timecycleFar(),
                                        lcs_world::timecycleFog() * lcs_world::worldDistance());
                }
                ImGui::TextDisabled("  camera: LOD x%.2f, spawn x%.2f; peds, cars; the pause menu sets both, 50-250 %%",
                                    lcs_world::lodMultiplier(), lcs_world::generationMultiplier());
                for (int h = 0; h < 2; ++h)
                {
                    uint32_t used = 0, size = 0;
                    if (lcs_world::heapUse(h, used, size))
                        ImGui::TextDisabled("  %s heap: %.1f of %.1f MB used (%.2f MB free)", h == 0 ? "Main" : "Streaming",
                                            used / 1048576.0, size / 1048576.0,
                                            (size > used ? size - used : 0u) / 1048576.0);
                }
            }
            else
                ImGui::TextDisabled("Draw distance: not installed (LCS_WORLD_DETAIL=0)");

            ImGui::SeparatorText("Effects");
            {
                bool trails = gsHwTrailsEnabled();
                if (ImGui::Checkbox("Trails (F7)", &trails))
                    gsHwSetTrailsEnabled(trails);
                ImGui::BeginDisabled(!trails);
                float strength = gsHwTrailsStrength() * 100.0f;
                if (ImGui::SliderFloat("Trails strength", &strength, 0.0f, 200.0f, "%.0f %%"))
                    gsHwSetTrailsStrength(strength / 100.0f);
                ImGui::TextDisabled("  blends a copy of the frame back: only visible with smear");
                float tint = gsHwTrailsTint() * 100.0f;
                if (ImGui::SliderFloat("Colour tint", &tint, 0.0f, 200.0f, "%.0f %%"))
                    gsHwSetTrailsTint(tint / 100.0f);
                const float smear = gsHwTrailsSmear();
                bool autoSmear = smear < 0.0f;
                if (ImGui::Checkbox("Smear by resolution", &autoSmear))
                    gsHwSetTrailsSmear(autoSmear ? -1.0f : 1.0f);
                if (!autoSmear)
                {
                    float s = smear * 100.0f;
                    if (ImGui::SliderFloat("Smear (100 % = PS2)", &s, 0.0f, 100.0f, "%.0f %%"))
                        gsHwSetTrailsSmear(s / 100.0f);
                }
                if (ImGui::Button("Trails: game defaults"))
                {
                    gsHwSetTrailsStrength(1.0f);
                    gsHwSetTrailsTint(1.0f);
                    gsHwSetTrailsSmear(-1.0f);
                }
                ImGui::EndDisabled();
            }
            {
                bool glow = gsHwGlowEnabled();
                if (ImGui::Checkbox("Glow", &glow))
                    gsHwSetGlowEnabled(glow);
            }
            {
                const char *names[] = {"As on PS2 (flickers)", "Smooth", "Smooth above 1x"};
                int i = g_lcsReflectionFilter;
                if (ImGui::Combo("Car reflections", &i, names, 3))
                {
                    g_lcsReflectionFilter = i;
                    gsHwSetTargetFilter(420u, 2u, i);
                }
            }

            ImGui::SeparatorText("Developer");
            if (lcs_freecam::installed())
            {
                bool fly = lcs_freecam::enabled();
                if (ImGui::Checkbox("Free camera (F8)", &fly))
                    lcs_freecam::setEnabled(fly);
                float speed = lcs_freecam::speed();
                if (ImGui::SliderFloat("Free camera speed", &speed, 1.0f, 500.0f, "%.0f m/s", ImGuiSliderFlags_Logarithmic))
                    lcs_freecam::setSpeed(speed);
                ImGui::TextDisabled("  WASD move, E/Space up, Q/C down, mouse or arrows look, Shift x4, Ctrl x0.25,");
                ImGui::TextDisabled("  wheel speed; pad: sticks, R2/L2 up/down, R1/L1 fast/slow. Close F2 for the mouse.");
            }
            else
                ImGui::TextDisabled("Free camera: not installed (LCS_FREECAM=0)");

            ImGui::SeparatorText("HUD");
            {
                bool shown = !gsHwHudHidden();
                if (ImGui::Checkbox("Show HUD (F9)", &shown))
                    gsHwSetHudHidden(!shown);
                ImGui::BeginDisabled(!g_display.wide);
                int m = g_display.hudMode;
                const char *names[] = {"Squeezed", "Screen edges", "Stretched"};
                if (ImGui::Combo("HUD layout (16:9)", &m, names, 3))
                {
                    g_display.hudMode = m;
                    applyDisplay();
                }
                if (g_display.hudMode == 0)
                {
                    float s = g_display.squeeze * 100.0f;
                    if (ImGui::SliderFloat("Squeeze (75 % = 4:3 shape)", &s, 75.0f, 100.0f, "%.1f %%"))
                    {
                        g_display.squeeze = s / 100.0f;
                        applyDisplay();
                    }
                }
                ImGui::EndDisabled();
            }
            ImGui::PopItemWidth();
            ImGui::Separator();
            ImGui::TextDisabled("Live, not saved here (the pause menu keeps its settings in lcs.ini).");
        }
        ImGui::End();
        ImGui::PopFont();
    }

    void setDefaultEnv(const char *name, const char *value)
    {
        if (const char *v = std::getenv(name); v && *v)
            return;
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
}

// [lcs] stderr off the game thread's critical path. run_153057 (-ProfileHitches): in the 120-500 ms hitches while
// driving the game thread spent about half its samples in NtWriteFile - every log line ([lcs-clump], the IOP's
// "Unknown device 'cdrom0'", ...) written to the redirected stderr.log at once (stderr unbuffered, std::cerr unit-
// buffered, std::endl a flush), a few ms each while streaming. Now: stderr fully buffered (1 MB), std::cerr into it
// without its flushes, and a thread writing the buffer out every 200 ms (and at exit). LCS_STDERR_SYNC=1: as before.
namespace
{
    class DeferredStderrBuf : public std::streambuf
    {
    protected:
        int_type overflow(int_type ch) override
        {
            if (ch == traits_type::eof())
                return traits_type::not_eof(ch);
            const char c = traits_type::to_char_type(ch);
            return std::fwrite(&c, 1, 1, stderr) == 1 ? ch : traits_type::eof();
        }
        std::streamsize xsputn(const char *data, std::streamsize n) override
        {
            return static_cast<std::streamsize>(std::fwrite(data, 1, static_cast<size_t>(n), stderr));
        }
        int sync() override { return 0; } // (std::endl / std::flush: the flusher thread writes it out)
    };

    void deferStderr()
    {
        const char *sync = std::getenv("LCS_STDERR_SYNC");
        if (sync && *sync == '1')
            return;
        static std::vector<char> buffer(1u << 20);
        std::setvbuf(stderr, buffer.data(), _IOFBF, buffer.size());
        static DeferredStderrBuf deferred;
        std::cerr.rdbuf(&deferred);
        std::cerr.unsetf(std::ios::unitbuf);
        std::atexit([] { std::fflush(stderr); });
        std::set_terminate([] {
            std::fflush(stderr);
            std::abort();
        });
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                std::fflush(stderr);
            }
        }).detach();
    }
}

// [lcs] how frames reach the display. G-Sync / FreeSync (the user's 180 Hz and 280 Hz monitors) only follow the game's
// frame times when a flip-model swap chain presents with DXGI_PRESENT_ALLOW_TEARING; Present(0, 0) without it (the
// old default, "immediate") lets the compositor place frames on the refresh grid - 60 fps on 180 Hz alternates 3/3,
// on 280 Hz 4/5 refreshes, uneven. LCS_PRESENT=tearing (default; plain immediate where tearing is not supported) |
// immediate | vsync.
void choosePresentMode()
{
    const char *e = std::getenv("LCS_PRESENT");
    const std::string_view v = e ? e : "tearing";
    const int mode = v == "vsync" ? 1 : v == "immediate" ? 0 : 2;
    present::setPresentMode(mode);
    g_lcsPresentMode = mode; // [lcs] the video page shows / changes it
    std::fprintf(stderr, "[present] mode %s (LCS_PRESENT)\n", mode == 1 ? "vsync" : mode == 0 ? "immediate" : "tearing (VRR)");
}

// [lcs] The release start (LCSrecomp.exe double-clicked, or started by a launcher with the flags below): everything
// in the game folder - the exe's folder, or --game-dir - the player's ISO as gamedata\LCS.iso (the only game data;
// see mountIso), the memory card in Savegame\, the log in logs\, cache\, lcs.ini - and the start script's settings as
// defaults (Classic controls, DirectInput pads, the RSL replay, the compiled VU1, the render thread; any already set
// in the environment win, a flag wins over both). Only a folder as the argument (an extracted disc, development)
// skips it.
//
//   LCSrecomp.exe [--iso <file>] [--fullscreen | --windowed] [--save-dir <folder>] [--game-dir <folder>] [<file>.iso]
//
// The flags are the existing switches by name: --iso = LCS_ISO, --fullscreen / --windowed = LCS_FULLSCREEN 1 / 0,
// --save-dir = LCS_SAVE_DIR; --game-dir is the folder the game works in (default: the exe's). Paths are taken
// relative to the folder the exe was started from.
namespace
{
    struct StartOptions
    {
        std::string iso, saveDir, gameDir, devFolder, error;
        int fullscreen = -1; // -1: not given
        bool help = false;
    };

    StartOptions parseArgs(int argc, char *argv[])
    {
        StartOptions o;
        auto abs = [](const char *p) { return std::filesystem::absolute(std::filesystem::path(p)).string(); };
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto value = [&](std::string &into) {
                if (i + 1 >= argc || std::string_view(argv[i + 1]).rfind("--", 0) == 0)
                    o.error = a + " needs a value";
                else
                    into = abs(argv[++i]);
            };
            if (a == "--help" || a == "-h" || a == "/?")
                o.help = true;
            else if (a == "--iso")
                value(o.iso);
            else if (a == "--save-dir")
                value(o.saveDir);
            else if (a == "--game-dir")
                value(o.gameDir);
            else if (a == "--fullscreen")
                o.fullscreen = 1;
            else if (a == "--windowed")
                o.fullscreen = 0;
            else if (a.rfind("--", 0) == 0)
                o.error = "unknown option " + a;
            else if (a.size() > 4 && (a.substr(a.size() - 4) == ".iso" || a.substr(a.size() - 4) == ".ISO"))
                o.iso = abs(a.c_str());
            else
                o.devFolder = abs(a.c_str()); // the extracted disc's folder (development)
            if (!o.error.empty())
                break;
        }
        return o;
    }

    void printUsage(FILE *to)
    {
        std::fprintf(to,
                     "LCSrecomp.exe [options] [<file>.iso]\n"
                     "  --iso <file>        the Liberty City Stories (USA) ISO (default: gamedata\\LCS.iso in the game folder)\n"
                     "  --fullscreen        start in fullscreen (the default)\n"
                     "  --windowed          start in a window\n"
                     "  --save-dir <folder> the savegame folder (default: Savegame in the game folder)\n"
                     "  --game-dir <folder> the folder the game works in: logs, cache, lcs.ini (default: the exe's folder)\n"
                     "  --help              this text\n");
    }

    extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void *, wchar_t *, unsigned long);
    extern "C" __declspec(dllimport) int __stdcall MessageBoxW(void *, const wchar_t *, const wchar_t *, unsigned int);
    void releaseStart(const StartOptions &o)
    {
        if (!o.devFolder.empty())
            return;
        wchar_t buf[2048] = {};
        const unsigned long n = GetModuleFileNameW(nullptr, buf, 2048);
        const std::filesystem::path dir = o.gameDir.empty() ? std::filesystem::path(std::wstring(buf, n)).parent_path()
                                                            : std::filesystem::path(o.gameDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::current_path(dir, ec);
        std::filesystem::create_directories(dir / "logs", ec);
        // the savegames: Savegame\ in the game folder (English folder names, the user's ask 2026-11-18); an older
        // saves\mc0 is moved there once
        if (!std::filesystem::exists(dir / "Savegame", ec) && std::filesystem::is_directory(dir / "saves" / "mc0", ec))
        {
            std::filesystem::rename(dir / "saves" / "mc0", dir / "Savegame", ec);
            std::filesystem::remove(dir / "saves", ec); // only when empty
        }
        if (o.saveDir.empty())
            std::filesystem::create_directories(dir / "Savegame", ec);
        const std::string log = (dir / "logs" / "last_run.log").string();
        // the run before stays as previous_run.log (a crash's log survives one restart)
        std::filesystem::rename(dir / "logs" / "last_run.log", dir / "logs" / "previous_run.log", ec);
        if (!std::freopen(log.c_str(), "w", stderr))
            std::fprintf(stdout, "[lcs] log %s not writable\n", log.c_str());
        else
        {
            // a size limit (the user's ask 2026-11-18: a short session wrote ~100 MB): past LCS_LOG_LIMIT_MB (default
            // 32, 0 = none) one line says so and the rest of the run's log goes nowhere
            long long limitMb = 32;
            if (const char *e = std::getenv("LCS_LOG_LIMIT_MB"); e && *e)
                limitMb = std::atoll(e);
            if (limitMb > 0)
                std::thread([log, limit = static_cast<std::uintmax_t>(limitMb) << 20] {
                    for (;;)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        std::error_code sizeErr;
                        if (std::filesystem::file_size(log, sizeErr) < limit || sizeErr)
                            continue;
                        std::fprintf(stderr, "\n[lcs] the log reached its limit (%llu MB, LCS_LOG_LIMIT_MB): nothing more is "
                                             "written this run\n", static_cast<unsigned long long>(limit >> 20));
                        std::fflush(stderr);
                        std::freopen("NUL", "w", stderr);
                        return;
                    }
                }).detach();
        }
        const std::string saves = (dir / "Savegame").string(), iso = (dir / "gamedata" / "LCS.iso").string();
        const char *defaults[][2] = {
            {"LCS_SAVE_DIR", saves.c_str()}, {"LCS_ISO", iso.c_str()}, {"PS2X_HW_SCALE", "0"}, {"PS2X_HW_MSAA", "8"},
            {"PS2X_HW_ANISO", "1"}, {"LCS_TRAILS", "1"}, {"LCS_RSL", "replay"}, {"LCS_CONTROLLER_PROFILE", "classic"},
            {"SDL_JOYSTICK_DIRECTINPUT", "1"}, {"SDL_JOYSTICK_WGI", "0"},
            {"SDL_JOYSTICK_GAMEINPUT", "0"}, {"SDL_JOYSTICK_RAWINPUT", "0"},
            {"PS2X_VU1_COMPILED", "1"}, {"LCS_INDEPENDENT_TIMING", "0"}, {"LCS_FULLSCREEN", "1"},
            {"PS2X_RENDER_THREAD", "1"}, {"LCS_CTIMER_HOST", "1"}, {"PS2X_DMA_MAX_TAGS", "1048576"}};
        // the flags first: they win over the environment and the defaults
        _putenv_s("LCS_GAME_DIR", dir.string().c_str()); // lcs.ini (lcs_menu.cpp) goes here
        if (!o.iso.empty())
            _putenv_s("LCS_ISO", o.iso.c_str());
        if (!o.saveDir.empty())
            _putenv_s("LCS_SAVE_DIR", o.saveDir.c_str());
        if (o.fullscreen >= 0)
            _putenv_s("LCS_FULLSCREEN", o.fullscreen ? "1" : "0");
        for (const auto &d : defaults)
            if (!std::getenv(d[0]))
                _putenv_s(d[0], d[1]);
        std::fprintf(stderr, "[lcs] release start in %s (ISO %s, saves %s, fullscreen %s)\n", dir.string().c_str(),
                     std::getenv("LCS_ISO"), std::getenv("LCS_SAVE_DIR"), std::getenv("LCS_FULLSCREEN"));
    }
}

// [lcs] The ISO alone (user's ask 2026-11-17: "entweder nur .iso oder nur entpackt"), as the reference port's main does: the image is
// mapped as the disc (ps2_disc::setImage) - every cdrom0: file and sector read goes to it - and only what the runtime
// loads by path is taken out of it into cache\ once: the ELF (SLUS_214.23) and the IOP modules of MODULES\IOPRP300.IMG
// (its ROMDIR; lcs_bootstrap loads them from LCS_IRX_DIR). Used for the release start (no arguments: gamedata\LCS.iso
// or LCS_ISO) and for an .iso given as the argument; a folder's ELF as the argument is the old extracted-disc mode.
namespace
{
    class MappedDisc final : public IPS2DiscImage
    {
    public:
        explicit MappedDisc(const lcs::IsoImage &iso) : m_iso(iso) {}
        bool findFile(std::string_view ps2Path, uint32_t &lsn, uint32_t &sizeBytes, bool &isDirectory) const override
        {
            lcs::IsoEntry entry;
            if (!m_iso.find(ps2Path, entry))
                return false;
            lsn = entry.lba;
            sizeBytes = entry.size;
            isDirectory = entry.isDirectory;
            return true;
        }
        bool readBytes(uint64_t offset, size_t byteCount, void *destination) const override
        {
            return m_iso.readBytes(offset, byteCount, destination);
        }
        uint32_t totalSectors() const override { return m_iso.totalSectors(); }
        bool listDirectory(std::string_view ps2Path, std::vector<DirEntry> &out) const override
        {
            const lcs::IsoImage::Children *kids = m_iso.children(ps2Path);
            if (!kids)
                return false;
            out.clear();
            for (const auto &[name, entry] : *kids)
                out.push_back(DirEntry{name, entry.lba, entry.size, entry.isDirectory});
            return true;
        }

    private:
        const lcs::IsoImage &m_iso;
    };

    lcs::IsoImage g_iso;
    std::unique_ptr<MappedDisc> g_disc;
    std::string g_isoPath; // set in ISO mode: the image for sector reads too

    bool writeIfChanged(const std::filesystem::path &out, const uint8_t *data, size_t size)
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && std::filesystem::file_size(out, ec) == size)
        {
            std::ifstream in(out, std::ios::binary);
            std::vector<char> have(size);
            if (in.read(have.data(), static_cast<std::streamsize>(size)) && std::memcmp(have.data(), data, size) == 0)
                return true;
        }
        std::filesystem::create_directories(out.parent_path(), ec);
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
        return file.good();
    }

    // returns the staged ELF's path, or empty with the reason on stderr
    std::filesystem::path mountIso(const std::string &isoPath)
    {
        if (!g_iso.open(isoPath))
        {
            std::cerr << "[lcs] cannot read the ISO " << isoPath << ": " << g_iso.error() << std::endl;
            return {};
        }
        lcs::IsoEntry elf;
        if (!g_iso.find("SLUS_214.23", elf) || elf.isDirectory)
        {
            std::cerr << "[lcs] SLUS_214.23 not on " << isoPath << ": not Liberty City Stories (USA)" << std::endl;
            return {};
        }
        const std::filesystem::path cache = std::filesystem::current_path() / "cache";
        std::vector<uint8_t> bytes(elf.size);
        g_iso.readBytes(uint64_t(elf.lba) * lcs::IsoImage::kSectorSize, bytes.size(), bytes.data());
        const std::filesystem::path elfPath = cache / "SLUS_214.23";
        if (!writeIfChanged(elfPath, bytes.data(), bytes.size()))
        {
            std::cerr << "[lcs] cannot write " << elfPath.string() << std::endl;
            return {};
        }
        // the IOP modules: IOPRP300.IMG's ROMDIR (16-byte entries: name[10], ext info u16, size u32), the files
        // after each other, 16-byte aligned, starting with RESET at 0
        lcs::IsoEntry img;
        int irx = 0;
        if (g_iso.find("MODULES/IOPRP300.IMG", img) && !img.isDirectory)
        {
            std::vector<uint8_t> rp(img.size);
            g_iso.readBytes(uint64_t(img.lba) * lcs::IsoImage::kSectorSize, rp.size(), rp.data());
            size_t dir = 0;
            while (dir + 6 <= rp.size() && std::memcmp(rp.data() + dir, "RESET\0", 6) != 0)
                dir += 16;
            uint64_t at = 0;
            for (size_t e = dir; e + 16 <= rp.size(); e += 16)
            {
                char name[11] = {};
                std::memcpy(name, rp.data() + e, 10);
                if (!name[0])
                    break;
                uint32_t size;
                std::memcpy(&size, rp.data() + e + 12, 4);
                const std::string n(name);
                if (n != "RESET" && n != "ROMDIR" && n != "EXTINFO" && at + size <= rp.size() &&
                    writeIfChanged(cache / "ioprp300" / (n + ".irx"), rp.data() + at, size))
                    ++irx;
                at += (size + 15u) & ~uint64_t(15);
            }
        }
        if (irx == 0)
        {
            std::cerr << "[lcs] no IOP modules in MODULES\\IOPRP300.IMG on " << isoPath << std::endl;
            return {};
        }
        _putenv_s("LCS_IRX_DIR", (cache / "ioprp300").string().c_str());
        g_disc = std::make_unique<MappedDisc>(g_iso);
        ps2_disc::setImage(g_disc.get());
        g_isoPath = isoPath;
        std::cerr << "[lcs] disc: the ISO " << isoPath << " (ELF and " << irx << " IOP modules in cache\\)" << std::endl;
        return elfPath;
    }
}

int main(int argc, char *argv[])
{
    const StartOptions opts = parseArgs(argc, argv);
    if (opts.help || !opts.error.empty())
    {
        if (!opts.error.empty())
            std::fprintf(stderr, "LCSrecomp.exe: %s\n", opts.error.c_str());
        printUsage(opts.error.empty() ? stdout : stderr);
        return opts.error.empty() ? 0 : 1;
    }
    releaseStart(opts);
    deferStderr();
    choosePresentMode();
    try
    {
        setDefaultEnv("PS2X_GS", "hw");   // the D3D11 hardware GS
        setDefaultEnv("PS2X_IOP", "1");   // LCS boots with the emulated IOP (no native sound / CDSTREAM layer yet)
        std::filesystem::path elfPath;
        if (opts.devFolder.empty())
        {
            const char *env = std::getenv("LCS_ISO");
            const std::string iso = env && *env ? std::string(env) : (std::filesystem::current_path() / "gamedata" / "LCS.iso").string();
            elfPath = mountIso(iso);
            if (elfPath.empty())
            {
                // not silently gone (the user's test 2026-11-18: the exe started from another folder closed at once):
                // the reason in a window, as in the log
                std::fflush(stderr);
                const bool missing = !std::filesystem::is_regular_file(std::filesystem::path(iso));
                const std::string text =
                    (missing ? "LCS Recomp can't start: the Liberty City Stories (USA) disc image was not found.\n\n"
                             : "LCS Recomp can't start: this disc image is not Liberty City Stories (USA) (SLUS-21423).\n\n") +
                    std::string("Looked for: ") + iso +
                    "\n\nStart the game from the LCS Recomp launcher, or put your ISO at gamedata\\LCS.iso next to "
                    "LCSrecomp.exe, or start it with --iso <file>.";
                const std::wstring wide = std::filesystem::path(text).wstring();
                MessageBoxW(nullptr, wide.c_str(), L"LCS Recomp", 0x00000010L /* MB_ICONERROR */);
                return 2;
            }
        }
        else
            elfPath = opts.devFolder; // the extracted disc's folder (development)

        PS2Runtime runtime;
        runtime.setDebugUiCallbacks([](PS2Runtime &, void *) {}, drawLcsDebugOverlay, nullptr, nullptr);
        if (const char *fullscreen = std::getenv("LCS_FULLSCREEN"); fullscreen && fullscreen[0] == '1')
            platform::setFullscreen(true);
        if (!runtime.initialize("GTA: Liberty City Stories"))
        {
            std::cerr << "Failed to initialize runtime" << std::endl;
            return 3;
        }
        lcs_bootstrap::setNativeBootMovie(true); // boot movies through lcs_movie (src/lcs/lcs_movie.cpp, FFmpeg)
        lcs_bootstrap::setEnabled(true); // LCS's boot overrides (support heap, syscalls, IOP heap, traces) at loadELF
        if (!runtime.loadELF(elfPath.string()))
        {
            std::cerr << "Failed to load " << elfPath.string() << std::endl;
            return 4;
        }
        { // the original LCS ISO for sector reads (movies, CDSTREAM): config/local_cd_image.txt as the LCS probe did,
          // else LCS_ISO, else gamedata\LCS.iso in the working folder
            auto paths = PS2Runtime::getIoPaths();
            // [lcs] the memory card in one place for every run (each run has its own folder: saves were lost):
            // LCS_SAVE_DIR (the release start and the start scripts set it), default Savegame in the working folder
            std::filesystem::path saveDir = std::filesystem::current_path() / "Savegame";
            if (const char *e = std::getenv("LCS_SAVE_DIR"); e && *e)
                saveDir = e;
            std::error_code mkErr;
            std::filesystem::create_directories(saveDir, mkErr);
            paths.mcRoot = saveDir;
            std::cerr << "[lcs] memory card: " << saveDir.string() << std::endl;
            std::string imagePath;
            if (!g_isoPath.empty())
                imagePath = g_isoPath; // ISO mode: the mapped disc
            else if (std::ifstream imageConfig("config/local_cd_image.txt"); imageConfig)
                std::getline(imageConfig, imagePath);
            while (!imagePath.empty() && (imagePath.back() == '\r' || imagePath.back() == ' '))
                imagePath.pop_back();
            if (imagePath.empty())
                if (const char *e = std::getenv("LCS_ISO"); e && *e)
                    imagePath = e;
            if (imagePath.empty())
                imagePath = (std::filesystem::current_path() / "gamedata" / "LCS.iso").string();
            if (!std::filesystem::is_regular_file(imagePath))
                throw std::runtime_error("LCS ISO not found: " + imagePath);
            paths.cdImage = std::filesystem::path(imagePath);
            std::cerr << "[lcs] original ISO: " << imagePath << std::endl;
            PS2Runtime::setIoPaths(paths);
        }
        lcs_sound::install(runtime); // [lcs-sound] effects and music natively (src/lcs/lcs_sound.cpp); needs the ISO path
        { // [lcs] LCS's render targets by role - the same VRAM layout as the reference port (run_202321 RT-read log: frame FBP 140, shown
          // FBP 0, the cars' reflection map FBP 420 fbw 2 = 128x128, trails FBP 384 fbw 8), registered as the the reference port display
          // layer does (ref_display.cpp installDisplayHooks): the reflection map takes the frame's scale instead of 1x,
          // read through the renderer's filter. LCS_REFLECTION_FILTER=0/1/2.
            gsHwSetTargetRole(140u, 10u, "frame", false);
            gsHwSetTargetRole(0u, 10u, "shown picture", false);
            gsHwSetTargetRole(420u, 2u, "reflection map (128x128 sample of the frame)", true);
            gsHwSetTargetRole(384u, 8u, "trails", false);
            int filter = kDefaultReflectionFilter;
            if (const char *e = std::getenv("LCS_REFLECTION_FILTER"); e && *e >= '0' && *e <= '2')
                filter = *e - '0';
            gsHwSetTargetFilter(420u, 2u, filter);
            g_lcsReflectionFilter = filter; // [lcs] the video page shows / changes it
            std::cerr << "[lcs] render target roles set; reflection map at the frame's scale, filter " << filter << std::endl;
        }
        ps2SetPresentOnGameFrames(true);
        lcs::startProfiler(runtime);
        runtime.run();
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
