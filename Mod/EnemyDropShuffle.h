#pragma once

#include <cstdint>

class EnemyDropShuffle {
   public:
    static bool Apply(std::uint32_t seed, int version);
    static void Reset();
};
