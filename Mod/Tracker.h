#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "TrackerData.generated.h"

namespace bloodstained::tracker {

enum class Difficulty : std::uint8_t {
    NORMAL = 1,
    HARD = 2,
    NIGHTMARE = 4,
};

class Tracker {
   public:
    void SetInventory(const std::unordered_map<std::string, std::uint32_t>& inventory);

    std::vector<bool> GetReachableRegions(Difficulty difficulty) const;
    std::vector<const generated::RoomMapData*> GetReachableRooms(Difficulty difficulty) const;
    std::vector<const generated::LocationData*> GetReachableLocations(Difficulty difficulty) const;
    std::vector<const generated::LocationData*> GetReachableMissingLocations(
        Difficulty difficulty, const std::unordered_set<std::uint64_t>& missingLocations) const;
    std::vector<std::string_view> GetReachableEnemyRooms(const generated::LocationData& location,
                                                         Difficulty difficulty) const;
    static std::optional<std::string_view> FindNativeLocationName(std::uint64_t id);
    static const generated::RoomMapData* FindRoom(std::string_view name);
    static bool IsRoomCellVisible(const generated::RoomMapData& room, std::uint32_t roomAssignment);
    static bool IsTraversalItem(std::string_view name);

   private:
    struct ReachabilityState {
        std::vector<bool> regions;
        std::vector<std::uint32_t> inventory;
    };

    ReachabilityState EvaluateReachability(Difficulty difficulty) const;
    static bool IsRuleSatisfied(std::uint32_t rule, const std::vector<std::uint32_t>& inventory);
    static bool IncludesDifficulty(std::uint8_t mask, Difficulty difficulty);

    std::vector<std::uint32_t> inventory_ = std::vector<std::uint32_t>(generated::ITEMS.size(), 0);
};

}  // namespace bloodstained::tracker
