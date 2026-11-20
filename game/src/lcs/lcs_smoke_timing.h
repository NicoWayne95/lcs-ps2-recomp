#pragma once

#include <cstdint>

constexpr bool lcsTimedParticleType(uint32_t type)
{
    return type <= 1u || type == 3u || type == 4u ||
           (type >= 37u && type <= 39u) || type == 44u || type == 45u ||
           type == 50u || type == 53u || type == 85u || type == 33u || (type >= 55u && type <= 57u) || (type >= 65u && type <= 67u);
}
