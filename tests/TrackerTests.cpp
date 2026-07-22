#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "Tracker.h"
#include "TrackerRuleEvaluator.h"

namespace {

using bloodstained::tracker::Difficulty;
using bloodstained::tracker::ItemRequirement;
using bloodstained::tracker::RuleEvaluator;
using bloodstained::tracker::RuleNode;
using bloodstained::tracker::RuleOperation;
using bloodstained::tracker::Tracker;

int failures = 0;

void Check(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

void TestRuleEvaluator() {
    constexpr std::array requirements = {
        ItemRequirement{0, 2},
        ItemRequirement{1, 1},
    };
    constexpr std::array<std::uint32_t, 8> operands = {0, 1, 1, 2, 0, 3, 0, 1};
    constexpr std::array nodes = {
        RuleNode{RuleOperation::NEVER, 0, 0, 0},
        RuleNode{RuleOperation::ALWAYS, 0, 0, 0},
        RuleNode{RuleOperation::HAS_ALL, 0, 2, 0},
        RuleNode{RuleOperation::HAS_ANY, 0, 2, 0},
        RuleNode{RuleOperation::AND, 2, 2, 0},
        RuleNode{RuleOperation::OR, 4, 2, 0},
        RuleNode{RuleOperation::COUNT, 6, 2, 3},
        RuleNode{RuleOperation::COUNT_UNIQUE, 6, 2, 2},
    };
    constexpr std::array<std::uint32_t, 2> empty_inventory = {0, 0};
    constexpr std::array<std::uint32_t, 2> partial_inventory = {2, 0};
    constexpr std::array<std::uint32_t, 2> full_inventory = {2, 1};
    const RuleEvaluator evaluator(requirements, operands, nodes);

    Check(!evaluator.Evaluate(0, full_inventory), "FALSE rule");
    Check(evaluator.Evaluate(1, empty_inventory), "TRUE rule");
    Check(!evaluator.Evaluate(2, partial_inventory), "HAS_ALL rejects a missing item");
    Check(evaluator.Evaluate(2, full_inventory), "HAS_ALL accepts all requirements");
    Check(!evaluator.Evaluate(3, empty_inventory), "HAS_ANY rejects an empty inventory");
    Check(evaluator.Evaluate(3, partial_inventory), "HAS_ANY accepts one requirement");
    Check(!evaluator.Evaluate(4, partial_inventory), "AND requires both child rules");
    Check(evaluator.Evaluate(4, full_inventory), "AND accepts both child rules");
    Check(!evaluator.Evaluate(5, empty_inventory), "OR rejects two false child rules");
    Check(evaluator.Evaluate(5, partial_inventory), "OR accepts one true child rule");
    Check(!evaluator.Evaluate(6, partial_inventory), "COUNT rejects an insufficient total");
    Check(evaluator.Evaluate(6, full_inventory), "COUNT sums item copies");
    Check(!evaluator.Evaluate(7, partial_inventory), "COUNT_UNIQUE rejects one item type");
    Check(evaluator.Evaluate(7, full_inventory), "COUNT_UNIQUE counts item types");
}

constexpr std::array<std::string_view, 90> CHECKED_SNAPSHOT = {{
    "Treasurebox_SIP000_Tutorial.0", "Treasurebox_SIP000_Tutorial.1", "Treasurebox_SIP002_1",
    "Treasurebox_SIP003_1", "Treasurebox_SIP004_1", "Treasurebox_SIP005_1", "Treasurebox_SIP005_2",
    "Treasurebox_SIP006_1", "Treasurebox_SIP007_1", "Treasurebox_SIP007_2", "Treasurebox_SIP009_1.0",
    "Treasurebox_SIP009_1.1", "Treasurebox_SIP009_1.2", "Treasurebox_SIP009_1.3", "Treasurebox_SIP011_1",
    "Treasurebox_SIP011_2.0", "Treasurebox_SIP011_2.1", "Treasurebox_SIP011_2.2",
    "Treasurebox_SIP011_2.3", "Treasurebox_SIP011_3", "Treasurebox_SIP011_4", "Treasurebox_SIP012_1.0",
    "Treasurebox_SIP012_1.1", "Treasurebox_SIP012_1.2", "Treasurebox_SIP012_1.3", "Treasurebox_SIP013_1",
    "Treasurebox_SIP015_1.0", "Treasurebox_SIP015_1.1", "Treasurebox_SIP015_1.2",
    "Treasurebox_SIP015_1.3", "Treasurebox_SIP016_1", "Treasurebox_SIP017_1.0",
    "Treasurebox_SIP017_1.1", "Treasurebox_SIP017_1.2", "Treasurebox_SIP017_1.3", "Treasurebox_SIP018_1",
    "Treasurebox_SIP020_1", "Treasurebox_SIP025_2", "Treasurebox_VIL003_1", "Treasurebox_VIL006_1",
    "Treasurebox_VIL006_2", "Treasurebox_VIL006_3", "Treasurebox_VIL007_1", "Treasurebox_VIL010_1",
    "Treasurebox_ENT002_1", "Treasurebox_ENT005_1", "Treasurebox_ENT005_2", "Treasurebox_ENT007_2.0",
    "Treasurebox_ENT007_2.1", "Treasurebox_ENT007_2.2", "Treasurebox_ENT007_2.3", "Treasurebox_ENT007_3",
    "Treasurebox_ENT009_1", "Treasurebox_ENT011_1", "Treasurebox_ENT014_1", "Treasurebox_ENT014_2",
    "Treasurebox_ENT014_3", "Treasurebox_ENT018_1.0", "Treasurebox_ENT018_1.1",
    "Treasurebox_ENT018_1.2", "Treasurebox_ENT018_1.3", "Treasurebox_ENT018_2.0",
    "Treasurebox_ENT018_2.1", "Treasurebox_ENT018_2.2", "Treasurebox_ENT018_2.3", "Treasurebox_ENT021_1",
    "Treasurebox_GDN002_1", "Treasurebox_GDN004_1", "Treasurebox_GDN006_2", "Treasurebox_SAN005_1",
    "Treasurebox_SAN005_2", "Treasurebox_SAN009_2", "Treasurebox_SAN013_1", "Treasurebox_SAN013_2",
    "Treasurebox_SAN014_1", "Treasurebox_UGD036_1.0", "Treasurebox_UGD036_1.1",
    "Treasurebox_UGD036_1.2", "Treasurebox_UGD036_1.3", "Treasurebox_UGD036_2", "Wall_SIP009_1",
    "Wall_SIP016_1", "Wall_ENT012_1", "Wall_SAN000_1", "Treasurebox_SIP019_1", "Treasurebox_SIP021_2",
    "Treasurebox_SIP025_1", "Treasurebox_VIL001_1", "Treasurebox_ENT002_2", "Treasurebox_ENT004_1",
}};

std::unordered_set<std::string_view> ReachableLocationNames(const Tracker& tracker, Difficulty difficulty) {
    std::unordered_set<std::string_view> names;
    for (const auto* location : tracker.GetReachableLocations(difficulty)) names.insert(location->name);
    return names;
}

void TestRealSnapshot() {
    Tracker tracker;
    tracker.SetInventory({{"Craftwork", 1}, {"Deep Sinker", 1}, {"Warhorse's Key", 1}});

    const auto normal = ReachableLocationNames(tracker, Difficulty::NORMAL);
    const auto hard = ReachableLocationNames(tracker, Difficulty::HARD);
    const auto nightmare = ReachableLocationNames(tracker, Difficulty::NIGHTMARE);
    std::cout << "Reachable locations for supplied inventory: normal=" << normal.size() << ", hard=" << hard.size()
              << ", nightmare=" << nightmare.size() << '\n';

    std::size_t reachable_checked = 0;
    for (std::string_view checked : CHECKED_SNAPSHOT) {
        if (hard.contains(checked)) {
            ++reachable_checked;
            continue;
        }
        std::cout << "Checked by the older snapshot but outside current Hard logic: " << checked << '\n';
    }
    Check(reachable_checked == 89, "True Randomizer door logic reaches 89 of the 90 checked snapshot locations");

    std::unordered_map<std::string, std::uint32_t> complete_inventory;
    for (std::string_view item : bloodstained::tracker::generated::ITEMS) complete_inventory.emplace(item, 1);
    tracker.SetInventory(complete_inventory);
    const auto complete = ReachableLocationNames(tracker, Difficulty::HARD);
    for (std::string_view checked : CHECKED_SNAPSHOT) {
        if (!complete.contains(checked)) {
            std::cerr << "Checked snapshot location is absent even with complete progression: " << checked << '\n';
            ++failures;
        }
    }

    tracker.SetInventory({{"Craftwork", 1}, {"Deep Sinker", 1}, {"Warhorse's Key", 1}});
    const auto reachable = tracker.GetReachableLocations(Difficulty::HARD);
    Check(!reachable.empty(), "real snapshot has reachable locations");
    if (!reachable.empty()) {
        const std::unordered_set<std::uint64_t> one_missing = {reachable.front()->id};
        const auto filtered = tracker.GetReachableMissingLocations(Difficulty::HARD, one_missing);
        Check(filtered.size() == 1 && filtered.front()->id == reachable.front()->id,
              "missing-location filtering retains only requested IDs");
    }
}

void TestGeneratedMapData() {
    const auto* start_room = Tracker::FindRoom("m01SIP_000");
    Check(start_room != nullptr, "start room has minimap geometry");
    if (start_room != nullptr) {
        Check(start_room->x == 2 && start_room->z == 0, "start room uses integer minimap coordinates");
        Check(start_room->width == 2 && start_room->height == 1, "start room has the expected dimensions");
        Check(!start_room->out_of_map, "start room is on the main map");
        Check(Tracker::IsRoomCellVisible(*start_room, 1), "ordinary room cell is visible");
    }
    const auto* waterway_room = Tracker::FindRoom("m11UGD_013");
    Check(waterway_room != nullptr, "non-rectangular Waterway room has minimap geometry");
    if (waterway_room != nullptr) {
        Check(!Tracker::IsRoomCellVisible(*waterway_room, 1), "native-hidden Waterway cell is trimmed");
        Check(Tracker::IsRoomCellVisible(*waterway_room, 4), "visible Waterway cell remains rendered");
    }
    Check(Tracker::FindRoom("not_a_room") == nullptr, "unknown room lookup fails cleanly");
    Check(Tracker::IsTraversalItem("Double Jump"), "generated traversal item is recognized");
    Check(Tracker::IsTraversalItem("Doublejump"), "native traversal shard ID is recognized");
    Check(Tracker::IsTraversalItem("Demoniccapture"), "native progression shard ID is recognized");
    Check(!Tracker::IsTraversalItem("Nothing"), "non-progression item is not traversal-relevant");

    Tracker tracker;
    tracker.SetInventory({});
    const auto reachable_rooms = tracker.GetReachableRooms(Difficulty::NORMAL);
    Check(std::ranges::any_of(reachable_rooms, [](const auto* room) { return room->name == "m01SIP_000"; }),
          "reachable-room projection contains the starting room");

    const auto wall = std::find_if(
        bloodstained::tracker::generated::LOCATIONS.begin(), bloodstained::tracker::generated::LOCATIONS.end(),
        [](const auto& location) { return location.name == "Wall_SIP009_1"; });
    Check(wall != bloodstained::tracker::generated::LOCATIONS.end(), "wall fixture exists");
    if (wall != bloodstained::tracker::generated::LOCATIONS.end()) {
        Check(wall->type == bloodstained::tracker::generated::LocationType::WALL,
              "wall checks are exported separately from chests");
        Check(wall->map_x > 0.04f && wall->map_x < 0.05f,
              "wall checks carry their room-local horizontal map position");
        Check(wall->map_z > 1.66f && wall->map_z < 1.67f,
              "wall checks carry their room-local vertical map position");
    }
}

void TestGalleonOpeningHeightGate() {
    Tracker tracker;
    tracker.SetInventory({});
    const auto without_height = ReachableLocationNames(tracker, Difficulty::NORMAL);
    Check(!without_height.contains("Treasurebox_SIP024_1"),
          "upper room above the Galleon spawn is blocked without a height ability");
    Check(!without_height.contains("Treasurebox_SIP024_2"),
          "both chests above the Galleon spawn are blocked without a height ability");

    tracker.SetInventory({{"Double Jump", 1}});
    const auto with_double_jump = ReachableLocationNames(tracker, Difficulty::NORMAL);
    Check(with_double_jump.contains("Treasurebox_SIP024_1"),
          "Double Jump opens the upper room above the Galleon spawn");
    Check(with_double_jump.contains("Treasurebox_SIP024_2"),
          "Double Jump opens both chests above the Galleon spawn");
}

}  // namespace

int main() {
    TestRuleEvaluator();
    TestRealSnapshot();
    TestGeneratedMapData();
    TestGalleonOpeningHeightGate();
    if (failures != 0) {
        std::cerr << failures << " tracker test(s) failed\n";
        return 1;
    }
    std::cout << "All tracker tests passed\n";
    return 0;
}
