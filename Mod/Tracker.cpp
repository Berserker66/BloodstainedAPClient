#include "Tracker.h"

#include <algorithm>

namespace bloodstained::tracker {

void Tracker::SetInventory(const std::unordered_map<std::string, std::uint32_t>& inventory) {
    std::fill(inventory_.begin(), inventory_.end(), 0);
    for (std::size_t i = 0; i < generated::ITEMS.size(); ++i) {
        auto item = inventory.find(std::string(generated::ITEMS[i]));
        if (item != inventory.end()) inventory_[i] = item->second;
    }
}

std::vector<bool> Tracker::GetReachableRegions(Difficulty difficulty) const {
    std::vector<bool> reachable(generated::REGIONS.size(), false);
    reachable[generated::START_REGION] = true;

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& entrance : generated::ENTRANCES) {
            if (!IncludesDifficulty(entrance.difficulties, difficulty) || !reachable[entrance.source] ||
                reachable[entrance.target] || !IsRuleSatisfied(entrance.rule)) {
                continue;
            }
            reachable[entrance.target] = true;
            changed = true;
        }
    }
    return reachable;
}

std::vector<const generated::RoomMapData*> Tracker::GetReachableRooms(Difficulty difficulty) const {
    const std::vector<bool> reachableRegions = GetReachableRegions(difficulty);
    std::vector<const generated::RoomMapData*> reachableRooms;
    for (std::size_t region = 0; region < reachableRegions.size(); ++region) {
        if (!reachableRegions[region] || generated::REGION_ROOMS[region].empty()) continue;
        const generated::RoomMapData* room = FindRoom(generated::REGION_ROOMS[region]);
        if (room != nullptr) reachableRooms.push_back(room);
    }
    std::ranges::sort(reachableRooms, {}, [](const generated::RoomMapData* room) { return room->name; });
    reachableRooms.erase(
        std::ranges::unique(reachableRooms, {}, [](const generated::RoomMapData* room) { return room->name; }).begin(),
        reachableRooms.end());
    return reachableRooms;
}

std::vector<const generated::LocationData*> Tracker::GetReachableLocations(Difficulty difficulty) const {
    const std::vector<bool> reachableRegions = GetReachableRegions(difficulty);
    std::vector<const generated::LocationData*> reachableLocations;
    for (const auto& location : generated::LOCATIONS) {
        if (IncludesDifficulty(location.difficulties, difficulty) && reachableRegions[location.region] &&
            IsRuleSatisfied(location.rule)) {
            reachableLocations.push_back(&location);
        }
    }
    return reachableLocations;
}

std::vector<const generated::LocationData*> Tracker::GetReachableMissingLocations(
    Difficulty difficulty, const std::unordered_set<std::uint64_t>& missingLocations) const {
    std::vector<const generated::LocationData*> reachableMissing;
    for (const generated::LocationData* location : GetReachableLocations(difficulty)) {
        if (missingLocations.contains(location->id)) reachableMissing.push_back(location);
    }
    return reachableMissing;
}

std::vector<std::string_view> Tracker::GetReachableEnemyRooms(const generated::LocationData& location,
                                                              Difficulty difficulty) const {
    std::vector<std::string_view> rooms;
    if (location.type != generated::LocationType::ENEMY) return rooms;

    const std::vector<bool> reachableRegions = GetReachableRegions(difficulty);
    for (const auto& entrance : generated::ENTRANCES) {
        if (entrance.target == location.region && IncludesDifficulty(entrance.difficulties, difficulty) &&
            reachableRegions[entrance.source] && IsRuleSatisfied(entrance.rule)) {
            const std::string_view room = generated::REGION_ROOMS[entrance.source];
            if (!room.empty()) rooms.push_back(room);
        }
    }
    std::sort(rooms.begin(), rooms.end());
    rooms.erase(std::unique(rooms.begin(), rooms.end()), rooms.end());
    return rooms;
}

std::optional<std::string_view> Tracker::FindNativeLocationName(std::uint64_t id) {
    const auto binding = std::lower_bound(
        generated::LOCATION_BINDINGS.begin(), generated::LOCATION_BINDINGS.end(), id,
        [](const generated::IdentifierBinding& candidate, std::uint64_t value) { return candidate.id < value; });
    if (binding == generated::LOCATION_BINDINGS.end() || binding->id != id) return std::nullopt;
    return binding->native_name;
}

const generated::RoomMapData* Tracker::FindRoom(std::string_view name) {
    const auto room = std::lower_bound(generated::ROOMS.begin(), generated::ROOMS.end(), name,
                                       [](const generated::RoomMapData& candidate, std::string_view value) {
                                           return candidate.name < value;
                                       });
    return room != generated::ROOMS.end() && room->name == name ? &*room : nullptr;
}

bool Tracker::IsRoomCellVisible(const generated::RoomMapData& room, std::uint32_t roomAssignment) {
    const auto first = generated::HIDDEN_ROOM_CELLS.begin() + room.hidden_cell_offset;
    const auto last = first + room.hidden_cell_count;
    return std::find(first, last, roomAssignment) == last;
}

bool Tracker::IsTraversalItem(std::string_view name) {
    return std::ranges::find(generated::TRAVERSAL_ITEMS, name) != generated::TRAVERSAL_ITEMS.end() ||
           std::ranges::find(generated::TRAVERSAL_NATIVE_ITEMS, name,
                             &generated::NativeTraversalItemData::native_id) !=
               generated::TRAVERSAL_NATIVE_ITEMS.end();
}

bool Tracker::IsRuleSatisfied(std::uint32_t rule) const {
    static const RuleEvaluator evaluator(generated::REQUIREMENTS, generated::OPERANDS, generated::NODES);
    return evaluator.Evaluate(rule, inventory_);
}

bool Tracker::IncludesDifficulty(std::uint8_t mask, Difficulty difficulty) {
    return (mask & static_cast<std::uint8_t>(difficulty)) != 0;
}

}  // namespace bloodstained::tracker
