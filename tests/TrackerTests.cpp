#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "EnemyDropShuffleLogic.h"
#include "QualityOfLifeLogic.h"
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

void TestExcessShardQuantity() {
    using bloodstained::qol::ExcessShardQuantity;
    using bloodstained::qol::ShardSaleQuantity;

    Check(ExcessShardQuantity(8, 1) == 0, "grade 8 to 9 is not wasted");
    Check(ExcessShardQuantity(9, 1) == 1, "a shard received at grade 9 is wasted");
    Check(ExcessShardQuantity(8, 2) == 1, "only the copy above grade 9 is wasted");
    Check(ExcessShardQuantity(9, 3) == 3, "all copies received at grade 9 are wasted");
    Check(ExcessShardQuantity(9, 0) == 0, "zero incoming shards are ignored");
    Check(ExcessShardQuantity(9, -1) == 0, "negative incoming quantities are ignored");
    Check(ShardSaleQuantity(8, 1, false) == 0, "ordinary grade 8 to 9 remains an inventory grant");
    Check(ShardSaleQuantity(9, 1, false) == 1, "ordinary grade 9 to 10 is sold");
    Check(ShardSaleQuantity(0, 1, true) == 1, "forced AP repeat is sold from grade 0");
    Check(ShardSaleQuantity(8, 1, true) == 1, "forced AP repeat is sold from grade 8");
    Check(ShardSaleQuantity(9, 1, true) == 1, "forced AP repeat is sold from grade 9");
    Check(ShardSaleQuantity(0, 0, true) == 0, "forced sale ignores zero quantity");
    Check(ShardSaleQuantity(0, -1, true) == 0, "forced sale ignores negative quantity");
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
    "Treasurebox_SIP025_1", "Treasurebox_VIL001_1", "Treasurebox_ENT002_3", "Treasurebox_ENT004_1",
}};

std::unordered_set<std::string_view> ReachableLocationNames(const Tracker& tracker, Difficulty difficulty) {
    std::unordered_set<std::string_view> names;
    for (const auto* location : tracker.GetReachableLocations(difficulty)) {
        const auto binding = std::lower_bound(
            bloodstained::tracker::generated::LOCATION_BINDINGS.begin(),
            bloodstained::tracker::generated::LOCATION_BINDINGS.end(), location->id,
            [](const auto& entry, std::uint64_t id) { return entry.id < id; });
        if (binding == bloodstained::tracker::generated::LOCATION_BINDINGS.end() || binding->id != location->id) {
            continue;
        }
        names.insert(binding->native_name);
        if (binding->native_name.ends_with(".0")) {
            names.insert(binding->native_name.substr(0, binding->native_name.size() - 2));
        }
    }
    return names;
}

const bloodstained::tracker::generated::LocationData* FindLocationByNativeName(std::string_view native_name) {
    const auto binding = std::find_if(
        bloodstained::tracker::generated::LOCATION_BINDINGS.begin(),
        bloodstained::tracker::generated::LOCATION_BINDINGS.end(),
        [native_name](const auto& entry) { return entry.native_name == native_name; });
    if (binding == bloodstained::tracker::generated::LOCATION_BINDINGS.end()) return nullptr;
    const auto location = std::lower_bound(
        bloodstained::tracker::generated::LOCATIONS.begin(),
        bloodstained::tracker::generated::LOCATIONS.end(), binding->id,
        [](const auto& entry, std::uint64_t id) { return entry.id < id; });
    return location != bloodstained::tracker::generated::LOCATIONS.end() && location->id == binding->id
               ? &*location
               : nullptr;
}

void TestRealSnapshot() {
    Tracker tracker;
    tracker.SetInventory({{"Craftwork", 1}, {"Deep Sinker", 1}, {"Warhorse's Key", 1}});

    const auto normal = ReachableLocationNames(tracker, Difficulty::NORMAL);
    const auto hard = ReachableLocationNames(tracker, Difficulty::HARD);
    const auto nightmare = ReachableLocationNames(tracker, Difficulty::NIGHTMARE);
    std::cout << "Reachable locations for supplied inventory: normal=" << normal.size() << ", hard=" << hard.size()
              << ", nightmare=" << nightmare.size() << '\n';

    std::unordered_set<std::string_view> unavailable_checked;
    for (std::string_view checked : CHECKED_SNAPSHOT) {
        if (!hard.contains(checked)) unavailable_checked.insert(checked);
    }
    for (std::string_view checked : unavailable_checked) {
        std::cout << "Checked by the older snapshot but outside current Hard logic: " << checked << '\n';
    }
    Check(unavailable_checked.empty(),
          "current logic reaches every location checked by the True Randomizer snapshot");

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
    const auto oleanders_level_two = std::lower_bound(
        bloodstained::tracker::generated::ITEM_BINDINGS.begin(),
        bloodstained::tracker::generated::ITEM_BINDINGS.end(), 725676ull,
        [](const auto& binding, std::uint64_t id) { return binding.id < id; });
    Check(oleanders_level_two != bloodstained::tracker::generated::ITEM_BINDINGS.end() &&
              oleanders_level_two->id == 725676ull &&
              oleanders_level_two->native_name == "PoisonSpikeShoes2" &&
              oleanders_level_two->display_name == "Oleanders Lv2",
          "levelled equipment protocol IDs bind to distinct native catalog rows");

    std::unordered_set<std::uint64_t> item_ids;
    std::unordered_set<std::string_view> item_native_names;
    std::unordered_set<std::string_view> item_display_names;
    for (const auto& binding : bloodstained::tracker::generated::ITEM_BINDINGS) {
        Check(!binding.native_name.empty(), "every AP item has a native identifier");
        Check(!binding.display_name.empty(), "every AP item has a display-name alias");
        Check(item_ids.insert(binding.id).second, "AP item protocol IDs are unique");
        Check(item_native_names.insert(binding.native_name).second, "AP native item identifiers are unique");
        Check(item_display_names.insert(binding.display_name).second, "AP item display names are unique");
    }

    const auto ribbon = std::lower_bound(
        bloodstained::tracker::generated::ITEM_BINDINGS.begin(),
        bloodstained::tracker::generated::ITEM_BINDINGS.end(), 725823ull,
        [](const auto& binding, std::uint64_t id) { return binding.id < id; });
    Check(ribbon != bloodstained::tracker::generated::ITEM_BINDINGS.end() &&
              ribbon->native_name == "ribbon" && ribbon->display_name == "Ribbon",
          "Ribbon binds to its case-sensitive native catalog identifier");

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

    const auto* wall = FindLocationByNativeName("Wall_SIP009_1");
    Check(wall != nullptr, "wall fixture exists");
    if (wall != nullptr) {
        const auto native_name = Tracker::FindNativeLocationName(wall->id);
        Check(native_name && *native_name == "Wall_SIP009_1",
              "canonical wall location resolves back to its native protocol name");
        Check(native_name && wall->name != *native_name,
              "player-facing wall name remains distinct from its native protocol name");
        Check(wall->type == bloodstained::tracker::generated::LocationType::WALL,
              "wall checks are exported separately from chests");
        Check(wall->map_x > 0.04f && wall->map_x < 0.05f,
              "wall checks carry their room-local horizontal map position");
        Check(wall->map_z > 1.66f && wall->map_z < 1.67f,
              "wall checks carry their room-local vertical map position");
    }

    const auto* chest = FindLocationByNativeName("Treasurebox_SIP000_Tutorial.0");
    Check(chest != nullptr, "chest fixture exists");
    if (chest != nullptr) {
        const auto native_name = Tracker::FindNativeLocationName(chest->id);
        Check(native_name && *native_name == "Treasurebox_SIP000_Tutorial.0",
              "canonical chest location resolves back to its native protocol name");
    }

    const auto* hidden_room_left = FindLocationByNativeName("Treasurebox_SIP025_1.0");
    const auto* hidden_room_right = FindLocationByNativeName("Treasurebox_SIP025_2.0");
    Check(hidden_room_left && hidden_room_left->has_map_position,
          "Galleon hidden-room left chest has an authoritative map position");
    Check(hidden_room_right && hidden_room_right->has_map_position,
          "Galleon hidden-room right chest has an authoritative map position");
    if (hidden_room_left && hidden_room_right) {
        Check(hidden_room_left->map_x > 0.42f && hidden_room_left->map_x < 0.44f,
              "Galleon hidden-room left chest uses its cooked actor position");
        Check(hidden_room_right->map_x > 0.79f && hidden_room_right->map_x < 0.80f,
              "Galleon hidden-room right chest uses its cooked actor position");
        Check(hidden_room_left->map_x < hidden_room_right->map_x,
              "Galleon hidden-room chest markers preserve left-to-right order");
    }

    constexpr std::array synthetic_capacity_pickups = {
        std::pair{"Treasurebox_ENT014_2.0", std::pair{1.380952f, 0.083333f}},
        std::pair{"Treasurebox_ENT014_3.0", std::pair{0.626984f, 1.083333f}},
        std::pair{"Treasurebox_GDN002_1.0", std::pair{0.142857f, 0.833333f}},
    };
    for (const auto& [native_name, expected_position] : synthetic_capacity_pickups) {
        const auto* location = FindLocationByNativeName(native_name);
        Check(location && location->has_map_position,
              "capacity pickup omitted from native marker arrays has a synthetic map position");
        if (!location) continue;
        Check(std::abs(location->map_x - expected_position.first) < 0.00001f,
              "synthetic capacity pickup uses its cooked horizontal actor position");
        Check(std::abs(location->map_z - expected_position.second) < 0.00001f,
              "synthetic capacity pickup uses its cooked vertical actor position");
    }

    const auto* shard = FindLocationByNativeName("N3007_Shard");
    Check(shard != nullptr, "shard fixture exists");
    if (shard != nullptr) {
        const auto native_name = Tracker::FindNativeLocationName(shard->id);
        Check(native_name && *native_name == "N3007_Shard",
              "canonical shard location resolves back to its native protocol name");
    }
    Check(!Tracker::FindNativeLocationName(0), "unknown location ID has no native protocol name");

    const bool all_locations_have_native_names =
        std::ranges::all_of(bloodstained::tracker::generated::LOCATIONS, [](const auto& location) {
            return Tracker::FindNativeLocationName(location.id).has_value();
        });
    Check(all_locations_have_native_names,
          "every player-facing tracker location resolves to a native protocol name");

    constexpr std::int32_t TRAVERSE_WIDTH = 200;
    constexpr std::int32_t TRAVERSE_HEIGHT = 100;
    constexpr std::int32_t ROOM_MAP_TO_TRAVERSE_Z = 50;
    for (const auto& room : bloodstained::tracker::generated::ROOMS) {
        if (room.out_of_map) continue;
        Check(room.x >= 0 && room.x + room.width <= TRAVERSE_WIDTH,
              "visible room geometry fits the traversal ledger horizontally");
        Check(room.z + ROOM_MAP_TO_TRAVERSE_Z >= 0 &&
                  room.z + ROOM_MAP_TO_TRAVERSE_Z + room.height <= TRAVERSE_HEIGHT,
              "visible room geometry fits the traversal ledger vertically");
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

void TestShardRoomCoverage() {
    Tracker tracker;
    std::unordered_map<std::string, std::uint32_t> complete_inventory;
    for (const std::string_view item : bloodstained::tracker::generated::ITEMS) {
        complete_inventory.emplace(item, 99u);
    }
    tracker.SetInventory(complete_inventory);

    const auto* shard = FindLocationByNativeName("N3109_Shard");
    Check(shard != nullptr, "Livre shard-room fixture exists");
    if (!shard) return;
    const auto rooms = tracker.GetReachableEnemyRooms(*shard, Difficulty::NORMAL);
    for (const std::string_view expected : {
             "m07LIB_012", "m07LIB_015", "m07LIB_020", "m07LIB_034"}) {
        Check(std::ranges::find(rooms, expected) != rooms.end(),
              "all base Livre rooms containing the shard enemy are exported");
    }
}

void TestScriptedAreaGates() {
    Tracker tracker;
    tracker.SetInventory({{"Zangetsuto", 1}});
    const auto zangetsuto_only = tracker.GetReachableRooms(Difficulty::NORMAL);
    Check(!std::ranges::any_of(zangetsuto_only, [](const auto* room) { return room->name.starts_with("m10BIG_"); }),
          "Zangetsuto alone does not open the Den portal");
    Check(!std::ranges::any_of(zangetsuto_only, [](const auto* room) { return room->name.starts_with("m20JRN_"); }),
          "Zangetsuto alone does not light The Tunnels through the Den");

    tracker.SetInventory({{"Den Portal Open", 1}});
    const auto injected_event = tracker.GetReachableRooms(Difficulty::NORMAL);
    Check(!std::ranges::any_of(injected_event, [](const auto* room) { return room->name.starts_with("m10BIG_"); }),
          "derived tracker events cannot be injected as player inventory");
}

void TestBackerRoomDoorKeys() {
    Tracker tracker;
    std::unordered_map<std::string, std::uint32_t> complete_inventory;
    for (const std::string_view item : bloodstained::tracker::generated::ITEMS) {
        complete_inventory.emplace(item, 99u);
    }

    for (const auto& [key, room] : {
             std::pair<std::string_view, std::string_view>{"Warhorse's Key", "m88BKR_001"},
             {"Millionaire's Key", "m88BKR_002"},
             {"Carpenter's Key", "m88BKR_004"},
         }) {
        auto inventory_without_key = complete_inventory;
        inventory_without_key.erase(std::string(key));
        tracker.SetInventory(inventory_without_key);
        const auto without_key = tracker.GetReachableRooms(Difficulty::NORMAL);
        Check(!std::ranges::any_of(without_key, [room](const auto* reachable) { return reachable->name == room; }),
              "a locked backer room is unreachable without its door key");

        tracker.SetInventory(complete_inventory);
        const auto with_key = tracker.GetReachableRooms(Difficulty::NORMAL);
        Check(std::ranges::any_of(with_key, [room](const auto* reachable) { return reachable->name == room; }),
              "a locked backer room is reachable with its door key");
    }
}

std::uint64_t HashDropShuffle(const bloodstained::enemy_drop_shuffle::ShuffleResult& result) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto& enemy : result.enemies) {
        for (const char character : enemy.enemy) {
            hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ull;
        }
        for (std::size_t slot = 0; slot < enemy.items.size(); ++slot) {
            hash = (hash ^ static_cast<std::uint64_t>(enemy.active[slot])) * 1099511628211ull;
            if (!enemy.active[slot]) continue;
            for (const char character : enemy.items[slot]) {
                hash = (hash ^ static_cast<unsigned char>(character)) * 1099511628211ull;
            }
        }
    }
    return hash;
}

void TestEnemyDropShuffle() {
    const auto firstSlotSeed = bloodstained::enemy_drop_shuffle::DeriveSeed("Seed123", 7);
    Check(firstSlotSeed == 1062718794u, "drop seed derivation matches its stable test vector");
    Check(firstSlotSeed == bloodstained::enemy_drop_shuffle::DeriveSeed("Seed123", 7),
          "drop seed is stable for a room seed and slot");
    Check(firstSlotSeed != bloodstained::enemy_drop_shuffle::DeriveSeed("Seed123", 8),
          "drop seed changes for a different slot");
    Check(firstSlotSeed != bloodstained::enemy_drop_shuffle::DeriveSeed("Seed124", 7),
          "drop seed changes for a different room seed");

    auto generate = [](std::uint32_t seed) {
        return bloodstained::enemy_drop_shuffle::Generate(
            [state = seed](std::uint32_t maxExclusive) mutable {
                state = state * 1664525u + 1013904223u;
                return state % maxExclusive;
            });
    };

    const auto first = generate(0x12345678u);
    const auto repeated = generate(0x12345678u);
    const auto different = generate(0x87654321u);
    Check(first.coversVanillaDrops, "enemy drop shuffle preserves every vanilla enemy drop");
    Check(first.enemies.size() == std::size(bloodstained::enemy_drop_data::ELIGIBLE_ENEMIES),
          "enemy drop shuffle covers every eligible enemy");
    Check(HashDropShuffle(first) == HashDropShuffle(repeated),
          "enemy drop shuffle is deterministic for a fixed RNG stream");
    Check(HashDropShuffle(first) != HashDropShuffle(different),
          "enemy drop shuffle changes for a different RNG stream");
}

}  // namespace

int main() {
    TestRuleEvaluator();
    TestExcessShardQuantity();
    TestRealSnapshot();
    TestGeneratedMapData();
    TestGalleonOpeningHeightGate();
    TestShardRoomCoverage();
    TestScriptedAreaGates();
    TestBackerRoomDoorKeys();
    TestEnemyDropShuffle();
    if (failures != 0) {
        std::cerr << failures << " tracker test(s) failed\n";
        return 1;
    }
    std::cout << "All tracker tests passed\n";
    return 0;
}
