// [lcs-sound] LCS sound without the emulated IOP's CDSTREAM.IRX sound code: its effects and music servers on the host
// (src/lcs/lcs_sound.cpp, a port of the reference port's src/game/ref_sound.cpp to LCS's own CDSTREAM.IRX).
#pragma once

class PS2Runtime;

namespace lcs_sound
{
    // At boot, before the game binds its sound servers. LCS_NATIVE_SOUND=0: sound stays on the emulated IOP.
    // Returns true when the native driver owns the sound.
    bool install(PS2Runtime &runtime);
}
