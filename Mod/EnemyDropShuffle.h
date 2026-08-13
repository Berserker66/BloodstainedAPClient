#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class EnemyDropShuffle {
   public:
    static bool Apply(std::uint32_t seed, int version);
    static void Reset();
    static bool IsApplied();
    static bool IsEligibleEnemy(std::string_view enemy);
    static const std::vector<std::string>& GetItemDroppers(std::string_view item);
};
