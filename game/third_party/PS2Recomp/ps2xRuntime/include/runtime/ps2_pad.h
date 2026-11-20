#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size, bool modernDriving, bool classicDriving);
    // Sampled with the pad report on the game thread. Returns false for the classic controls.
    bool drivingPedals(uint8_t &gas, uint8_t &brake) const;

    // [lcs-recomp] The game's own input mapping (src/game/ref_input.cpp: the player's bindings) fills the report
    // instead of the built-in one below when set: data[2..3] buttons (active low), data[4..7] rx, ry, lx, ly. Game
    // thread; false falls back to the built-in mapping.
    using Reader = bool (*)(uint8_t *data);
    static void setReader(Reader reader);

    // [lcs-recomp] The game's motor levels from scePadSetActDirect (act 0: small motor on / off, act 1: large motor
    // 0..255, the DualShock 2's usual alignment). Game thread.
    using ActuatorSink = void (*)(uint8_t small, uint8_t large);
    static void setActuatorSink(ActuatorSink sink);
    static void actuators(uint8_t small, uint8_t large);

    // [lcs-recomp] The input snapshot (platform::inputSnapshotNs) the game's last pad read saw: where a frame's input
    // delay starts (gs_hw.cpp stamps it on the frame). Any thread.
    static int64_t lastInputNs();

private:
    bool m_modernDriving = false;
    uint8_t m_gas = 0;
    uint8_t m_brake = 0;
};

#endif
