#include "runtime/ps2_pad.h"
#include <atomic>
#include "ps2_host_backend.h"
#include "runtime/ps2_control.h"
#include <algorithm>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

namespace
{
    PSPadBackend::Reader g_reader = nullptr;             // [lcs-recomp] setReader
    PSPadBackend::ActuatorSink g_actuatorSink = nullptr; // [lcs-recomp] setActuatorSink
}

void PSPadBackend::setReader(Reader reader) { g_reader = reader; }

// [lcs-recomp] lastInputNs
static std::atomic<int64_t> g_padInputNs{0};
int64_t PSPadBackend::lastInputNs() { return g_padInputNs.load(std::memory_order_relaxed); }
void PSPadBackend::setActuatorSink(ActuatorSink sink) { g_actuatorSink = sink; }

void PSPadBackend::actuators(uint8_t small, uint8_t large)
{
    if (g_actuatorSink)
        g_actuatorSink(small, large);
}

// [lcs-recomp] Keyboard, mouse and gamepad all feed one DualShock 2. Keyboard layout (see docs/CONTROLS.md):
//   WASD left stick, IJKL right stick, arrows D-pad, Shift Cross, Space Square, F Triangle, Ctrl/LMB Circle,
//   RMB/E R1, Q L1, 1/Z L2, 3/X R2, C L3, V R3, Enter Start, Backspace Select.
bool PSPadBackend::drivingPedals(uint8_t &gas, uint8_t &brake) const
{
    gas = m_gas;
    brake = m_brake;
    return m_modernDriving;
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size, bool modernDriving, bool classicDriving)
{
    if (!data || size < 32)
        return false;

    g_padInputNs.store(platform::inputSnapshotNs(), std::memory_order_relaxed); // [lcs-recomp] lastInputNs
    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    if (port == 0)
    {
        m_modernDriving = false;
        m_gas = m_brake = 0;
    }
    if (g_reader) // [lcs-recomp] the player's bindings (setReader)
    {
        data[2] = data[3] = 0xFF;
        data[4] = data[5] = data[6] = data[7] = 0x80;
        if (g_reader(data))
            return true;
    }

    uint16_t btns = 0xFFFFu;
    auto press = [&btns](bool down, uint16_t mask)
    {
        if (down)
            btns &= ~mask;
    };
    float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

    uint8_t gas = 0, brake = 0;
    modernDriving = port == 0 && modernDriving;
    classicDriving = port == 0 && classicDriving;
    bool useClassicPressure = false;
    const bool physical = ps2control::physicalInputOn(); // [lcs-recomp] off in hidden test runs
    const bool blocked = ps2control::gameInputBlocked();
    if (physical && platform::gamepadConnected())
    {
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_UP), PAD_UP);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_DOWN), PAD_DOWN);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_LEFT), PAD_LEFT);
        press(platform::gamepadButton(platform::GAMEPAD_DPAD_RIGHT), PAD_RIGHT);
        press(!modernDriving && platform::gamepadButton(platform::GAMEPAD_SOUTH), PAD_CROSS);
        press(platform::gamepadButton(platform::GAMEPAD_EAST), PAD_CIRCLE);
        press(!modernDriving && platform::gamepadButton(platform::GAMEPAD_WEST), PAD_SQUARE);
        press(platform::gamepadButton(platform::GAMEPAD_NORTH), PAD_TRIANGLE);
        press(platform::gamepadButton(platform::GAMEPAD_LEFT_SHOULDER), PAD_L1);
        press(platform::gamepadButton(platform::GAMEPAD_RIGHT_SHOULDER), PAD_R1);
        const float leftTrigger = platform::gamepadAxis(platform::AXIS_LEFT_TRIGGER);
        const float rightTrigger = platform::gamepadAxis(platform::AXIS_RIGHT_TRIGGER);
        if (classicDriving && !blocked)
        {
            const int cross = platform::gamepadButtonPressure(platform::GAMEPAD_SOUTH);
            const int square = platform::gamepadButtonPressure(platform::GAMEPAD_WEST);
            useClassicPressure = cross >= 0 && square >= 0;
            if (useClassicPressure)
            {
                gas = static_cast<uint8_t>(cross);
                brake = static_cast<uint8_t>(square);
            }
        }
        const bool drivingTriggers = modernDriving && !platform::gamepadButton(platform::GAMEPAD_BACK);
        if (drivingTriggers)
        {
            const auto pressure = [](float value) -> uint8_t {
                return value > 0.03f ? static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f) : 0;
            };
            const int cross = platform::gamepadButtonPressure(platform::GAMEPAD_SOUTH);
            const int square = platform::gamepadButtonPressure(platform::GAMEPAD_WEST);
            if (!blocked)
            {
                gas = std::max(pressure(rightTrigger), static_cast<uint8_t>(std::max(0, cross)));
                brake = std::max(pressure(leftTrigger), static_cast<uint8_t>(std::max(0, square)));
            }
        }
        else
        {
            press(leftTrigger > 0.3f, PAD_L2);
            press(rightTrigger > 0.3f, PAD_R2);
        }
        press(platform::gamepadButton(platform::GAMEPAD_START), PAD_START);
        press(platform::gamepadButton(platform::GAMEPAD_BACK), PAD_SELECT);
        press(platform::gamepadButton(platform::GAMEPAD_LEFT_STICK), PAD_L3);
        press(platform::gamepadButton(platform::GAMEPAD_RIGHT_STICK), PAD_R3);
        // small radial deadzone: worn sticks rest a few percent off centre
        auto deadzone = [](float &x, float &y)
        {
            if (x * x + y * y < 0.12f * 0.12f)
                x = y = 0.0f;
        };
        lx = platform::gamepadAxis(platform::AXIS_LEFT_X);
        ly = platform::gamepadAxis(platform::AXIS_LEFT_Y);
        rx = platform::gamepadAxis(platform::AXIS_RIGHT_X);
        ry = platform::gamepadAxis(platform::AXIS_RIGHT_Y);
        deadzone(lx, ly);
        deadzone(rx, ry);
    }

    // [lcs-recomp] an in-game UI with the input (settings / trainer menu) takes injected keys from the game too
    auto key = [blocked](int k)
    { return !blocked && ps2control::keyDown(k); };
    press(key(platform::KEY_UP), PAD_UP);
    press(key(platform::KEY_DOWN), PAD_DOWN);
    press(key(platform::KEY_LEFT), PAD_LEFT);
    press(key(platform::KEY_RIGHT), PAD_RIGHT);
    press(key(platform::KEY_LEFT_SHIFT) || key(platform::KEY_RIGHT_SHIFT), PAD_CROSS);
    press(key(platform::KEY_SPACE), PAD_SQUARE);
    press(key(platform::KEY_F), PAD_TRIANGLE);
    press(key(platform::KEY_LEFT_CONTROL) || key(platform::KEY_RIGHT_CONTROL) || (physical && platform::mouseDown(platform::MOUSE_LEFT)), PAD_CIRCLE);
    press(key(platform::KEY_Q), PAD_L1);
    press(key(platform::KEY_E) || (physical && platform::mouseDown(platform::MOUSE_RIGHT)), PAD_R1);
    press(key(platform::KEY_ONE) || key(platform::KEY_Z), PAD_L2);
    press(key(platform::KEY_THREE) || key(platform::KEY_X), PAD_R2);
    press(key(platform::KEY_C), PAD_L3);
    press(key(platform::KEY_V), PAD_R3);
    // [lcs-recomp] Alt+Enter switches full screen (main.cpp), so Enter with Alt held is not Start
    const bool alt = key(platform::KEY_LEFT_ALT) || key(platform::KEY_RIGHT_ALT);
    press(!alt && (key(platform::KEY_ENTER) || key(platform::KEY_KP_ENTER)), PAD_START);
    press(key(platform::KEY_BACKSPACE), PAD_SELECT);

    auto axis = [](float current, bool negative, bool positive)
    {
        if (negative != positive)
            return negative ? -1.0f : 1.0f;
        return current;
    };
    lx = axis(lx, key(platform::KEY_A), key(platform::KEY_D));
    ly = axis(ly, key(platform::KEY_W), key(platform::KEY_S));
    rx = axis(rx, key(platform::KEY_J), key(platform::KEY_L));
    ry = axis(ry, key(platform::KEY_I), key(platform::KEY_K));

    auto toByte = [](float v)
    {
        v = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint8_t>(v < 0.0f ? 128.0f + v * 128.0f : 128.0f + v * 127.0f);
    };
    if (modernDriving || useClassicPressure)
    {
        m_modernDriving = true;
        m_gas = key(platform::KEY_LEFT_SHIFT) || key(platform::KEY_RIGHT_SHIFT) ? 255 : gas;
        m_brake = key(platform::KEY_SPACE) ? 255 : brake;
    }
    data[4] = toByte(rx);
    data[5] = toByte(ry);
    data[6] = toByte(lx);
    data[7] = toByte(ly);
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
