// [lcs-freecam] A free camera for looking around the world (dev tool) - the port of the the reference port project's
// src/game/ref_freecam.cpp to LCS.
//
// LCS's camera, found by matching the the reference port functions instruction by instruction (2026-11-16):
// - CCamera::Process = 0x28FAC8 (this = TheCamera 0x43BBF0, called from 0x2FBAF0 / 0x28D038); it updates the active
//   CCam with CCam::Process 0x27A380 (the reference port 0x294540, 82 %) and then calls CCam::ProcessSpecialHeightRoutines 0x287020
//   (the reference port 0x2A2ED0, 91 %) for it: a0 = TheCamera + 0x220 + TheCamera[0x10C] * 0x2B0 (the reference port: + 0x70 + [0x50] * 0x270).
// - CCam fields, from the follow-ped camera 0x281E90 (the reference port 0x29DAB0, 85 %) and the height routine: mode +0x40 (the reference port +0),
//   Front +0x1C0 (+0x10), Source +0x1D0 (+0x20), Up +0x1F0 (+0x60), alpha / beta +0xE0 / +0xF0 (+0x78 / +0x7C),
//   FOV +0xE8 (+0x134), the target point +0x160 (+0x1A0), the target entity +0x274 (+0x70).
//
// While flying the height routine is replaced: the game's camera update has run (a cutscene's camera plays its path),
// then the free camera writes its pose into the CCam, which CCamera::Process turns into the view as usual. The game
// reads a neutral pad meanwhile (Pad.cpp padSetBlocked): keyboard, mouse and gamepad belong to the camera.
//
// Controls: F8 on / off (or the F2 video page). Keyboard: W / S forward / back, A / D sideways, E or Space up, Q or C
// down, arrow keys turn, Left Shift x4, Left Ctrl x0.25, mouse look (captured while flying), mouse wheel the speed.
// Gamepad: left stick move, right stick look, R2 up, L2 down, R1 x4, L1 x0.25. Real time: neither the frame rate nor
// the game's speed changes how fast it flies. LCS_FREECAM=0: not installed.
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "platform/platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2_stubs { void padSetBlocked(bool on); } // Kernel/Stubs/Pad.cpp
namespace lcs_input { void afterCameraUpdate(uint8_t *ram, uint32_t cam); } // the mouse camera's held heading

namespace lcs_freecam
{
    namespace
    {
        constexpr uint32_t kRamMask = PS2_RAM_MASK;
        constexpr uint32_t kTheCamera = 0x0043BBF0u;
        constexpr uint32_t kActiveCam = kTheCamera + 0x10Cu; // byte: which of Cams[3] is active
        constexpr uint32_t kCams = kTheCamera + 0x220u, kCamSize = 0x2B0u;
        constexpr uint32_t kCamFront = 0x1C0u, kCamSource = 0x1D0u, kCamUp = 0x1F0u, kCamFov = 0xE8u, kCamTarget = 0x160u;
        constexpr uint32_t kCamHeightRoutines = 0x00287020u;
        // [lcs-freecam] Tony can't die while the camera flies (user request): CWorld::Players[PlayerInFocus].m_pPed -
        // LCS's FindPlayerPed 0x107368 (the reference port 0x24A430, the same code): Players 0x408E40 (0x190 each), PlayerInFocus byte
        // 0x3D9BC8 (lui 0x3E, -0x6438; 0x3E9BC8 before 2026-11-16 was wrong); CPed::m_fHealth +0x4B8 (the reference port +0x4E4: read / written as often, set to 100.0f by four functions).
        // Held at 1000 while flying (one explosion's damage is less), the health from before back afterwards.
        // LCS_FREECAM_GOD=0: off.
        constexpr uint32_t kPlayers = 0x00408E40u, kPlayerSize = 0x190u, kPlayerInFocus = 0x003D9BC8u, kPedHealth = 0x4B8u;
        const bool g_god = !(std::getenv("LCS_FREECAM_GOD") && std::getenv("LCS_FREECAM_GOD")[0] == '0');
        float g_savedHealth = -1.0f; // the player's health when the free camera started (< 0: none held)
        uint32_t g_godPed = 0;

        uint32_t playerPed(const uint8_t *ram)
        {
            uint32_t ped;
            std::memcpy(&ped, ram + ((kPlayers + ram[kPlayerInFocus] * kPlayerSize) & kRamMask), 4);
            return ped >= 0x00100000u && ped < PS2_RAM_SIZE ? ped : 0u;
        }

        constexpr float kTurnRate = 2.5f;     // radians a second, stick / arrow keys at full tilt
        constexpr float kMouseTurn = 0.0025f; // radians a mouse count
        constexpr float kPitchLimit = 1.55f;

        PS2Runtime::RecompiledFunction g_origHeight = nullptr;
        std::atomic<bool> g_installed{false}, g_wanted{false};
        std::atomic<float> g_speed{20.0f}; // metres a second

        // game thread
        bool g_flying = false;
        float g_x = 0, g_y = 0, g_z = 0, g_yaw = 0, g_pitch = 0, g_fov = 70;
        std::chrono::steady_clock::time_point g_last{};

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & kRamMask), &v, 4); }
        void wrVec(uint8_t *ram, uint32_t a, float x, float y, float z)
        {
            wrf(ram, a, x);
            wrf(ram, a + 4, y);
            wrf(ram, a + 8, z);
        }

        bool activeCam(const uint8_t *ram, uint32_t cam) { return cam == kCams + ram[kActiveCam] * kCamSize; }

        // the free camera where the game's camera was: no jump when it starts
        void begin(const uint8_t *ram, uint32_t cam)
        {
            g_x = rdf(ram, cam + kCamSource);
            g_y = rdf(ram, cam + kCamSource + 4);
            g_z = rdf(ram, cam + kCamSource + 8);
            const float fx = rdf(ram, cam + kCamFront), fy = rdf(ram, cam + kCamFront + 4), fz = rdf(ram, cam + kCamFront + 8);
            g_yaw = std::atan2(fy, fx);
            g_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kPitchLimit, kPitchLimit);
            const float fov = rdf(ram, cam + kCamFov);
            g_fov = fov >= 10.0f && fov <= 130.0f ? fov : 70.0f;
            g_last = std::chrono::steady_clock::now();
            g_flying = true;
            if (const uint32_t ped = g_god ? playerPed(ram) : 0u)
            {
                g_godPed = ped;
                g_savedHealth = rdf(ram, ped + kPedHealth);
            }
            std::fprintf(stderr, "[freecam] on at (%.1f, %.1f, %.1f), FOV %.0f\n", g_x, g_y, g_z, g_fov);
        }

        void end(uint8_t *ram)
        {
            if (!g_flying)
                return;
            g_flying = false;
            if (g_savedHealth >= 0.0f && g_godPed && playerPed(ram) == g_godPed)
                wrf(ram, g_godPed + kPedHealth, std::max(g_savedHealth, 1.0f));
            g_savedHealth = -1.0f;
            g_godPed = 0;
            std::fprintf(stderr, "[freecam] off at (%.1f, %.1f, %.1f)\n", g_x, g_y, g_z);
        }

        float axis(platform::GamepadAxis a)
        {
            const float v = platform::gamepadConnected() ? platform::gamepadAxis(a) : 0.0f;
            return std::fabs(v) < 0.15f ? 0.0f : v;
        }
        float key(int k) { return platform::keyDown(k) ? 1.0f : 0.0f; }

        // one camera update (game thread): the controls in real time, then the pose into the CCam
        void update(uint8_t *ram, uint32_t cam)
        {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - g_last).count(), 0.0f, 0.1f);
            g_last = now;

            if (g_god)
                if (const uint32_t ped = playerPed(ram); ped && ped == g_godPed && rdf(ram, ped + kPedHealth) > 0.0f)
                    wrf(ram, ped + kPedHealth, 1000.0f);

            float mdx = 0.0f, mdy = 0.0f;
            if (platform::mouseCaptured())
                platform::takeMouseMotion(mdx, mdy);
            const float turn = key(platform::KEY_RIGHT) - key(platform::KEY_LEFT) + axis(platform::AXIS_RIGHT_X);
            const float tilt = key(platform::KEY_UP) - key(platform::KEY_DOWN) - axis(platform::AXIS_RIGHT_Y);
            g_yaw = std::remainder(g_yaw - turn * kTurnRate * dt - mdx * kMouseTurn, 6.2831855f);
            g_pitch = std::clamp(g_pitch + tilt * kTurnRate * dt - mdy * kMouseTurn, -kPitchLimit, kPitchLimit);

            const float forward = std::clamp(key(platform::KEY_W) - key(platform::KEY_S) - axis(platform::AXIS_LEFT_Y), -1.0f, 1.0f);
            const float strafe = std::clamp(key(platform::KEY_D) - key(platform::KEY_A) + axis(platform::AXIS_LEFT_X), -1.0f, 1.0f);
            const float rise = std::clamp(std::max(key(platform::KEY_E), key(platform::KEY_SPACE)) -
                                              std::max(key(platform::KEY_Q), key(platform::KEY_C)) +
                                              axis(platform::AXIS_RIGHT_TRIGGER) - axis(platform::AXIS_LEFT_TRIGGER),
                                          -1.0f, 1.0f);
            const bool fast = platform::keyDown(platform::KEY_LEFT_SHIFT) ||
                              (platform::gamepadConnected() && platform::gamepadButton(platform::GAMEPAD_RIGHT_SHOULDER));
            const bool slow = platform::keyDown(platform::KEY_LEFT_CONTROL) ||
                              (platform::gamepadConnected() && platform::gamepadButton(platform::GAMEPAD_LEFT_SHOULDER));

            const float cy = std::cos(g_yaw), sy = std::sin(g_yaw), cp = std::cos(g_pitch), sp = std::sin(g_pitch);
            const float front[3] = {cp * cy, cp * sy, sp};
            const float right[3] = {sy, -cy, 0.0f};
            const float up[3] = {-cy * sp, -sy * sp, cp};
            const float step = g_speed.load(std::memory_order_relaxed) * (fast ? 4.0f : 1.0f) * (slow ? 0.25f : 1.0f) * dt;
            g_x += (front[0] * forward + right[0] * strafe) * step;
            g_y += (front[1] * forward + right[1] * strafe) * step;
            g_z += (front[2] * forward + rise) * step;

            wrVec(ram, cam + kCamSource, g_x, g_y, g_z);
            wrVec(ram, cam + kCamFront, front[0], front[1], front[2]);
            wrVec(ram, cam + kCamUp, up[0], up[1], up[2]);
            wrVec(ram, cam + kCamTarget, g_x + front[0], g_y + front[1], g_z + front[2]);
            wrf(ram, cam + kCamFov, g_fov);
        }

        // CCam::ProcessSpecialHeightRoutines(cam a0), right after the active CCam's update in CCamera::Process
        PS2_GUEST_CC void camHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t cam = GPR_U32(ctx, 4) & kRamMask;
            if (ctx->pc != kCamHeightRoutines || !activeCam(rdram, cam))
                return g_origHeight(rdram, ctx, runtime);
            if (!g_wanted.load(std::memory_order_relaxed))
            {
                end(rdram);
                g_origHeight(rdram, ctx, runtime);
                lcs_input::afterCameraUpdate(rdram, cam); // (empty outside the menu build)
                return;
            }
            if (!g_flying)
                begin(rdram, cam);
            update(rdram, cam);
            ctx->pc = GPR_U32(ctx, 31); // the height routine is skipped (it lifts the follow camera over peds)
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_FREECAM"); e && e[0] == '0')
            return;
        g_origHeight = runtime.lookupFunction(kCamHeightRoutines);
        if (!g_origHeight)
        {
            std::fprintf(stderr, "[freecam] CCam::ProcessSpecialHeightRoutines 0x287020 missing: no free camera\n");
            return;
        }
        runtime.replaceFunction(kCamHeightRoutines, &camHeight);
        g_installed = true;
        std::fprintf(stderr, "[freecam] installed (F8)\n");
    }

    bool installed() { return g_installed.load(std::memory_order_relaxed); }
    bool enabled() { return g_wanted.load(std::memory_order_relaxed); }
    void setEnabled(bool on)
    {
        if (!installed())
            return;
        g_wanted.store(on, std::memory_order_relaxed);
        ps2_stubs::padSetBlocked(on);
    }
    float speed() { return g_speed.load(std::memory_order_relaxed); }
    void setSpeed(float metresPerSecond) { g_speed.store(std::clamp(metresPerSecond, 1.0f, 500.0f), std::memory_order_relaxed); }

    // main thread, once a host frame: F8, the mouse wheel's speed, the mouse captured while flying (not while the
    // video page wants the cursor)
    void hostFrame(bool menuWantsMouse)
    {
        if (!installed())
            return;
        if (platform::keyPressed(platform::KEY_F8))
            setEnabled(!enabled());
        const bool flying = enabled();
        if (flying && platform::keyPressed(platform::KEY_WHEEL_UP))
            setSpeed(speed() * 1.25f);
        if (flying && platform::keyPressed(platform::KEY_WHEEL_DOWN))
            setSpeed(speed() / 1.25f);
        // the mouse while flying; when the flight ends it is let go once (lcs_input holds it again for keyboard & mouse
        // play) - not every frame, which would take it from lcs_input
        static bool wasFlying = false;
        if (flying)
        {
            const bool capture = !menuWantsMouse;
            if (capture != platform::mouseCaptured())
                platform::setMouseCaptured(capture);
        }
        else if (wasFlying)
            platform::setMouseCaptured(false);
        wasFlying = flying;
    }
}
