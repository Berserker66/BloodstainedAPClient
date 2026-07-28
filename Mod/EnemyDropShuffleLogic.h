#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "EnemyDropData.generated.h"

namespace bloodstained::enemy_drop_shuffle {

inline constexpr int VERSION = 1;
inline constexpr float DROP_RATE = 16.0f;

inline std::uint32_t DeriveSeed(std::string_view seedName, std::uint32_t slotId) {
    // FNV-1a over an explicitly encoded (seed-name length, seed name, slot ID)
    // tuple. Team is intentionally absent: corresponding slots in racing teams
    // must receive the same drop shuffle.
    std::uint32_t hash = 2166136261u;
    const auto hashByte = [&hash](std::uint8_t value) {
        hash ^= value;
        hash *= 16777619u;
    };
    const auto hashUint32 = [&hashByte](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hashByte(static_cast<std::uint8_t>(value >> shift));
        }
    };

    hashUint32(static_cast<std::uint32_t>(seedName.size()));
    for (const unsigned char character : seedName) hashByte(character);
    hashUint32(slotId);
    return hash;
}

struct EnemyAssignment {
    std::string_view enemy;
    std::array<std::string_view, 4> items{};
    std::array<bool, 4> active{};
};

struct ShuffleResult {
    std::vector<EnemyAssignment> enemies;
    bool coversVanillaDrops = false;
};

namespace detail {

enum class Pool { Cooking, Standard, Enemy };

template <typename RandomIndex>
void ShuffleIndices(std::vector<std::size_t>& values, RandomIndex& randomIndex) {
    for (std::size_t remaining = values.size(); remaining > 1; --remaining) {
        const std::size_t other = randomIndex(static_cast<std::uint32_t>(remaining));
        std::swap(values[remaining - 1], values[other]);
    }
}

template <typename RandomIndex>
void ShuffleItems(std::vector<std::string_view>& values, RandomIndex& randomIndex) {
    for (std::size_t remaining = values.size(); remaining > 1; --remaining) {
        const std::size_t other = randomIndex(static_cast<std::uint32_t>(remaining));
        std::swap(values[remaining - 1], values[other]);
    }
}

inline Pool SlotPool(Pool favored, std::size_t slot) {
    constexpr std::array<std::array<Pool, 4>, 3> patterns{{
        {Pool::Cooking, Pool::Standard, Pool::Enemy, Pool::Cooking},
        {Pool::Standard, Pool::Enemy, Pool::Cooking, Pool::Standard},
        {Pool::Enemy, Pool::Cooking, Pool::Standard, Pool::Enemy},
    }};
    return patterns[static_cast<std::size_t>(favored)][slot];
}

template <typename RandomIndex>
std::string_view PickItem(Pool pool, std::vector<std::string_view>& remainingEnemyMaterials,
                          RandomIndex& randomIndex) {
    using namespace enemy_drop_data;
    if (pool == Pool::Enemy && !remainingEnemyMaterials.empty()) {
        const std::size_t index = randomIndex(static_cast<std::uint32_t>(remainingEnemyMaterials.size()));
        const std::string_view item = remainingEnemyMaterials[index];
        remainingEnemyMaterials.erase(remainingEnemyMaterials.begin() + index);
        return item;
    }
    if (pool == Pool::Cooking) {
        return COOKING_MATERIAL_POOL[randomIndex(
            static_cast<std::uint32_t>(std::size(COOKING_MATERIAL_POOL)))];
    }
    return STANDARD_MATERIAL_POOL[randomIndex(
        static_cast<std::uint32_t>(std::size(STANDARD_MATERIAL_POOL)))];
}

template <typename RandomIndex>
void GuaranteeRequiredDrops(std::vector<EnemyAssignment>& assignments, bool ingredientChannel,
                            const std::string_view* required, std::size_t requiredCount,
                            RandomIndex& randomIndex) {
    std::vector<std::size_t> activeSlots;
    std::vector<std::size_t> inactiveSlots;
    const std::size_t firstSlot = ingredientChannel ? 2 : 0;
    const std::size_t lastSlot = ingredientChannel ? 4 : 2;

    for (std::size_t enemy = 0; enemy < assignments.size(); ++enemy) {
        for (std::size_t slot = firstSlot; slot < lastSlot; ++slot) {
            const std::size_t flatIndex = enemy * 4 + slot;
            (assignments[enemy].active[slot] ? activeSlots : inactiveSlots).push_back(flatIndex);
        }
    }

    ShuffleIndices(inactiveSlots, randomIndex);
    while (activeSlots.size() < requiredCount && !inactiveSlots.empty()) {
        const std::size_t flatIndex = inactiveSlots.back();
        inactiveSlots.pop_back();
        assignments[flatIndex / 4].active[flatIndex % 4] = true;
        activeSlots.push_back(flatIndex);
    }

    ShuffleIndices(activeSlots, randomIndex);
    std::vector<std::string_view> requiredItems(required, required + requiredCount);
    ShuffleItems(requiredItems, randomIndex);
    for (std::size_t index = 0; index < requiredItems.size(); ++index) {
        const std::size_t flatIndex = activeSlots[index];
        assignments[flatIndex / 4].items[flatIndex % 4] = requiredItems[index];
    }
}

inline bool CoversRequiredDrops(const std::vector<EnemyAssignment>& assignments) {
    std::unordered_set<std::string_view> itemDrops;
    std::unordered_set<std::string_view> ingredientDrops;
    for (const auto& assignment : assignments) {
        for (std::size_t slot = 0; slot < assignment.items.size(); ++slot) {
            if (!assignment.active[slot]) continue;
            (slot < 2 ? itemDrops : ingredientDrops).insert(assignment.items[slot]);
        }
    }

    return std::ranges::all_of(enemy_drop_data::REQUIRED_VANILLA_ITEM_DROPS,
                               [&itemDrops](std::string_view item) { return itemDrops.contains(item); }) &&
           std::ranges::all_of(
               enemy_drop_data::REQUIRED_VANILLA_INGREDIENT_DROPS,
               [&ingredientDrops](std::string_view item) { return ingredientDrops.contains(item); });
}

}  // namespace detail

// randomIndex(maxExclusive) must return a value in [0, maxExclusive).
template <typename RandomIndex>
ShuffleResult Generate(RandomIndex randomIndex) {
    using namespace enemy_drop_data;
    ShuffleResult result;
    result.enemies.reserve(std::size(ELIGIBLE_ENEMIES));
    std::vector<std::string_view> remainingEnemyMaterials(
        std::begin(ENEMY_MATERIAL_POOL), std::end(ENEMY_MATERIAL_POOL));

    for (const std::string_view enemy : ELIGIBLE_ENEMIES) {
        EnemyAssignment assignment;
        assignment.enemy = enemy;
        const auto favored = static_cast<detail::Pool>(randomIndex(3));
        for (std::size_t slot = 0; slot < assignment.items.size(); ++slot) {
            assignment.active[slot] = randomIndex(3) != 0;
            assignment.items[slot] =
                detail::PickItem(detail::SlotPool(favored, slot), remainingEnemyMaterials, randomIndex);
        }
        result.enemies.push_back(assignment);
    }

    detail::GuaranteeRequiredDrops(result.enemies, false, REQUIRED_VANILLA_ITEM_DROPS,
                                   std::size(REQUIRED_VANILLA_ITEM_DROPS), randomIndex);
    detail::GuaranteeRequiredDrops(result.enemies, true, REQUIRED_VANILLA_INGREDIENT_DROPS,
                                   std::size(REQUIRED_VANILLA_INGREDIENT_DROPS), randomIndex);
    result.coversVanillaDrops = detail::CoversRequiredDrops(result.enemies);
    return result;
}

}  // namespace bloodstained::enemy_drop_shuffle
