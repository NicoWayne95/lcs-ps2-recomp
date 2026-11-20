// [lcs-input] Keyboard & mouse as GTA San Andreas on the PC, and which device the player uses (docs/KBM_PROMPTS_CONCEPT.md
// step 1). Experimental: the menu build only (LCS_MENU_VIDEO=1), as the pause-menu work; the other builds keep the
// runtime's built-in mapping (third_party/PS2Recomp/ps2xRuntime/src/lib/ps2_pad.cpp).
//
// The game reads the pad once a game frame (Pad.cpp -> PSPadBackend::readState); PSPadBackend::setReader puts this
// file's reader in place of the built-in mapping: the gamepad as before (positions, not labels), keyboard and mouse by
// the SA PC defaults in two contexts - on foot / in a vehicle (FindPlayerVehicle: CWorld::Players[PlayerInFocus
// 0x3D9BC8] (0x408E40, 0x190 each) -> ped +0x534 vehicle, +0x538 in it) - and a third, the pause menu (pause flags
// 0x3D9CB4 / 0x3D9C68): arrows, Enter, Esc / Backspace.
//
// Keyboard & mouse never lock on (the user's decision 2026-11-16): nothing sends R1 on foot; the right mouse button is
// free aim's (not built yet: until then it does nothing on foot). The mouse turns the camera through the right stick
// meanwhile (1500 counts a second = full tilt; the 1:1 mouse camera is a later step); in a vehicle only with the right
// button held (SA's mouse look). Wheel notches are short presses (platform: 50 ms), one weapon / station each.
//
// Device detection: the last device used wins - a bound key, a mouse button or a mouse movement makes it keyboard &
// mouse, a gamepad button or a stick past 0.35 the gamepad; logged on a change ([lcs-input] device ...). The mouse is
// held in the window while playing on keyboard & mouse (hostFrame, main thread); free in the pause menu, the F2 pages,
// with the window in the background and while the free camera flies (it handles the mouse itself).
// LCS_KBM=0: the built-in mapping as before.
#include "lcs/lcs_diag.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_pad.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cctype>
#include <initializer_list>

namespace lcs_freecam { bool enabled(); }
namespace lcs_menu { bool onSubpage(); void requestBack(); bool mouseOverEntry(); int currentScreen(); } // Esc on a page of ours: one page back; the mouse over an entry

namespace lcs_input
{
    enum Device : int { kKeyboardMouse = 0, kGamepad = 1 };

#if LCS_MENU_VIDEO
    namespace
    {
        using namespace platform;
        constexpr uint16_t PAD_SELECT = 0x0001u, PAD_L3 = 0x0002u, PAD_R3 = 0x0004u, PAD_START = 0x0008u, PAD_UP = 0x0010u,
                           PAD_RIGHT = 0x0020u, PAD_DOWN = 0x0040u, PAD_LEFT = 0x0080u, PAD_L2 = 0x0100u, PAD_R2 = 0x0200u,
                           PAD_L1 = 0x0400u, PAD_R1 = 0x0800u, PAD_TRIANGLE = 0x1000u, PAD_CIRCLE = 0x2000u,
                           PAD_CROSS = 0x4000u, PAD_SQUARE = 0x8000u;

        PS2Runtime *g_runtime = nullptr;
        std::atomic<int> g_sensitivity{3};  // 0..9 bars: 0.25 + 0.25 x step (3 = 1.0, the 1500 counts a second)
        std::atomic<bool> g_rawInput{true};
        std::atomic<bool> g_invertMouse{false}; // INVERT MOUSE: ON = the mouse forward looks down (OFF: forward looks up)
        float sensitivityScale() { return 0.25f + 0.25f * static_cast<float>(g_sensitivity.load(std::memory_order_relaxed)); }
        std::atomic<int> g_device{kKeyboardMouse};
        std::atomic<bool> g_menuUp{false}, g_inVehicle{false};

        // a binding: up to three keys (KEY_* incl. mouse buttons / wheel), 0 = empty
        struct Bind
        {
            int k[3];
            bool down() const
            {
                for (int key : k)
                    if (key && keyDown(key))
                        return true;
                return false;
            }
        };

        // SA PC defaults (the user's screenshot; docs/KBM_PROMPTS_CONCEPT.md 3c)
        struct Foot
        {
            Bind fire{KEY_MOUSE_LEFT, KEY_LEFT_CONTROL, KEY_KP_0};
            Bind nextWeapon{KEY_WHEEL_DOWN, KEY_E, KEY_KP_ENTER};
            Bind prevWeapon{KEY_WHEEL_UP, KEY_Q, KEY_KP_DECIMAL};
            Bind forward{KEY_W, KEY_UP, 0}, back{KEY_S, KEY_DOWN, 0}, left{KEY_A, KEY_LEFT, 0}, right{KEY_D, KEY_RIGHT, 0};
            Bind enterExit{KEY_F, KEY_ENTER, 0};
            Bind camera{KEY_V, KEY_HOME, 0};
            Bind jump{KEY_LEFT_SHIFT, KEY_RIGHT_CONTROL, 0};
            Bind sprint{KEY_SPACE, 0, 0};
            Bind aim{KEY_MOUSE_RIGHT, KEY_CAPS_LOCK, KEY_DELETE}; // free aim (later); never the PS2's lock-on
            Bind crouch{KEY_C, 0, 0};
            Bind action{KEY_TAB, 0, 0};
            Bind walk{KEY_LEFT_ALT, 0, 0};
            Bind lookBehind{KEY_MOUSE_MIDDLE, KEY_KP_1, 0};
        } g_foot;
        struct Vehicle
        {
            Bind fire{KEY_MOUSE_LEFT, KEY_LEFT_ALT, KEY_RIGHT_CONTROL};
            Bind accelerate{KEY_W, 0, 0}, brake{KEY_S, 0, 0};
            Bind left{KEY_A, KEY_LEFT, 0}, right{KEY_D, KEY_RIGHT, 0};
            Bind steerDown{KEY_UP, 0, 0}, steerUp{KEY_DOWN, 0, 0};
            Bind enterExit{KEY_F, KEY_ENTER, 0};
            Bind tripSkip{KEY_Y, 0, 0};
            Bind nextRadio{KEY_WHEEL_UP, KEY_INSERT, KEY_FOUR};
            Bind horn{KEY_H, KEY_CAPS_LOCK, 0};
            Bind subMission{KEY_TWO, KEY_KP_ADD, 0};
            Bind camera{KEY_V, KEY_HOME, 0};
            Bind handbrake{KEY_SPACE, KEY_RIGHT_CONTROL, 0};
            Bind lookBehind{KEY_MOUSE_MIDDLE, 0, 0};
            Bind mouseLook{KEY_MOUSE_RIGHT, 0, 0};
            Bind lookLeft{KEY_Q, 0, 0}, lookRight{KEY_E, 0, 0};
            Bind specialLeft{KEY_KP_4, 0, 0}, specialRight{KEY_KP_6, 0, 0}, specialUp{KEY_KP_8, 0, 0}, specialDown{KEY_KP_2, 0, 0};
        } g_vehicle;
        Bind g_pause{KEY_ESCAPE, 0, 0};

        uint32_t rd32(uint32_t a) { return g_runtime->memory().read32(a); }

        // [lcs-input] The native mouse camera on foot (the port of the reference port ref_input.cpp's foot camera; docs/
        // KBM_PROMPTS_CONCEPT.md 6). The active CCam (TheCamera 0x43BBF0 + 0x220 + u8 [+0x10C] x 0x2B0) in mode 4 (follow
        // ped, 0x281E90): the mouse turns its heading (+0xF0, 0..2 pi, right turns clockwise) and pitch (+0xE0, up +)
        // itself, 400 counts a radian at sensitivity 100 % (MOUSE SENSITIVITY scales it), and the two points the camera
        // eases toward (+0x220 / +0x230) turn with it - about the player for the heading, about the target point (+0x160)
        // for the pitch - else the camera pulls back behind Tony. The right stick gets nothing from the mouse then (no
        // top speed, no pull back). Matched against the reference port's follow camera (0x29DAB0, 85 %). Pitch kept in +-0.7 rad (the
        // game's own limits, the reference port's tuning table, are still to be found). LCS_MOUSE_CAMERA=0: the mouse as the right stick.
        constexpr uint32_t kTheCamera = 0x0043BBF0u, kCamModeFollowPed = 4u;
        // [lcs-input] The first-person weapon cameras, as the reference port's (vcs_input.cpp scopeMode, its
        // game-internals.md 12c): sniper 7, rocket launcher 8, M16 34, 45 .. 47 (mounted gun, camera, binoculars). Their
        // update turns them by the CCam's heading / pitch itself and keeps them in its limits, so the mouse turns those
        // directly (the user's report 2026-11-18: in a scope the mouse did nothing - the right button's R1 put the scope
        // up, which the free-aim code below took for its own aiming camera and threw the mouse away, and without the
        // button the mouse went to the right stick, which a scope doesn't read). The modes are the reference port's
        // numbers; a scope in another mode shows in the log's "camera mode" lines.
        bool scopeMode(uint32_t mode) { return mode == 7u || mode == 8u || mode == 34u || (mode >= 45u && mode <= 47u); }
        const bool g_nativeCamera = !(std::getenv("LCS_MOUSE_CAMERA") && std::getenv("LCS_MOUSE_CAMERA")[0] == '0');
        float sensitivityScale();

        // [lcs-input] No auto aim: sub_001DBD90 (a0 the ped, a1 the target) is the player's target setter (releases the
        // old target via 0x2B5CF8, registers the new one via 0x2B5C60, stores ped +0x6BC). While the aim button's R1
        // runs before L3 (the game's free aim), a ped target is refused: the lock on a passer-by for ~0.5 s the user saw.
        constexpr uint32_t kSetTarget = 0x001DBD90u;
        std::atomic<bool> g_blockLock{false};
        PS2Runtime::RecompiledFunction g_origSetTarget = nullptr;
        PS2_GUEST_CC void setTarget(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_blockLock.load(std::memory_order_relaxed) && GPR_U32(ctx, 5) != 0u)
            {
                uint32_t player;
                std::memcpy(&player, rdram + 0x00408E40u + rdram[0x003D9BC8u] * 0x190u, 4);
                if (GPR_U32(ctx, 4) == player)
                {
                    static uint32_t n = 0;
                    if (n++ < 8u)
                        std::fprintf(stderr, "[lcs-aim] lock-on on 0x%08X refused (ra 0x%08X)\n", GPR_U32(ctx, 5),
                                     GPR_U32(ctx, 31));
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
            }
            g_origSetTarget(rdram, ctx, runtime);
        }

        // [lcs-input] The game's free-aim update (sub_0034AF20(ped, pad), the reference port's FUN_002343E0: the caller of the target
        // setter that writes the aim point ped +0xBB0) put the dummy back each frame along its own, lagging aim, 40 m out
        // (log 13:25: 30 of 90 reads, 0.02 rad behind ours) - the "imprecise" aim. Right after it, the dummy and the aim
        // point are set to the mouse's (the reader's last), so the camera, the circle and the shots that follow see ours.
        constexpr uint32_t kFreeAimUpdate = 0x0034AF20u, kPedAimPoint = 0xBB0u;
        std::atomic<bool> g_aimHold{false};
        std::atomic<uint32_t> g_aimDummy{0};
        std::atomic<float> g_aimAt[3];
        std::atomic<uint32_t> g_aimFixes{0};
        PS2Runtime::RecompiledFunction g_origFreeAimUpdate = nullptr;
        PS2_GUEST_CC void freeAimUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kFreeAimUpdate)
                return g_origFreeAimUpdate(rdram, ctx, runtime);
            const uint32_t ped = GPR_U32(ctx, 4) & PS2_RAM_MASK;
            g_origFreeAimUpdate(rdram, ctx, runtime);
            if (!g_aimHold.load(std::memory_order_acquire))
                return;
            uint32_t player, target;
            std::memcpy(&player, rdram + 0x00408E40u + rdram[0x003D9BC8u] * 0x190u, 4);
            if (ped != player || ped == 0u)
                return;
            std::memcpy(&target, rdram + ped + 0x6BCu, 4);
            const uint32_t dummy = g_aimDummy.load(std::memory_order_relaxed);
            if (target != dummy || dummy == 0u || dummy >= (PS2_RAM_MASK & ~0xFu))
                return;
            for (uint32_t i = 0; i < 3u; ++i)
            {
                const float v = g_aimAt[i].load(std::memory_order_relaxed);
                std::memcpy(rdram + dummy + 0x30u + i * 4u, &v, 4);
                std::memcpy(rdram + ped + kPedAimPoint + i * 4u, &v, 4);
            }
            g_aimFixes.fetch_add(1, std::memory_order_relaxed);
        }

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & PS2_RAM_MASK), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & PS2_RAM_MASK), &v, 4); }

        // [lcs-input] The follow camera's own rules, as the reference port's (game-internals.md 12b there), found for LCS 2026-11-16:
        // - its tuning table 0x3936D0 (outdoors; 0x39370C indoors, +0x3C) is the reference port's 0x487B30 value for value: +0x2C caps
        //   the turn behind a walking player (0.02 / 0.03; 0 switches every automatic turn off - the user's "anchor"),
        //   +0x34 / +0x38 the pitch up / down (0.785 / 1.484);
        // - gp - 0x48 = 0x3D6EA8 is the reference port's 0x487E58: the line of sight that pulls the camera in toward Tony at a wall (and
        //   off the ground looking down) runs only for a turned stick or while it is > 0 (set to 2 after a pull-in, counted
        //   down: 0x2834D0 / 0x282374, checked at 0x28205C). The mouse turns no stick, so the camera stood in the wall at
        //   its last good place while Tony walked on (the user's report). As the reference port: 1 while the mouse turns and 300 ms of
        //   game time (0x3D9B70) after.
        constexpr uint32_t kCamTable = 0x003936D0u, kCamTableIndoors = 0x0039370Cu, kWallCounter = 0x003D6EA8u,
                           kGameMs = 0x003D9B70u;
        float g_turnCap[2] = {-1.0f, -1.0f}; // the table's turn caps while the mouse camera holds them at 0 (-1: as the game)
        uint32_t g_lastMouseTurn = 0;
        void holdAutoTurn(uint8_t *ram, bool off)
        {
            const uint32_t at[2] = {kCamTable + 0x2Cu, kCamTableIndoors + 0x2Cu};
            for (int i = 0; i < 2; ++i)
            {
                if (off && g_turnCap[i] < 0.0f)
                {
                    g_turnCap[i] = rdf(ram, at[i]);
                    wrf(ram, at[i], 0.0f);
                }
                else if (!off && g_turnCap[i] >= 0.0f)
                {
                    wrf(ram, at[i], g_turnCap[i]);
                    g_turnCap[i] = -1.0f;
                }
            }
        }

        // the heading the mouse set at this frame's pad read, held through the camera's update (afterCameraUpdate)
        float g_holdBeta = 0.0f, g_holdAlpha = 0.0f, g_lastDx = 0.0f, g_lastDy = 0.0f;
        uint32_t g_holdCam = 0;
        bool g_hold = false;

        // true: the mouse turned the camera itself (game thread, at the pad read: before the frame's camera update)
        bool footMouseCamera(float dx, float dy)
        {
            if (!g_nativeCamera)
                return false;
            uint8_t *ram = g_runtime->memory().getRDRAM();
            const uint32_t cam = kTheCamera + 0x220u + ram[kTheCamera + 0x10Cu] * 0x2B0u;
            uint16_t mode;
            std::memcpy(&mode, ram + cam + 0x40u, 2);
            if (mode != kCamModeFollowPed)
                return false;
            const uint32_t player = ram[0x003D9BC8u];
            uint32_t ped;
            std::memcpy(&ped, ram + 0x00408E40u + player * 0x190u, 4);
            if (!ped || ped >= (PS2_RAM_MASK & ~0xFu))
                return false;
            const float look = 0.0025f * sensitivityScale();
            const float turn = -dx * look;
            if (turn != 0.0f)
            {
                float beta = std::fmod(rdf(ram, cam + 0xF0u) + turn, 6.2831855f);
                wrf(ram, cam + 0xF0u, beta < 0.0f ? beta + 6.2831855f : beta);
                const float px = rdf(ram, ped + 0x30u), py = rdf(ram, ped + 0x34u), c = std::cos(turn), sn = std::sin(turn);
                for (uint32_t at : {cam + 0x220u, cam + 0x230u})
                {
                    const float ox = rdf(ram, at) - px, oy = rdf(ram, at + 4u) - py;
                    wrf(ram, at, px + ox * c - oy * sn);
                    wrf(ram, at + 4u, py + ox * sn + oy * c);
                }
            }
            // LCS's pitch has the reference port's sign the other way (+ looks down: mouse forward looked down on foot while free aim
            // looked up, user's test 2026-11-16): INVERT MOUSE flips up / down on foot and aiming alike; the limits swap
            // with it (down 1.484 from the table's +0x38, up 0.785 from +0x34). 2026-11-19 (beta testers: "Invert ON is
            // actually not inverted"): OFF is now the usual PC mouse look, ON the inverted one - OFF was the inverted one
            // (mouse down looked up, set as the default 2026-11-17).
            const float up = (g_invertMouse.load(std::memory_order_relaxed) ? -dy : dy) * look; // + : look up
            const float alpha = rdf(ram, cam + 0xE0u);
            const float tilt = std::clamp(alpha - up, -rdf(ram, kCamTable + 0x34u), rdf(ram, kCamTable + 0x38u)) - alpha;
            if (tilt != 0.0f)
            {
                // as the reference port: the pitch and the camera's points turned up / down about the target point (+0x160); it stood
                // still before only for the walls (the line of sight below), not for this
                wrf(ram, cam + 0xE0u, alpha + tilt);
                const float tx = rdf(ram, cam + 0x160u), ty = rdf(ram, cam + 0x164u), tz = rdf(ram, cam + 0x168u);
                for (uint32_t at : {cam + 0x220u, cam + 0x230u})
                {
                    const float ox = rdf(ram, at) - tx, oy = rdf(ram, at + 4u) - ty, oz = rdf(ram, at + 8u) - tz;
                    const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                    if (h < 1e-4f)
                        continue;
                    // tilt already has LCS's sign (alpha - up): the points turn by it as they did (flipping both
                    // undid the flip - the camera follows its points, user's test 2026-11-17)
                    const float e = std::clamp(std::atan2(-oz, h) + tilt, -1.55f, 1.55f);
                    const float k = d * std::cos(e) / h;
                    wrf(ram, at, tx + ox * k);
                    wrf(ram, at + 4u, ty + oy * k);
                    wrf(ram, at + 8u, tz - d * std::sin(e));
                }
            }
            // the walls: the pull-in's path while the mouse turns and 300 ms after; no automatic turn behind him
            uint32_t now;
            std::memcpy(&now, ram + kGameMs, 4);
            if (turn != 0.0f || tilt != 0.0f)
                g_lastMouseTurn = now;
            uint32_t wall;
            std::memcpy(&wall, ram + kWallCounter, 4);
            if (now - g_lastMouseTurn < 300u && wall == 0u)
            {
                const uint32_t one = 1;
                std::memcpy(ram + kWallCounter, &one, 4);
            }
            holdAutoTurn(ram, true);
            g_holdBeta = rdf(ram, cam + 0xF0u);
            g_holdAlpha = rdf(ram, cam + 0xE0u);
            g_lastDx = dx;
            g_lastDy = dy;
            g_holdCam = cam;
            g_hold = true;
            static bool logged = false;
            if (!logged && (turn != 0.0f || tilt != 0.0f))
            {
                logged = true;
                std::fprintf(stderr, "[lcs-input] native mouse camera on foot (CCam 0x%06X mode 4)\n", cam);
            }
            return true;
        }

        bool playerInVehicle()
        {
            const uint32_t player = g_runtime->memory().read8(0x003D9BC8u);
            const uint32_t ped = rd32(0x00408E40u + player * 0x190u);
            return ped && ped < (PS2_RAM_MASK & ~0xFu) && rd32(ped + 0x538u) != 0u && rd32(ped + 0x534u) != 0u;
        }

        // any key / button the keyboard-and-mouse side would use (device detection)
        bool kbmTouched(float mdx, float mdy)
        {
            if (std::fabs(mdx) + std::fabs(mdy) > 2.0f)
                return true;
            for (int k = 32; k < KEY_COUNT; ++k)
                if (k != KEY_WHEEL_UP && k != KEY_WHEEL_DOWN && keyDown(k))
                    return true;
            return keyDown(KEY_WHEEL_UP) || keyDown(KEY_WHEEL_DOWN);
        }

        float stickAxis(GamepadAxis a)
        {
            const float v = gamepadAxis(a);
            return std::fabs(v) < 0.12f ? 0.0f : v;
        }

        bool reader(uint8_t *data)
        {
            if (!g_runtime)
                return false;
            const bool physical = ps2control::physicalInputOn();
            const bool blocked = ps2control::gameInputBlocked();
            uint16_t btn = 0;
            float lx = 0, ly = 0, rx = 0, ry = 0;

            // the gamepad, as the built-in mapping
            bool padUsed = false;
            if (physical && gamepadConnected())
            {
                const struct { GamepadButton b; uint16_t m; } map[] = {
                    {GAMEPAD_DPAD_UP, PAD_UP}, {GAMEPAD_DPAD_DOWN, PAD_DOWN}, {GAMEPAD_DPAD_LEFT, PAD_LEFT},
                    {GAMEPAD_DPAD_RIGHT, PAD_RIGHT}, {GAMEPAD_SOUTH, PAD_CROSS}, {GAMEPAD_EAST, PAD_CIRCLE},
                    {GAMEPAD_WEST, PAD_SQUARE}, {GAMEPAD_NORTH, PAD_TRIANGLE}, {GAMEPAD_LEFT_SHOULDER, PAD_L1},
                    {GAMEPAD_RIGHT_SHOULDER, PAD_R1}, {GAMEPAD_LEFT_STICK, PAD_L3}, {GAMEPAD_RIGHT_STICK, PAD_R3},
                    {GAMEPAD_BACK, PAD_SELECT}, {GAMEPAD_START, PAD_START}};
                for (const auto &e : map)
                    if (gamepadButton(e.b))
                        btn |= e.m;
                if (gamepadAxis(AXIS_LEFT_TRIGGER) > 0.3f)
                    btn |= PAD_L2;
                if (gamepadAxis(AXIS_RIGHT_TRIGGER) > 0.3f)
                    btn |= PAD_R2;
                lx = stickAxis(AXIS_LEFT_X);
                ly = stickAxis(AXIS_LEFT_Y);
                rx = stickAxis(AXIS_RIGHT_X);
                ry = stickAxis(AXIS_RIGHT_Y);
                padUsed = btn != 0 || std::max({std::fabs(lx), std::fabs(ly), std::fabs(rx), std::fabs(ry)}) > 0.35f;
            }

            // the mouse's movement since the last read (the free camera takes it while it flies)
            float mdx = 0, mdy = 0;
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.001f, 0.1f);
            last = now;
            const bool fly = lcs_freecam::enabled();
            if (physical && !fly && mouseCaptured())
                takeMouseMotion(mdx, mdy);

            const bool kbmUsed = physical && kbmTouched(mdx, mdy);
            if (g_device.load(std::memory_order_relaxed) == kGamepad && g_turnCap[0] >= 0.0f)
                holdAutoTurn(g_runtime->memory().getRDRAM(), false); // the gamepad: the game's own camera turn
            const int before = g_device.load(std::memory_order_relaxed);
            const int device = padUsed && !kbmUsed ? kGamepad : kbmUsed && !padUsed ? kKeyboardMouse : before;
            if (device != before)
            {
                g_device.store(device, std::memory_order_relaxed);
                std::fprintf(stderr, "[lcs-input] device: %s\n", device == kGamepad ? "gamepad" : "keyboard and mouse");
            }

            // keyboard & mouse
            const bool menu = g_menuUp.load(std::memory_order_relaxed);
            const bool vehicle = !menu && playerInVehicle();
            g_inVehicle.store(vehicle, std::memory_order_relaxed);
            if (physical && !blocked)
            {
                auto press = [&](bool down, uint16_t m) { if (down) btn |= m; };
                auto axis = [](float cur, bool neg, bool pos) { return neg != pos ? (neg ? -1.0f : 1.0f) : cur; };
                // Esc (the user's ask 2026-11-18: back step by step, at the top it closes - not Backspace). What it does
                // is decided when it goes down and held until it is let go (the pad is read more often than the menu
                // reads it - short pulses were missed, user's test 17:05 "ESC wird nicht erkannt"; a long press does one
                // step): outside the menu START (it opens); on a page of ours its parent (Cross, which the menu turns into
                // "open the page above": lcs_menu requestBack); on the game's deeper screens (8 and up: loading, saving)
                // Triangle; on the menu's own pages (screens 0 - 7) START - it closes. The right mouse button the same.
                const bool esc = g_pause.down();
                const bool rmb = menu && !mouseCaptured() && platform::mouseDown(platform::MOUSE_RIGHT);
                enum EscDoes { kNone, kOpen, kBackOurs, kBackGame, kClose };
                static int escDoes = kNone;
                if (!esc && !rmb)
                    escDoes = kNone;
                else if (escDoes == kNone)
                {
                    const int screen = lcs_menu::currentScreen();
                    if (!menu)
                        escDoes = esc ? kOpen : kNone;
                    else if (lcs_menu::onSubpage())
                    {
                        lcs_menu::requestBack();
                        escDoes = kBackOurs;
                    }
                    else
                        escDoes = screen >= 8 ? kBackGame : kClose;
                    if (escDoes != kNone)
                        std::fprintf(stderr, "[lcs-input] Esc: %s (screen %d)\n",
                                     escDoes == kOpen ? "open the menu" : escDoes == kBackOurs ? "one page back"
                                     : escDoes == kBackGame ? "back (Triangle)" : "close the menu", screen);
                }
                press(escDoes == kBackOurs, PAD_CROSS);
                press(escDoes == kBackGame, PAD_TRIANGLE);
                press(escDoes == kOpen || escDoes == kClose, PAD_START);
                if (menu)
                {
                    press(keyDown(KEY_UP), PAD_UP);
                    press(keyDown(KEY_DOWN), PAD_DOWN);
                    press(keyDown(KEY_LEFT), PAD_LEFT);
                    press(keyDown(KEY_RIGHT), PAD_RIGHT);
                    press(keyDown(KEY_ENTER) || keyDown(KEY_KP_ENTER) || keyDown(KEY_SPACE), PAD_CROSS);
                    // a left click on the entry under the cursor: Cross (the game's entries; ours step themselves)
                    press(!mouseCaptured() && platform::mouseDown(platform::MOUSE_LEFT) && lcs_menu::mouseOverEntry(), PAD_CROSS);
                }
                else if (!vehicle)
                {
                    const Foot &f = g_foot;
                    press(f.fire.down(), PAD_CIRCLE);
                    press(f.nextWeapon.down(), PAD_R2);
                    press(f.prevWeapon.down(), PAD_L2);
                    press(f.enterExit.down(), PAD_TRIANGLE);
                    press(f.camera.down(), PAD_SELECT);
                    press(f.jump.down(), PAD_SQUARE);
                    press(f.sprint.down(), PAD_CROSS);
                    press(f.crouch.down(), PAD_L3);
                    press(f.action.down(), PAD_L1);
                    press(f.lookBehind.down(), PAD_R3);
                    // [lcs-input] Free aim, step 1 (the reference port game-internals.md 12c: the game's own free aim is R1 then L3, or both
                    // in one pad report from nothing - no lock-on on a ped first): the aim button's first read sends R1 +
                    // L3, then R1 while held. In it the left stick moves the aiming circle (Tony stands still, as on the
                    // PS2): the mouse drives that stick, the move keys do nothing meanwhile. Never R1 alone: no auto aim.
                    // Step 2 (Tony moving, the camera 1:1, the shot through the circle) needs the ped's target / free-aim
                    // fields of LCS. LCS_FREEAIM=0: the aim button does nothing.
                    static const bool freeAimOn = !(std::getenv("LCS_FREEAIM") && std::getenv("LCS_FREEAIM")[0] == '0');
                    static bool aimWas = false;
                    static uint32_t aimReads = 0;
                    const bool aimNow = freeAimOn && f.aim.down();
                    // LCS's help (HELP42): "While targeting, tap L3 to enter Free Aim mode" - R1 + L3 in one report did
                    // nothing (test 2026-11-16, 9 tries): R1 first, L3 tapped at the 4th read, R1 held after. The camera's
                    // mode is logged (when it changes, while held) to see what the game does; the mouse stays the camera's
                    // until the aiming camera is up.
                    uint16_t camMode = 0;
                    {
                        uint8_t *ram = g_runtime->memory().getRDRAM();
                        const uint32_t cam = kTheCamera + 0x220u + ram[kTheCamera + 0x10Cu] * 0x2B0u;
                        std::memcpy(&camMode, ram + cam + 0x40u, 2);
                    }
                    if (aimNow)
                    {
                        aimReads = aimWas ? aimReads + 1 : 0;
                        btn |= PAD_R1;
                        // the gamepad's way (log 2026-11-16: R1 held, then L3 held a few frames -> camera mode 11, free
                        // aim; L3 for one read after three of R1 did nothing): R1 alone 10 reads, then L3 for 3 reads,
                        // again every 20 reads while the aiming camera (11) is not up
                        // no auto aim: R1 alone for those 10 reads locked on to a ped nearby (user's test). The player's
                        // target (ped +0x6BC: the reference port +0x820, matched in the lock-on camera 0x280E28 / the reference port 0x29C280, 6 of 6)
                        // is cleared until L3 is sent - from then the game's free aim puts its own aiming object there
                        g_blockLock.store(aimReads < 10u, std::memory_order_relaxed);
                        if (aimReads < 10u)
                        {
                            uint8_t *ram = g_runtime->memory().getRDRAM();
                            uint32_t ped;
                            std::memcpy(&ped, ram + 0x00408E40u + ram[0x003D9BC8u] * 0x190u, 4);
                            uint32_t target = 0;
                            if (ped && ped < (PS2_RAM_MASK & ~0xFu))
                                std::memcpy(&target, ram + ped + 0x6BCu, 4);
                            if (target)
                            {
                                const uint32_t none = 0;
                                std::memcpy(ram + ped + 0x6BCu, &none, 4);
                                static bool logged = false;
                                if (!logged)
                                {
                                    logged = true;
                                    std::fprintf(stderr, "[lcs-input] free aim: lock-on on 0x%08X taken back (no auto aim)\n", target);
                                }
                            }
                        }
                        const uint32_t phase = aimReads < 10u ? 0u : (aimReads - 10u) % 20u;
                        if (aimReads >= 10u && phase < 3u && camMode == kCamModeFollowPed)
                            btn |= PAD_L3;
                        btn &= static_cast<uint16_t>(~PAD_CROSS); // no sprint: the game never enters free aim sprinting
                        static uint16_t lastMode = 0xFFFF;
                        if (camMode != lastMode || aimReads == 3u)
                        {
                            std::fprintf(stderr, "[lcs-input] aim read %u: camera mode %u%s\n", aimReads, camMode,
                                         aimReads == 3u ? " (L3 tapped)" : "");
                            lastMode = camMode;
                        }
                    }
                    if (!aimNow)
                        g_blockLock.store(false, std::memory_order_relaxed);
                    const bool scope = scopeMode(camMode);
                    const bool aimCamera = aimNow && camMode != kCamModeFollowPed && !scope; // the game's aiming camera is up
                    static bool aiming = false; // the dummy is ours (camera mode not 4, a target in +0x6BC)
                    static float aimYaw = 0.0f, aimPitch = 0.0f;
                    if (aimCamera)
                    {
                        // [lcs-input] Native free aim, as the reference port's ref_aim.cpp (no script, no stick): in the game's free aim
                        // the player's target (ped +0x6BC) is a dummy object the circle is drawn on and the lock-on camera
                        // centres. The mouse turns the aim (yaw / pitch about Tony's shoulders) 1:1 at the foot camera's
                        // rate (0.0025 rad a count x MOUSE SENSITIVITY), the same up / down as on foot (INVERT MOUSE
                        // alike), the dummy is put 12 m out along it, and the camera's heading / pitch turned by the same
                        // amount so it has nothing to catch up. The left stick stays centred (else the game moves the
                        // dummy itself, the "joystick" the user felt).
                        uint8_t *ram = g_runtime->memory().getRDRAM();
                        const uint32_t cam = kTheCamera + 0x220u + ram[kTheCamera + 0x10Cu] * 0x2B0u;
                        uint32_t ped = 0, dummy = 0;
                        std::memcpy(&ped, ram + 0x00408E40u + ram[0x003D9BC8u] * 0x190u, 4);
                        if (ped && ped < (PS2_RAM_MASK & ~0xFu))
                            std::memcpy(&dummy, ram + ped + 0x6BCu, 4);
                        if (dummy && dummy < (PS2_RAM_MASK & ~0xFu) && dummy != ped)
                        {
                            constexpr float kDummyDistance = 12.0f, kPivotHeight = 0.6f, kPitchLimit = 0.9f;
                            if (!aiming)
                            {
                                // start where the camera looks, about level
                                const float fx = rdf(ram, cam + 0x1C0u), fy = rdf(ram, cam + 0x1C4u), fz = rdf(ram, cam + 0x1C8u);
                                aimYaw = std::atan2(fy, fx);
                                aimPitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -0.35f, 0.35f);
                                aiming = true;
                                std::fprintf(stderr, "[lcs-aim] native free aim on (dummy 0x%08X, yaw %.2f pitch %.2f)\n",
                                             dummy, aimYaw, aimPitch);
                            }
                            const float look = 0.0025f * sensitivityScale();
                            const float turn = -mdx * look;
                            const float up = (g_invertMouse.load(std::memory_order_relaxed) ? mdy : -mdy) * look; // + : up (as on foot: OFF the usual mouse look, ON inverted)
                            const float pitch = std::clamp(aimPitch + up, -kPitchLimit, kPitchLimit);
                            const float tilt = pitch - aimPitch;
                            aimYaw = std::remainder(aimYaw + turn, 6.2831855f);
                            aimPitch = pitch;
                            if (turn != 0.0f || tilt != 0.0f)
                            {
                                float beta = std::fmod(rdf(ram, cam + 0xF0u) + turn, 6.2831855f);
                                wrf(ram, cam + 0xF0u, beta < 0.0f ? beta + 6.2831855f : beta);
                                wrf(ram, cam + 0xE0u, rdf(ram, cam + 0xE0u) - tilt); // LCS: + looks down
                            }
                            const float cp = std::cos(aimPitch);
                            const float px = rdf(ram, ped + 0x30u), py = rdf(ram, ped + 0x34u), pz = rdf(ram, ped + 0x38u);
                            static float lastAt[3] = {0, 0, 0};
                            static uint32_t moved = 0, checks = 0;
                            // does the game keep our place (or put the dummy back somewhere of its own)?
                            const float ox = rdf(ram, dummy + 0x30u), oy = rdf(ram, dummy + 0x34u), oz = rdf(ram, dummy + 0x38u);
                            ++checks;
                            // the reference port (game-internals.md 12c): the game's free-aim update puts the dummy back on a point of
                            // its own each frame (the reference port ped +0xC90), which undid the mouse every third read here (log
                            // 13:13: 30 of 90). LCS's field is found here: when the game moved the dummy, a vec3 in the
                            // ped (or its CPlayerInfo) equal to where it put it; three hits on one offset and it is
                            // written with the dummy from then on.
                            static uint32_t pointOff = kPedAimPoint, candOff = 0, candHits = 0, misses = 0; // found 13:25
                            const bool gameMoved =
                                std::fabs(ox - lastAt[0]) + std::fabs(oy - lastAt[1]) + std::fabs(oz - lastAt[2]) > 0.05f;
                            if (gameMoved)
                            {
                                ++moved;
                                if (!pointOff)
                                {
                                    uint32_t hit = 0;
                                    const uint32_t info = 0x00408E40u + ram[0x003D9BC8u] * 0x190u;
                                    for (uint32_t base : {ped, info})
                                        for (uint32_t off = 0x40u; off < (base == ped ? 0xC00u : 0x190u) && !hit; off += 4u)
                                        {
                                            if (base == ped && off == 0x30u)
                                                continue;
                                            const uint32_t at = base + off;
                                            if (std::fabs(rdf(ram, at) - ox) < 0.002f && std::fabs(rdf(ram, at + 4u) - oy) < 0.002f &&
                                                std::fabs(rdf(ram, at + 8u) - oz) < 0.002f)
                                                hit = base == ped ? off : 0x80000000u | off;
                                        }
                                    if (hit && hit == candOff)
                                        ++candHits;
                                    else if (hit)
                                    {
                                        candOff = hit;
                                        candHits = 1;
                                    }
                                    else
                                        ++misses;
                                    if (candHits >= 3u)
                                    {
                                        pointOff = candOff;
                                        std::fprintf(stderr, "[lcs-aim] the game's aim point: %s +0x%X (written with the dummy now)\n",
                                                     (pointOff & 0x80000000u) ? "player info" : "ped", pointOff & 0xFFFFu);
                                    }
                                    static uint32_t told = 0;
                                    if (!hit && told++ < 4u)
                                    {
                                        // where the game put it, from Tony: its yaw / pitch against ours
                                        const float dx = ox - px, dy = oy - py, dz = oz - pz - kPivotHeight;
                                        std::fprintf(stderr,
                                                     "[lcs-aim] dummy moved, no copy found: at %.2f m, yaw %.3f pitch %.3f (ours %.3f / %.3f)\n",
                                                     std::sqrt(dx * dx + dy * dy + dz * dz), std::atan2(dy, dx),
                                                     std::atan2(dz, std::sqrt(dx * dx + dy * dy)), aimYaw, aimPitch);
                                    }
                                }
                            }
                            const float at[3] = {px + kDummyDistance * cp * std::cos(aimYaw),
                                                 py + kDummyDistance * cp * std::sin(aimYaw),
                                                 pz + kPivotHeight + kDummyDistance * std::sin(aimPitch)};
                            const uint32_t pointAt = !pointOff ? 0u
                                                     : (pointOff & 0x80000000u)
                                                         ? 0x00408E40u + ram[0x003D9BC8u] * 0x190u + (pointOff & 0xFFFFu)
                                                         : ped + pointOff;
                            for (int i = 0; i < 3; ++i)
                            {
                                wrf(ram, dummy + 0x30u + i * 4u, at[i]);
                                if (pointAt)
                                    wrf(ram, pointAt + i * 4u, at[i]);
                                lastAt[i] = at[i];
                                g_aimAt[i].store(at[i], std::memory_order_relaxed);
                            }
                            g_aimDummy.store(dummy, std::memory_order_relaxed);
                            g_aimHold.store(true, std::memory_order_release);
                            if (checks >= 90u)
                            {
                                std::fprintf(stderr, "[lcs-aim] dummy moved by the game in %u of %u reads, %u set after its update (yaw %.2f pitch %.2f)\n",
                                             moved, checks, g_aimFixes.exchange(0), aimYaw, aimPitch);
                                moved = checks = 0;
                            }
                        }
                        lx = 0.0f;
                        ly = 0.0f;
                    }
                    if (!aimCamera)
                        g_aimHold.store(false, std::memory_order_release);
                    if (!aimCamera && aiming)
                    {
                        aiming = false;
                        std::fprintf(stderr, "[lcs-aim] native free aim off\n");
                    }
                    aimWas = aimNow;
                    if (!aimCamera)
                    {
                        lx = axis(lx, f.left.down(), f.right.down());
                        ly = axis(ly, f.forward.down(), f.back.down());
                        if (f.walk.down())
                        {
                            lx *= 0.45f;
                            ly *= 0.45f;
                        }
                        if (scope)
                        {
                            // a scope: the mouse turns its heading (+0xF0, right turns clockwise, 0..2 pi) and pitch
                            // (+0xE0, LCS: + looks down) at the foot camera's rate; the scope's update keeps its limits
                            uint8_t *ram = g_runtime->memory().getRDRAM();
                            const uint32_t cam = kTheCamera + 0x220u + ram[kTheCamera + 0x10Cu] * 0x2B0u;
                            const float look = 0.0025f * sensitivityScale();
                            const float turn = -mdx * look;
                            const float up = (g_invertMouse.load(std::memory_order_relaxed) ? -mdy : mdy) * look; // + : up (as on foot)
                            if (turn != 0.0f || up != 0.0f)
                            {
                                float beta = std::fmod(rdf(ram, cam + 0xF0u) + turn, 6.2831855f);
                                wrf(ram, cam + 0xF0u, beta < 0.0f ? beta + 6.2831855f : beta);
                                wrf(ram, cam + 0xE0u, rdf(ram, cam + 0xE0u) - up);
                            }
                            static uint16_t toldMode = 0;
                            if (toldMode != camMode)
                            {
                                toldMode = camMode;
                                std::fprintf(stderr, "[lcs-input] scope camera (mode %u): the mouse turns it\n", camMode);
                            }
                        }
                        else if (!footMouseCamera(mdx, mdy)) // the camera turned by the mouse itself, else the right stick
                        {
                            rx += mdx * sensitivityScale() / (1500.0f * dt);
                            ry += mdy * sensitivityScale() / (1500.0f * dt);
                        }
                    }
                }
                else
                {
                    const Vehicle &v = g_vehicle;
                    press(v.fire.down(), PAD_CIRCLE);
                    press(v.accelerate.down() || v.tripSkip.down(), PAD_CROSS);
                    press(v.brake.down(), PAD_SQUARE);
                    press(v.enterExit.down(), PAD_TRIANGLE);
                    press(v.nextRadio.down(), PAD_L1);
                    press(v.horn.down(), PAD_L3);
                    press(v.subMission.down(), PAD_R3);
                    press(v.camera.down(), PAD_SELECT);
                    press(v.handbrake.down(), PAD_R1);
                    press(v.lookLeft.down() || v.lookBehind.down(), PAD_L2);
                    press(v.lookRight.down() || v.lookBehind.down(), PAD_R2);
                    lx = axis(lx, v.left.down(), v.right.down());
                    ly = axis(ly, v.steerDown.down(), v.steerUp.down());
                    rx = axis(rx, v.specialLeft.down(), v.specialRight.down());
                    ry = axis(ry, v.specialUp.down(), v.specialDown.down());
                    if (v.mouseLook.down())
                    {
                        rx += mdx * sensitivityScale() / (1500.0f * dt);
                        ry += mdy * sensitivityScale() / (1500.0f * dt);
                    }
                }
            }

            auto toByte = [](float x) {
                x = std::clamp(x, -1.0f, 1.0f);
                return static_cast<uint8_t>(x < 0.0f ? 128.0f + x * 128.0f : 128.0f + x * 127.0f);
            };
            { // the mouse's path, every 3 s while it moves: counts in, the right stick out
                static float sumX = 0, sumY = 0, maxRx = 0;
                static uint32_t reads = 0;
                static auto since = now;
                sumX += mdx;
                sumY += mdy;
                maxRx = std::max(maxRx, std::fabs(rx));
                ++reads;
                static const bool inputLog = lcs_diag::wanted("LCS_INPUT_LOG"); // release: only with LCS_INPUT_LOG=1
                if (inputLog && now - since > std::chrono::seconds(3))
                {
                    if (sumX != 0.0f || sumY != 0.0f || !mouseCaptured())
                        std::fprintf(stderr, "[lcs-input] mouse: held %d, %u reads, counts %.0f / %.0f, max right stick %.2f, %s, %s\n",
                                     mouseCaptured() ? 1 : 0, reads, sumX, sumY, maxRx,
                                     g_device.load() == kGamepad ? "gamepad" : "keyboard and mouse",
                                     menu ? "menu" : vehicle ? "vehicle" : "on foot");
                    sumX = sumY = maxRx = 0;
                    reads = 0;
                    since = now;
                }
            }
            { // [lcs-input] the aim diagnosis: every change of the active camera's mode (any device) with the buttons sent
              // then, and R1 / L3 presses - which modes LCS's targeting and free aim use (free aim not entered from the
              // keyboard: the camera stayed mode 4 with R1 held and L3 tapped, 2026-11-16)
                static const bool aimLog = lcs_diag::wanted("LCS_INPUT_LOG");
                uint8_t *ram = g_runtime->memory().getRDRAM();
                const uint32_t cam = kTheCamera + 0x220u + ram[kTheCamera + 0x10Cu] * 0x2B0u;
                uint16_t m;
                std::memcpy(&m, ram + cam + 0x40u, 2);
                static uint16_t lastM = 0xFFFF, lastAim = 0;
                const uint16_t aimBits = btn & (PAD_R1 | PAD_L3 | PAD_L1 | PAD_R2 | PAD_L2);
                static uint32_t aimLogReads = 0;
                if (aimLog && m == 11u && (std::fabs(lx) + std::fabs(ly) + std::fabs(rx) + std::fabs(ry) > 0.05f) && ++aimLogReads % 10u == 0u)
                    std::fprintf(stderr, "[lcs-aim] mode 11 sticks: left %.2f %.2f right %.2f %.2f, mouse %.0f %.0f, %s\n", lx, ly, rx,
                                 ry, mdx, mdy, padUsed ? "gamepad" : "keyboard/mouse");
                if (aimLog && (m != lastM || aimBits != lastAim))
                {
                    std::fprintf(stderr, "[lcs-aim] camera mode %u, buttons %04X (R1 %d L3 %d), %s\n", m, btn,
                                 (btn & PAD_R1) ? 1 : 0, (btn & PAD_L3) ? 1 : 0, padUsed ? "gamepad" : "keyboard/mouse");
                    lastM = m;
                    lastAim = aimBits;
                }
            }
            const uint16_t active = static_cast<uint16_t>(~btn); // active low
            data[2] = static_cast<uint8_t>(active & 0xFFu);
            data[3] = static_cast<uint8_t>(active >> 8);
            data[4] = toByte(rx);
            data[5] = toByte(ry);
            data[6] = toByte(lx);
            data[7] = toByte(ly);
            return true;
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("LCS_KBM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs-input] off (LCS_KBM=0): the built-in mapping\n");
            return;
        }
        g_runtime = &runtime;
        PSPadBackend::setReader(&reader);
        if (std::getenv("LCS_FREEAIM") == nullptr || std::getenv("LCS_FREEAIM")[0] != '0')
        {
            g_origSetTarget = runtime.lookupFunction(kSetTarget);
            if (g_origSetTarget)
                runtime.replaceFunction(kSetTarget, &setTarget);
            else
                std::fprintf(stderr, "[lcs-input] target setter 0x1DBD90 missing: lock-on not blocked\n");
            g_origFreeAimUpdate = runtime.lookupFunction(kFreeAimUpdate);
            if (g_origFreeAimUpdate)
                runtime.replaceFunction(kFreeAimUpdate, &freeAimUpdate);
            else
                std::fprintf(stderr, "[lcs-input] free-aim update 0x34AF20 missing: the game's aim wins each frame\n");
        }
        std::fprintf(stderr, "[lcs-input] keyboard and mouse as GTA SA PC (no lock-on), device detection on\n");
    }

    // main thread, once a host frame: whether the pause menu is up (the reader's menu keys), the mouse held in the
    // window while playing on keyboard & mouse
    void hostFrame(bool pageOpen)
    {
        if (!g_runtime)
            return;
        const uint32_t user = rd32(0x003D9CB4u), code = rd32(0x003D9C68u);
        const bool menu = user != 0u || code != 0u;
        g_menuUp.store(menu, std::memory_order_relaxed);
        if (lcs_freecam::enabled())
            return; // the free camera holds the mouse itself
        // held whatever the device (as the reference port): a free mouse reports no movement, so it could never win the device back
        // (first test 2026-11-16: the gamepad won once, the mouse did nothing after)
        const bool capture = !menu && !pageOpen && windowFocused();
        if (capture != mouseCaptured())
        {
            setMouseCaptured(capture);
            std::fprintf(stderr, "[lcs-input] mouse %s (menu %d, F2 page %d, focus %d)\n", capture ? "held" : "free", menu ? 1 : 0,
                         pageOpen ? 1 : 0, windowFocused() ? 1 : 0);
        }
    }

    int device() { return g_device.load(std::memory_order_relaxed); }
    bool inVehicle() { return g_inVehicle.load(std::memory_order_relaxed); }

    // [lcs-input] After the active CCam's update (lcs_freecam's hook on 0x287020, called right after it): the follow
    // camera turns itself back behind Tony as he runs (the gamepad's stick holds it off; the mouse does not touch the
    // stick) - the camera anchored itself when turning (user's test 2026-11-16). On keyboard & mouse the heading the mouse
    // set is held: the game's own turn of this update is taken back, its position, view and up turned about the player
    // by the same angle (the update's wall handling stays). Game thread.
    void afterCameraUpdate(uint8_t *ram, uint32_t cam)
    {
        if (!g_hold || cam != g_holdCam)
            return;
        g_hold = false;
        uint16_t mode;
        std::memcpy(&mode, ram + cam + 0x40u, 2);
        if (mode != kCamModeFollowPed)
            return;
        const float delta = std::remainder(g_holdBeta - rdf(ram, cam + 0xF0u), 6.2831855f);
        { // [lcs-input] the camera log (what the game's update did against the mouse; the user's "anchor" feel): a line
          // a frame for 1200 frames from the first mouse turn, then every 20th. LCS_CAMLOG=0: none.
            static const bool on = !(std::getenv("LCS_CAMLOG") && std::getenv("LCS_CAMLOG")[0] == '0');
            static uint32_t lines = 0, frames = 0;
            static bool started = false;
            if (g_lastDx != 0.0f || g_lastDy != 0.0f)
                started = true;
            uint32_t pped;
            std::memcpy(&pped, ram + 0x00408E40u + ram[0x003D9BC8u] * 0x190u, 4);
            if (on && started && pped && pped < (PS2_RAM_MASK & ~0xFu) && (lines < 1200 || ++frames % 20 == 0))
            {
                ++lines;
                const float px = rdf(ram, pped + 0x30u), py = rdf(ram, pped + 0x34u), pz = rdf(ram, pped + 0x38u);
                const float sx = rdf(ram, cam + 0x1D0u) - px, sy = rdf(ram, cam + 0x1D4u) - py, sz = rdf(ram, cam + 0x1D8u) - pz;
                const float ax = rdf(ram, cam + 0x220u) - px, ay = rdf(ram, cam + 0x224u) - py;
                static float lpx = px, lpy = py;
                const float moved = std::sqrt((px - lpx) * (px - lpx) + (py - lpy) * (py - lpy));
                lpx = px;
                lpy = py;
                std::fprintf(stderr,
                             "[camlog] dx %5.0f dy %5.0f | beta %.3f game %+.4f | alpha %.3f game %+.4f | cam dist %.2f h %.2f "
                             "pt dist %.2f | ped moved %.3f\n",
                             g_lastDx, g_lastDy, g_holdBeta, -delta, g_holdAlpha, rdf(ram, cam + 0xE0u) - g_holdAlpha,
                             std::sqrt(sx * sx + sy * sy), sz, std::sqrt(ax * ax + ay * ay), moved);
            }
        }
        if (std::fabs(delta) < 1e-5f || true) // the turn cap (holdAutoTurn) stops the game's own turn: nothing to take back
            return;
        uint32_t ped;
        std::memcpy(&ped, ram + 0x00408E40u + ram[0x003D9BC8u] * 0x190u, 4);
        if (!ped || ped >= (PS2_RAM_MASK & ~0xFu))
            return;
        const float px = rdf(ram, ped + 0x30u), py = rdf(ram, ped + 0x34u), c = std::cos(delta), sn = std::sin(delta);
        for (uint32_t at : {cam + 0x1D0u, cam + 0x220u, cam + 0x230u}) // the camera's position, its two points
        {
            const float ox = rdf(ram, at) - px, oy = rdf(ram, at + 4u) - py;
            wrf(ram, at, px + ox * c - oy * sn);
            wrf(ram, at + 4u, py + ox * sn + oy * c);
        }
        for (uint32_t at : {cam + 0x1C0u, cam + 0x1F0u}) // view and up: directions
        {
            const float x = rdf(ram, at), y = rdf(ram, at + 4u);
            wrf(ram, at, x * c - y * sn);
            wrf(ram, at + 4u, x * sn + y * c);
        }
        wrf(ram, cam + 0xF0u, g_holdBeta);
    }

    // the pause menu's MOUSE AND KEYBOARD page (src/lcs/lcs_menu.cpp; main thread)
    int mouseSensitivity() { return g_sensitivity.load(std::memory_order_relaxed); }
    void setMouseSensitivity(int step) { g_sensitivity.store(std::clamp(step, 0, 9), std::memory_order_relaxed); }
    bool rawInput() { return g_rawInput.load(std::memory_order_relaxed); }
    bool invertMouse() { return g_invertMouse.load(std::memory_order_relaxed); }
    void setInvertMouse(bool on) { g_invertMouse.store(on, std::memory_order_relaxed); }
    void setRawInput(bool raw)
    {
        g_rawInput.store(raw, std::memory_order_relaxed);
        platform::setMouseRawInput(raw);
        std::fprintf(stderr, "[lcs-input] raw input %s\n", raw ? "on" : "off (system pointer speed)");
    }

    // the bindings as the FOOT / VEHICLE CONTROLS pages show them: label, then the keys ("LMB / LCTRL / NUM 0")
    namespace
    {
        std::string keyLabel(int k)
        {
            switch (k)
            {
            case KEY_MOUSE_LEFT: return "LMB";
            case KEY_MOUSE_RIGHT: return "RMB";
            case KEY_MOUSE_MIDDLE: return "MMB";
            case KEY_WHEEL_UP: return "WHEEL UP";
            case KEY_WHEEL_DOWN: return "WHEEL DOWN";
            case KEY_LEFT_CONTROL: return "LCTRL";
            case KEY_RIGHT_CONTROL: return "RCTRL";
            case KEY_LEFT_SHIFT: return "LSHIFT";
            case KEY_RIGHT_SHIFT: return "RSHIFT";
            case KEY_LEFT_ALT: return "LALT";
            case KEY_RIGHT_ALT: return "RALT";
            case KEY_CAPS_LOCK: return "CAPSLOCK";
            case KEY_KP_DECIMAL: return "NUM .";
            case KEY_KP_ENTER: return "NUM ENTER";
            case KEY_KP_ADD: return "NUM +";
            case KEY_ENTER: return "RETURN";
            case KEY_DELETE: return "DEL";
            case KEY_INSERT: return "INS";
            default: break;
            }
            if (k >= KEY_KP_0 && k <= KEY_KP_9)
                return "NUM " + std::to_string(k - KEY_KP_0);
            std::string n = keyName(k) ? keyName(k) : "?";
            for (char &c : n)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return n;
        }
        std::string keys(std::initializer_list<const Bind *> binds)
        {
            std::string out;
            for (const Bind *b : binds)
                for (int k : b->k)
                    if (k)
                    {
                        if (!out.empty())
                            out += " / ";
                        out += keyLabel(k);
                    }
            return out.empty() ? "-" : out;
        }
        struct Line
        {
            std::string label, value;
        };
        std::vector<Line> g_lines[2];
    }

    // rebuilt from the bindings (main thread); the strings stay until the next call
    int bindingLines(bool vehicle, const char **labels, const char **values, int max)
    {
        std::vector<Line> &l = g_lines[vehicle ? 1 : 0];
        l.clear();
        if (!vehicle)
        {
            const Foot &f = g_foot;
            l = {{"FIRE:", keys({&f.fire})}, {"NEXT WEAPON:", keys({&f.nextWeapon})}, {"PREVIOUS WEAPON:", keys({&f.prevWeapon})},
                 {"MOVE:", keys({&f.forward, &f.left, &f.back, &f.right})}, {"ENTER + EXIT:", keys({&f.enterExit})},
                 {"CHANGE CAMERA:", keys({&f.camera})}, {"JUMP:", keys({&f.jump})}, {"SPRINT:", keys({&f.sprint})},
                 {"AIM WEAPON:", keys({&f.aim})}, {"CROUCH:", keys({&f.crouch})}, {"ACTION:", keys({&f.action})},
                 {"WALK:", keys({&f.walk})}, {"LOOK BEHIND:", keys({&f.lookBehind})}};
        }
        else
        {
            const Vehicle &v = g_vehicle;
            l = {{"FIRE:", keys({&v.fire})}, {"ACCELERATE:", keys({&v.accelerate})}, {"BRAKE / REVERSE:", keys({&v.brake})},
                 {"STEER:", keys({&v.left, &v.right})}, {"STEER UP / DOWN:", keys({&v.steerDown, &v.steerUp})},
                 {"ENTER + EXIT:", keys({&v.enterExit})}, {"NEXT RADIO STATION:", keys({&v.nextRadio})},
                 {"HORN:", keys({&v.horn})}, {"SUB-MISSION:", keys({&v.subMission})}, {"CHANGE CAMERA:", keys({&v.camera})},
                 {"HANDBRAKE:", keys({&v.handbrake})}, {"LOOK LEFT / RIGHT:", keys({&v.lookLeft, &v.lookRight})},
                 {"LOOK BEHIND:", keys({&v.lookBehind})}, {"MOUSE LOOK:", keys({&v.mouseLook})}};
        }
        const int n = std::min(max, static_cast<int>(l.size()));
        for (int i = 0; i < n; ++i)
        {
            labels[i] = l[i].label.c_str();
            values[i] = l[i].value.c_str();
        }
        return n;
    }
#else
    void install(PS2Runtime &) {}
    void hostFrame(bool) {}
    int device() { return kGamepad; }
    bool inVehicle() { return false; }
    void afterCameraUpdate(uint8_t *, uint32_t) {}
    int mouseSensitivity() { return 3; }
    void setMouseSensitivity(int) {}
    bool rawInput() { return true; }
    void setRawInput(bool) {}
    bool invertMouse() { return false; }
    void setInvertMouse(bool) {}
    int bindingLines(bool, const char **, const char **, int) { return 0; }
#endif
}
