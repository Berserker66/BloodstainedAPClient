#pragma once
#include "Archipelago.h"

#include <Basic.hpp>
#include <Engine_classes.hpp>
#include <ProjectBlood_classes.hpp>
#include <ProjectBlood_structs.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <nlohmann/json.hpp>
#include <numeric>
#include <set>
#include <string_view>
#include <utility>

#include "ClientVersion.h"
#include "ConnectionUri.h"
#include "EnemyDropShuffle.h"
#include "EnemyDropShuffleLogic.h"
#include "GameManager.h"
#include "HookManager.h"
#include "InGameTracker.h"
#include "Logger.h"
#include "MainMenuStatus.h"
#include "ProgressiveItems.h"
#include "ThreadQueue.h"
#include "TrackerData.generated.h"
#include "Utils.h"
#include "apclient.hpp"
#include "apuuid.hpp"

std::unique_ptr<APClient> ap;
bool ap_slot_connect_sent = false;
double deathtime = -1;
bool awaiting_password = false;

const std::string GAME_NAME = "Bloodstained: Ritual of the Night";
const APClient::Version CLIENT_VERSION = {ClientVersion::ArchipelagoProtocol::Major,
                                          ClientVersion::ArchipelagoProtocol::Minor,
                                          ClientVersion::ArchipelagoProtocol::Build};
std::string game_seed;
const int MAX_SEED_LENGTH = 32;

const std::string LEGACY_ITEM_INDEX_SAVE_VALUE = "ItemIndex";
const std::string LEGACY_ITEM_LEDGER_VERSION_VALUE = "ItemLedgerVersion";
const std::string ENTITLEMENT_LEDGER_VERSION_VALUE = "EntitlementLedgerVersion";
const std::string ENTITLEMENT_LEDGER_COUNT_VALUE = "EntitlementLedgerCount";
const std::string CONNECTION_SAVE_PREFIX = "AP_LastConnection_";
const std::string CLEARED_LOCATION_SAVE_PREFIX = "AP_ClearedLocation_";
const std::string PENDING_CLEARED_LOCATION_SAVE_PREFIX = "AP_PendingClearedLocation_";
const std::string ENEMY_DROP_SHUFFLE_SEED_VALUE = "AP_EnemyDropShuffleSeed";
const std::string ENEMY_DROP_SHUFFLE_VERSION_VALUE = "AP_EnemyDropShuffleVersion";
const std::string STATIC_PAK_SAVE_SCHEMA_VALUE = "AP_StaticPakSaveSchema";
const std::string LEGACY_CONTENT_POLICY = "base-and-free-content-v1";
const std::string CONTENT_POLICY = "base-free-and-optional-paid-dlc-v2";
const int32_t STATIC_PAK_SCHEMA = 1;
const int32_t CONNECTION_SAVE_VERSION = 3;
const int32_t LEGACY_BOOLEAN_DEATHLINK_CONNECTION_SAVE_VERSION = 2;
const int32_t ENTITLEMENT_LEDGER_VERSION = 1;
const int32_t MAX_ITEM_LEDGER_ENTRIES = 4096;
const int32_t LEGACY_SAVE_SLOT_SCAN_LIMIT = 100;
const size_t MAX_CONNECTION_FIELD_LENGTH = 1024;
const int32_t MAX_CLEARED_LOCATIONS = 4096;
const std::chrono::milliseconds ITEM_GRANT_INTERVAL(100);
const std::chrono::milliseconds ITEM_GRANT_RETRY_DELAY(500);

// These enemies award their shard exactly once, as part of their boss defeat sequence. If the client was not able
// to observe that short-lived shard actor, the save's completed-boss flag is the authoritative durable evidence
// that the corresponding location was cleared.
struct BossShardLocation {
    std::string_view boss_id;
    std::string_view location_id;
};

constexpr std::array BOSS_SHARD_LOCATIONS = {
    BossShardLocation{"N1001", "N1001_Shard"}, BossShardLocation{"N1002", "N1002_Shard"},
    BossShardLocation{"N1003", "N1003_Shard"}, BossShardLocation{"N1004", "N1004_Shard"},
    BossShardLocation{"N1005", "N1005_Shard"}, BossShardLocation{"N1006", "N1006_Shard"},
    BossShardLocation{"N1008", "N1008_Shard"}, BossShardLocation{"N2001", "N2001_Shard"},
    BossShardLocation{"N2004", "N2004_Shard"}, BossShardLocation{"N2006", "N2006_Shard"},
    BossShardLocation{"N2007", "N2007_Shard"}, BossShardLocation{"N2012", "N2012_Shard"},
    BossShardLocation{"N2013", "N2013_Shard"}, BossShardLocation{"N2016", "FamiliaArcher_Shard"},
    BossShardLocation{"N2017", "N2017_Shard"},
};

#define UUID_FILE "uuid"
#define CERT_STORE "cacert.pem"

using json = nlohmann::json;

static std::optional<int32_t> LoadSavedValue(const std::string& saveValueName);
static std::optional<std::string> LoadSavedString(const std::string& saveValueName);

static std::string_view GetCurrentDifficultyName() {
    switch (SDK::UPBGameInstance::GetGameLevel()) {
        case SDK::EPBGameLevel::Hard:
            return "hard";
        case SDK::EPBGameLevel::Nightmare:
            return "nightmare";
        default:
            return "normal";
    }
}

static std::optional<int64_t> LoadInt64(const std::string& name) {
    const auto low = LoadSavedValue(name + "Low");
    const auto high = LoadSavedValue(name + "High");
    if (!low || !high) return std::nullopt;
    const uint64_t value = static_cast<uint32_t>(*low) | (static_cast<uint64_t>(static_cast<uint32_t>(*high)) << 32);
    return static_cast<int64_t>(value);
}

static bool IsShardOrSkillItem(const std::string& itemId) {
    if (GameManager::Instance().IsPaidDlcShard(itemId)) return true;
    return GameManager::Instance().ItemHasItemCategories(
        itemId, {SDK::ECarriedCatalog::AllShard, SDK::ECarriedCatalog::TriggerShard,
                 SDK::ECarriedCatalog::DirectionalShard, SDK::ECarriedCatalog::EffectiveShard,
                 SDK::ECarriedCatalog::EnchantShard, SDK::ECarriedCatalog::FamiliarShard,
                 SDK::ECarriedCatalog::Skill});
}

static std::optional<std::string_view> GetNativeItemName(int64_t itemId) {
    const auto& bindings = bloodstained::tracker::generated::ITEM_BINDINGS;
    const auto binding = std::lower_bound(bindings.begin(), bindings.end(), static_cast<std::uint64_t>(itemId),
                                          [](const auto& entry, std::uint64_t id) { return entry.id < id; });
    if (binding == bindings.end() || binding->id != static_cast<std::uint64_t>(itemId)) return std::nullopt;
    return binding->native_name;
}

static bool IsKnownItemId(int64_t itemId) {
    return GetNativeItemName(itemId).has_value() ||
           bloodstained::items::FindProgressiveItem(static_cast<std::uint64_t>(itemId)) != nullptr;
}

static std::optional<int64_t> GetItemId(std::string_view nativeName) {
    const auto& bindings = bloodstained::tracker::generated::ITEM_BINDINGS;
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [nativeName](const auto& entry) {
        return entry.native_name == nativeName;
    });
    return binding == bindings.end() ? std::nullopt : std::optional<int64_t>(binding->id);
}

static std::optional<int64_t> GetLocationId(std::string_view nativeName) {
    const auto& bindings = bloodstained::tracker::generated::LOCATION_BINDINGS;
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [nativeName](const auto& entry) {
        return entry.native_name == nativeName;
    });
    return binding == bindings.end() ? std::nullopt : std::optional<int64_t>(binding->id);
}

static std::string GetDisplayItemName(int64_t itemId) {
    if (ap && ap->is_data_package_valid()) return ap->get_item_name(itemId, ap->get_game());
    const auto nativeName = GetNativeItemName(itemId);
    if (nativeName) return std::string(*nativeName);
    const auto* progressive = bloodstained::items::FindProgressiveItem(static_cast<std::uint64_t>(itemId));
    return progressive ? std::string(progressive->display_name) : "Unknown item " + std::to_string(itemId);
}

Archipelago& Archipelago::Instance() {
    static Archipelago instance;
    return instance;
}

Archipelago* Archipelago::ConnectedInstance() {
    if (!ap || !Instance().IsConnected()) {
        return nullptr;
    }
    return &Instance();
}

Archipelago::Archipelago() : state_(ArchipelagoConnectionState::Disconnected) {}

Archipelago::~Archipelago() {}

void Archipelago::ExecuteConsoleCommand(const char* command) { GameManager::Instance().GivePlayerItem(command); }

void Archipelago::ConnectSlot() {
    bool _connected = false;
    if (ap) {
        if (state_ == ArchipelagoConnectionState::Connected) {
            std::list<std::string> tags;
            if (deathLinkMode_ != DeathLinkMode::Off) {
                tags.push_back("DeathLink");
            }
            _connected = ap->ConnectSlot(slotName_, password_, itemsHandling_, tags, CLIENT_VERSION);
            if (_connected) {
                ap_slot_connect_sent = true;
                Logger::Log("[AP] Connection from here was successful.");
                return;
            } else {
                Logger::Log("[AP] Max retries reached. Giving up.");
                currentUri_.clear();
            }
        }
    } else {
        Logger::Log(LogLevel::Debug, "[AP]", "Connection lost.");
    }
}

std::unordered_map<std::string, std::uint32_t> Archipelago::GetTrackerInventory() const {
    std::unordered_map<std::string, std::uint32_t> inventory;
    if (!ap || !IsConnected()) return inventory;

    std::unordered_map<int64_t, std::uint32_t> occurrences;
    for (const auto& [index, item] : receivedItems_) {
        // ReceivedItems is the server's complete delivery history, including entries which are still waiting
        // behind a rejected native grant. Reachability must follow the durable award ledger, not run ahead of
        // the player's actual inventory merely because a later item is already present in that history.
        const auto awarded = awardedItemCounts_.find(item.item);
        const std::uint32_t awardedOccurrences =
            awarded == awardedItemCounts_.end() ? 0u : awarded->second;
        if (++occurrences[item.item] > awardedOccurrences) continue;
        const std::string itemName = GetDisplayItemName(item.item);
        if (!itemName.empty()) inventory[itemName]++;
    }
    return inventory;
}

std::unordered_set<std::uint64_t> Archipelago::GetMissingLocationIds() const {
    std::unordered_set<std::uint64_t> missingLocations;
    if (!ap || !IsConnected()) return missingLocations;

    const auto serverMissingLocations = ap->get_missing_locations();
    missingLocations.insert(serverMissingLocations.begin(), serverMissingLocations.end());
    return missingLocations;
}

bool Archipelago::IsMissingLocation(const std::string& locationName, std::uint64_t expectedId) const {
    if (!ap || !IsConnected()) return false;
    const auto locationId = GetLocationId(locationName);
    return locationId && static_cast<std::uint64_t>(*locationId) == expectedId &&
           ap->get_missing_locations().contains(*locationId);
}

bool Archipelago::WasLocationClearedLocally(const std::string& locationName) {
    LoadClearedLocations();
    const std::string localName = "AP_" + locationName;
    return std::ranges::any_of(clearedLocations_, [&localName](const std::string& clearedLocation) {
        return clearedLocation == localName || clearedLocation.starts_with(localName + ".") ||
               localName.starts_with(clearedLocation + ".");
    });
}

struct LocationResolution {
    bool exists = false;
    std::set<int64_t> missing;
};

static LocationResolution ResolveLocation(const std::string& locationId, const std::set<int64_t>& missingLocations,
                                          const std::set<int64_t>& checkedLocations) {
    LocationResolution resolution;
    std::string locationWithoutPrefix = locationId.substr(3);
    auto locationExistsInWorld = [&missingLocations, &checkedLocations](int64_t locationId) {
        return missingLocations.contains(locationId) || checkedLocations.contains(locationId);
    };

    auto apLocationId = GetLocationId(locationWithoutPrefix);
    if (apLocationId && locationExistsInWorld(*apLocationId)) {
        resolution.exists = true;
        if (missingLocations.contains(*apLocationId)) resolution.missing.insert(*apLocationId);
    }
    for (int i = 0; i <= 3; i++) {
        std::string fullLocation = locationWithoutPrefix + "." + std::to_string(i);
        apLocationId = GetLocationId(fullLocation);
        if (apLocationId && locationExistsInWorld(*apLocationId)) {
            resolution.exists = true;
            if (missingLocations.contains(*apLocationId)) resolution.missing.insert(*apLocationId);
        }
    }
    return resolution;
}

std::optional<bool> Archipelago::IsCurrentWorldLocation(const std::string& locationId) const {
    if (!worldLocationSnapshotReady_ || !ap || !locationId.starts_with("AP_")) return std::nullopt;
    return ResolveLocation(locationId, ap->get_missing_locations(), ap->get_checked_locations()).exists;
}

void Archipelago::ResetLocalLocationCache() {
    clearedLocations_.clear();
    clearedLocationsLoaded_ = false;
    pendingClearedLocations_.clear();
    pendingClearedLocationsLoaded_ = false;
}

void Archipelago::LoadClearedLocations() {
    if (clearedLocationsLoaded_) return;

    clearedLocations_.clear();
    clearedLocationsLoaded_ = true;
    int32_t locationCount = LoadSavedValue(CLEARED_LOCATION_SAVE_PREFIX + "Count").value_or(0);
    if (locationCount < 0 || locationCount > MAX_CLEARED_LOCATIONS) {
        Logger::Log(LogLevel::Error, "[AP] Invalid saved cleared-location count: ", locationCount);
        return;
    }

    for (int32_t index = 0; index < locationCount; index++) {
        auto location = LoadSavedString(CLEARED_LOCATION_SAVE_PREFIX + std::to_string(index));
        if (!location || !location->starts_with("AP_")) continue;
        if (std::find(clearedLocations_.begin(), clearedLocations_.end(), *location) == clearedLocations_.end()) {
            clearedLocations_.push_back(*location);
        }
    }
    Logger::Log("[AP] Loaded ", clearedLocations_.size(), " locally cleared locations");
}

void Archipelago::RecordClearedLocation(const std::string& locationId) {
    LoadClearedLocations();
    if (std::find(clearedLocations_.begin(), clearedLocations_.end(), locationId) != clearedLocations_.end()) return;
    if (clearedLocations_.size() >= static_cast<size_t>(MAX_CLEARED_LOCATIONS)) {
        Logger::Log(LogLevel::Error, "[AP] Cleared-location save limit reached");
        return;
    }

    size_t index = clearedLocations_.size();
    SaveLocalString(CLEARED_LOCATION_SAVE_PREFIX + std::to_string(index), locationId);
    SaveLocalValue(CLEARED_LOCATION_SAVE_PREFIX + "Count", static_cast<int32_t>(index + 1));
    clearedLocations_.push_back(locationId);
    InGameTracker::Instance().ObserveLocationCleared(locationId);
    Logger::Log(LogLevel::File, "[AP] Journaled cleared location:", locationId, "index:", index);
}

void Archipelago::LoadPendingClearedLocations() {
    if (pendingClearedLocationsLoaded_) return;

    pendingClearedLocations_.clear();
    pendingClearedLocationsLoaded_ = true;
    const int32_t locationCount = LoadSavedValue(PENDING_CLEARED_LOCATION_SAVE_PREFIX + "Count").value_or(0);
    if (locationCount < 0 || locationCount > MAX_CLEARED_LOCATIONS) {
        Logger::Log(LogLevel::Error, "[AP] Invalid pending cleared-location count: ", locationCount);
        return;
    }

    for (int32_t index = 0; index < locationCount; index++) {
        auto location = LoadSavedString(PENDING_CLEARED_LOCATION_SAVE_PREFIX + std::to_string(index));
        if (!location || !location->starts_with("AP_")) continue;
        if (std::find(pendingClearedLocations_.begin(), pendingClearedLocations_.end(), *location) ==
            pendingClearedLocations_.end()) {
            pendingClearedLocations_.push_back(*location);
        }
    }
}

void Archipelago::RecordPendingClearedLocation(const std::string& locationId) {
    LoadPendingClearedLocations();
    if (std::find(pendingClearedLocations_.begin(), pendingClearedLocations_.end(), locationId) !=
        pendingClearedLocations_.end()) {
        return;
    }
    if (pendingClearedLocations_.size() >= static_cast<size_t>(MAX_CLEARED_LOCATIONS)) {
        Logger::Log(LogLevel::Error, "[AP] Pending cleared-location save limit reached");
        return;
    }

    const size_t index = pendingClearedLocations_.size();
    SaveLocalString(PENDING_CLEARED_LOCATION_SAVE_PREFIX + std::to_string(index), locationId);
    SaveLocalValue(PENDING_CLEARED_LOCATION_SAVE_PREFIX + "Count", static_cast<int32_t>(index + 1));
    pendingClearedLocations_.push_back(locationId);
    InGameTracker::Instance().ObserveLocationCleared(locationId);
    Logger::Log(LogLevel::File, "[AP] Journaled pre-connect cleared location:", locationId, "index:", index);
}

void Archipelago::PromotePendingClearedLocations() {
    if (!HasCurrentSaveSchema()) return;
    LoadPendingClearedLocations();
    if (pendingClearedLocations_.empty()) return;

    const auto pending = pendingClearedLocations_;
    for (const auto& locationId : pending) RecordClearedLocation(locationId);
    SaveLocalValue(PENDING_CLEARED_LOCATION_SAVE_PREFIX + "Count", 0);
    pendingClearedLocations_.clear();
    Logger::Log(LogLevel::File, "[AP] Promoted pre-connect cleared locations:", pending.size());
}

size_t Archipelago::SendMissingClearedLocations() {
    if (!ap || !IsConnected()) return 0;

    LoadClearedLocations();
    const auto missingLocations = ap->get_missing_locations();
    const auto checkedLocations = ap->get_checked_locations();
    std::unordered_set<int64_t> locations;
    std::string locationNames;
    for (const auto& clearedLocation : clearedLocations_) {
        auto resolution = ResolveLocation(clearedLocation, missingLocations, checkedLocations);
        if (!resolution.missing.empty()) {
            if (!locationNames.empty()) locationNames += ", ";
            locationNames += clearedLocation;
        }
        locations.insert(resolution.missing.begin(), resolution.missing.end());
    }

    if (locations.empty()) return 0;
    Logger::Log(LogLevel::File, "[AP] Submitting missing cleared locations:", locationNames, "resolved IDs:",
                locations.size());
    try {
        std::list<int64_t> sortedLocations(locations.begin(), locations.end());
        sortedLocations.sort();
        ap->LocationChecks(sortedLocations);
    } catch (const std::exception& exception) {
        Logger::Log(LogLevel::File, "[AP] Location-check send failed; checks remain journaled for reconnect:",
                    exception.what());
        return 0;
    }
    Logger::Log("[AP] Reconciled ", locations.size(), " missing location checks");
    return locations.size();
}

void Archipelago::ReconcileCompletedBossShardLocations() {
    if (!ap || !IsConnected()) return;

    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (!gameInstance) return;

    const auto missingLocations = ap->get_missing_locations();
    for (const auto& bossLocation : BOSS_SHARD_LOCATIONS) {
        const std::string locationName(bossLocation.location_id);
        const auto locationId = GetLocationId(locationName);
        // Do not journal boss state for an older or differently configured world which has no such check.
        if (!locationId || !missingLocations.contains(*locationId)) continue;

        if (!gameInstance->IsCompletedBoss(FNameFromString(std::string(bossLocation.boss_id)))) continue;

        const std::string nativeLocationName = "AP_" + locationName;
        Logger::Log(LogLevel::File, "[AP] Recovered completed boss shard location from save:",
                    nativeLocationName);
        SendLocationChecks(nativeLocationName);
    }
}

LocationCheckResult Archipelago::SendLocationChecks(const std::string& locationId) {
    if (!locationId.starts_with("AP_")) return LocationCheckResult::UnknownLocation;
    const bool wasClearedLocally = WasLocationClearedLocally(locationId.substr(3));
    if (!HasCurrentSaveSchema()) {
        RecordPendingClearedLocation(locationId);
        return LocationCheckResult::NotReady;
    }
    RecordClearedLocation(locationId);
    if (!ap || !worldLocationSnapshotReady_) return LocationCheckResult::NotReady;

    const auto missingLocations = ap->get_missing_locations();
    const auto checkedLocations = ap->get_checked_locations();
    auto requestedLocation = ResolveLocation(locationId, missingLocations, checkedLocations);
    if (!requestedLocation.exists) return LocationCheckResult::UnknownLocation;
    if (!IsConnected()) {
        return requestedLocation.missing.empty() || wasClearedLocally ? LocationCheckResult::AlreadyChecked
                                                                      : LocationCheckResult::NotReady;
    }

    if (!wasClearedLocally && !requestedLocation.missing.empty()) {
        InGameTracker::Instance().AuditLocationClear(locationId);
    }
    SendMissingClearedLocations();
    return requestedLocation.missing.empty() ? LocationCheckResult::AlreadyChecked : LocationCheckResult::Sent;
}

void Archipelago::Sync() {
    if (IsConnected() && ap) {
        if (!LoadLocalProgress()) return;
        ap->Sync();
        SendMissingClearedLocations();
    }
}

void Archipelago::SaveLocalValue(const std::string& saveValueName, int32_t value) const {
    std::wstring wideSaveValueName(saveValueName.begin(), saveValueName.end());
    auto saveValueId = SDK::UKismetStringLibrary::Conv_StringToName(wideSaveValueName.c_str());
    SDK::UPBGameInstance::SetSavedValue(saveValueId, value);
}

void Archipelago::SaveLocalInt64(const std::string& name, int64_t value) const {
    SaveLocalValue(name + "Low", static_cast<int32_t>(static_cast<uint64_t>(value) & 0xffffffffull));
    SaveLocalValue(name + "High", static_cast<int32_t>(static_cast<uint64_t>(value) >> 32));
}

static std::optional<int32_t> LoadSavedValue(const std::string& saveValueName) {
    std::wstring wideSaveValueName(saveValueName.begin(), saveValueName.end());
    auto saveValueId = SDK::UKismetStringLibrary::Conv_StringToName(wideSaveValueName.c_str());
    int32_t value = 0;
    bool isValid = false;
    SDK::UPBGameInstance::GetSavedValue(saveValueId, &value, &isValid);
    return isValid ? std::optional<int32_t>(value) : std::nullopt;
}

static std::optional<std::string> LoadSavedString(const std::string& saveValueName) {
    auto lengthValue = LoadSavedValue(saveValueName + "Length");
    if (!lengthValue || *lengthValue < 0 || static_cast<size_t>(*lengthValue) > MAX_CONNECTION_FIELD_LENGTH) {
        return std::nullopt;
    }

    std::string value(static_cast<size_t>(*lengthValue), '\0');
    for (size_t offset = 0; offset < value.size(); offset += 4) {
        auto packedValue = LoadSavedValue(saveValueName + std::to_string(offset / 4));
        if (!packedValue) return std::nullopt;
        uint32_t bytes = static_cast<uint32_t>(*packedValue);
        for (size_t byteIndex = 0; byteIndex < 4 && offset + byteIndex < value.size(); byteIndex++) {
            value[offset + byteIndex] = static_cast<char>((bytes >> (byteIndex * 8)) & 0xff);
        }
    }
    return value;
}

void Archipelago::SaveLocalString(const std::string& saveValueName, const std::string& value) const {
    SaveLocalValue(saveValueName + "Length", static_cast<int32_t>(value.size()));
    for (size_t offset = 0; offset < value.size(); offset += 4) {
        uint32_t packedValue = 0;
        for (size_t byteIndex = 0; byteIndex < 4 && offset + byteIndex < value.size(); byteIndex++) {
            packedValue |=
                static_cast<uint32_t>(static_cast<unsigned char>(value[offset + byteIndex])) << (byteIndex * 8);
        }
        SaveLocalValue(saveValueName + std::to_string(offset / 4), static_cast<int32_t>(packedValue));
    }
}

void Archipelago::SaveConnectionInfo() const {
    SaveLocalString(CONNECTION_SAVE_PREFIX + "Uri", currentUri_);
    SaveLocalString(CONNECTION_SAVE_PREFIX + "Slot", slotName_);
    SaveLocalString(CONNECTION_SAVE_PREFIX + "Password", password_);

    SaveLocalValue(CONNECTION_SAVE_PREFIX + "DeathLink", static_cast<int32_t>(deathLinkMode_));
    SaveLocalValue(CONNECTION_SAVE_PREFIX + "Version", CONNECTION_SAVE_VERSION);
    Logger::Log("[AP] Saved successful connection info to the current save");
}

void Archipelago::ApplySavedEnemyDropShuffle() {
    // The drop data table can survive a trip through the title screen. Restore
    // the PAK-owned rows first so loading an older/non-AP save cannot inherit
    // the previous save's slot-specific shuffle.
    EnemyDropShuffle::Reset();
    if (!HasCurrentSaveSchema()) return;
    const auto seed = LoadSavedValue(ENEMY_DROP_SHUFFLE_SEED_VALUE);
    const auto version = LoadSavedValue(ENEMY_DROP_SHUFFLE_VERSION_VALUE);
    if (!seed || !version) return;

    EnemyDropShuffle::Apply(static_cast<std::uint32_t>(*seed), *version);
}

bool Archipelago::ApplyConnectedEnemyDropShuffle(const std::string& seedName, std::uint32_t slotId) {
    const int version = bloodstained::enemy_drop_shuffle::VERSION;
    const std::uint32_t seed = bloodstained::enemy_drop_shuffle::DeriveSeed(seedName, slotId);
    const auto savedSeed = LoadSavedValue(ENEMY_DROP_SHUFFLE_SEED_VALUE);
    const auto savedVersion = LoadSavedValue(ENEMY_DROP_SHUFFLE_VERSION_VALUE);
    if ((savedSeed && static_cast<std::uint32_t>(*savedSeed) != seed) ||
        (savedVersion && *savedVersion != version)) {
        Logger::Log(LogLevel::File,
                    "[DropShuffle] Rebinding save to connected slot's shuffle; old seed:",
                    savedSeed ? static_cast<std::uint32_t>(*savedSeed) : 0, "new seed:", seed);
    }
    Logger::Log(LogLevel::File, "[DropShuffle] Derived seed:", seed, "from room seed:",
                seedName, "slot:", slotId);

    // SetSavedValue writes into the active story-save object. The values reach
    // disk on the player's next normal save.
    SaveLocalValue(ENEMY_DROP_SHUFFLE_SEED_VALUE, static_cast<std::int32_t>(seed));
    SaveLocalValue(ENEMY_DROP_SHUFFLE_VERSION_VALUE, version);
    return EnemyDropShuffle::Apply(seed, version);
}

std::optional<ArchipelagoConnectionInfo> Archipelago::LoadSavedConnectionInfo() const {
    if (!HasCurrentSaveSchema()) return std::nullopt;
    auto version = LoadSavedValue(CONNECTION_SAVE_PREFIX + "Version");
    if (!version || (*version != CONNECTION_SAVE_VERSION &&
                     *version != LEGACY_BOOLEAN_DEATHLINK_CONNECTION_SAVE_VERSION)) {
        return std::nullopt;
    }

    auto uri = LoadSavedString(CONNECTION_SAVE_PREFIX + "Uri");
    auto slotName = LoadSavedString(CONNECTION_SAVE_PREFIX + "Slot");
    auto password = LoadSavedString(CONNECTION_SAVE_PREFIX + "Password");
    auto deathLink = LoadSavedValue(CONNECTION_SAVE_PREFIX + "DeathLink");
    if (!uri || !slotName || !password || !deathLink || slotName->empty()) return std::nullopt;

    DeathLinkMode deathLinkMode = DeathLinkMode::Off;
    if (*version == LEGACY_BOOLEAN_DEATHLINK_CONNECTION_SAVE_VERSION) {
        deathLinkMode = *deathLink != 0 ? DeathLinkMode::GameOver : DeathLinkMode::Off;
    } else if (*deathLink >= static_cast<int32_t>(DeathLinkMode::Off) &&
               *deathLink <= static_cast<int32_t>(DeathLinkMode::GameOver)) {
        deathLinkMode = static_cast<DeathLinkMode>(*deathLink);
    } else {
        return std::nullopt;
    }

    return ArchipelagoConnectionInfo{*uri, *slotName, *password, deathLinkMode};
}

bool Archipelago::HasCurrentSaveSchema() const {
    const auto schema = LoadSavedValue(STATIC_PAK_SAVE_SCHEMA_VALUE);
    return schema && *schema == STATIC_PAK_SCHEMA;
}

bool Archipelago::HasLegacySaveState() const {
    const auto connectionVersion = LoadSavedValue(CONNECTION_SAVE_PREFIX + "Version");
    // Any AP connection state without the static-pak schema belongs to an older client. A matching connection
    // serialization version does not make that story save compatible with the new entitlement model.
    if (connectionVersion) return true;
    if (LoadSavedValue(CLEARED_LOCATION_SAVE_PREFIX + "Count")) return true;
    if (LoadSavedValue(ENEMY_DROP_SHUFFLE_SEED_VALUE) || LoadSavedValue(ENEMY_DROP_SHUFFLE_VERSION_VALUE)) {
        return true;
    }
    if (localSavePrefix_.empty()) return false;
    return LoadSavedValue(localSavePrefix_ + LEGACY_ITEM_INDEX_SAVE_VALUE).has_value() ||
           LoadSavedValue(localSavePrefix_ + LEGACY_ITEM_LEDGER_VERSION_VALUE).has_value();
}

bool Archipelago::ValidateSlotAndSave(const json& slotData) {
    lastError_.clear();
    if (!slotData.is_object() || !slotData.contains("static_pak_schema") ||
        !slotData.at("static_pak_schema").is_number_integer() ||
        slotData.at("static_pak_schema").get<int32_t>() != STATIC_PAK_SCHEMA) {
        lastError_ = "This slot does not use Bloodstained AP static-pak schema 1";
        return false;
    }
    if (!slotData.contains("difficulty") || !slotData.at("difficulty").is_string()) {
        lastError_ = "This slot has no Bloodstained difficulty setting";
        return false;
    }
    if (!slotData.contains("content_policy") || !slotData.at("content_policy").is_string()) {
        lastError_ = "This slot has no supported Bloodstained content policy";
        return false;
    }
    const auto contentPolicy = slotData.at("content_policy").get<std::string>();
    if (contentPolicy != LEGACY_CONTENT_POLICY && contentPolicy != CONTENT_POLICY) {
        lastError_ = "This slot does not use a supported Bloodstained content policy";
        return false;
    }

    const std::string expectedDifficulty = slotData.at("difficulty").get<std::string>();
    const std::string actualDifficulty(GetCurrentDifficultyName());
    if (expectedDifficulty != actualDifficulty) {
        lastError_ = "Difficulty mismatch: slot expects " + expectedDifficulty + ", save is " + actualDifficulty;
        return false;
    }

    const auto saveSchema = LoadSavedValue(STATIC_PAK_SAVE_SCHEMA_VALUE);
    if (saveSchema) {
        if (*saveSchema != STATIC_PAK_SCHEMA) {
            lastError_ = "Unsupported Bloodstained AP save schema " + std::to_string(*saveSchema);
            return false;
        }
        PromotePendingClearedLocations();
        return true;
    }
    if (HasLegacySaveState()) {
        lastError_ = "Pre-1.1.0 Bloodstained AP saves are unsupported; start a new story save";
        return false;
    }

    SaveLocalValue(STATIC_PAK_SAVE_SCHEMA_VALUE, STATIC_PAK_SCHEMA);
    PromotePendingClearedLocations();
    Logger::Log(LogLevel::File, "[AP] Initialized static-pak save schema:", STATIC_PAK_SCHEMA,
                "difficulty:", expectedDifficulty);
    return true;
}

bool Archipelago::ValidateRequiredDlcLocations() {
    lastError_.clear();
    if (!ap) {
        lastError_ = "DLC ownership could not be verified; reconnect after game initialization completes.";
        return false;
    }

    const auto igaShardLocation = GetLocationId("N2013_Shard");
    if (!igaShardLocation) {
        lastError_ = "The client has no generated binding for the IGA shard location.";
        return false;
    }

    const bool igaShardIsActive = ap->get_missing_locations().contains(*igaShardLocation) ||
                                  ap->get_checked_locations().contains(*igaShardLocation);
    if (!igaShardIsActive) return true;

    switch (GameManager::Instance().GetDlcOwnership("DLC_0002")) {
        case DlcOwnership::Owned:
            return true;
        case DlcOwnership::NotOwned:
            lastError_ = "This multiworld includes the IGA shard location. Purchase and install IGA's Back Pack before connecting.";
            return false;
        case DlcOwnership::NotReady:
            lastError_ = "IGA's Back Pack ownership could not be verified; reconnect after DLC initialization completes.";
            return false;
        case DlcOwnership::NotApplicable:
            break;
    }
    lastError_ = "IGA's Back Pack ownership could not be verified; reconnect after DLC initialization completes.";
    return false;
}

bool Archipelago::LoadLocalProgress() {
    if (!ap || localSavePrefix_.empty() || !HasCurrentSaveSchema()) return false;

    localProgressLoaded_ = LoadEntitlementLedger();
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    receivedProgressionInventoryReconciled_ = false;
    if (localProgressLoaded_) {
        Logger::Log(LogLevel::File, "[AP] Loaded schema-1 entitlement ledger; awarded item types:",
                    awardedItemCounts_.size(), "prefix:", localSavePrefix_);
    }
    return localProgressLoaded_;
}

bool Archipelago::LoadEntitlementLedger() {
    awardedItemCounts_.clear();

    enum class LedgerLoadResult { Missing, Valid, Repaired, Invalid };
    auto loadFromPrefix = [](const std::string& ledgerPrefix,
                             std::unordered_map<int64_t, std::uint32_t>& output,
                             std::string& error) {
        output.clear();
        const auto version = LoadSavedValue(ledgerPrefix + ENTITLEMENT_LEDGER_VERSION_VALUE);
        if (!version) return LedgerLoadResult::Missing;
        if (*version != ENTITLEMENT_LEDGER_VERSION) {
            error = "Unsupported Bloodstained AP entitlement ledger version";
            return LedgerLoadResult::Invalid;
        }

        const auto awardedCountValue = LoadSavedValue(ledgerPrefix + ENTITLEMENT_LEDGER_COUNT_VALUE);
        if (!awardedCountValue || *awardedCountValue < 0 || *awardedCountValue > MAX_ITEM_LEDGER_ENTRIES) {
            error = "Invalid Bloodstained AP entitlement ledger count";
            return LedgerLoadResult::Invalid;
        }

        int32_t discardedEntries = 0;
        for (int32_t entry = 0; entry < *awardedCountValue; entry++) {
            const std::string prefix = ledgerPrefix + "EntitlementLedger" + std::to_string(entry);
            const auto itemId = LoadInt64(prefix + "Item");
            const auto count = LoadSavedValue(prefix + "Count");
            if (!itemId || !count || *count <= 0 || !IsKnownItemId(*itemId)) {
                discardedEntries++;
                continue;
            }
            const auto [existing, inserted] =
                output.emplace(*itemId, static_cast<std::uint32_t>(*count));
            if (!inserted) {
                existing->second = std::max(existing->second, static_cast<std::uint32_t>(*count));
                discardedEntries++;
            }
        }
        if (discardedEntries > 0) {
            if (output.empty()) {
                error = "Bloodstained AP entitlement ledger contains no recoverable entries";
                return LedgerLoadResult::Invalid;
            }
            error = "Recovered mixed Bloodstained AP entitlement ledger; discarded entries: " +
                    std::to_string(discardedEntries);
            return LedgerLoadResult::Repaired;
        }
        return LedgerLoadResult::Valid;
    };

    std::string error;
    const auto currentResult = loadFromPrefix(localSavePrefix_, awardedItemCounts_, error);
    if (currentResult == LedgerLoadResult::Valid) return true;
    if (currentResult == LedgerLoadResult::Repaired) {
        Logger::Log(LogLevel::File, "[AP] ", error, "; preserving valid unique entries:",
                    awardedItemCounts_.size());
        PersistAwardedItemCounts();
        return true;
    }
    if (currentResult == LedgerLoadResult::Invalid) {
        lastError_ = error;
        Logger::Log(LogLevel::File, "[AP] ", lastError_);
        return false;
    }

    // Pre-release schema-1 builds included GetLastUsedSaveSlotIndex() in these keys. During new-save startup that
    // value can still name the previously loaded slot, so recover the most complete ledger stored in this story save
    // and immediately rewrite it under the stable seed/team/player prefix.
    std::unordered_map<int64_t, std::uint32_t> recoveredCounts;
    std::string recoveredPrefix;
    std::uint64_t recoveredOccurrences = 0;
    for (int32_t slot = 0; slot < LEGACY_SAVE_SLOT_SCAN_LIMIT; slot++) {
        const std::string candidatePrefix = localSavePrefix_ + "Save" + std::to_string(slot) + "_";
        std::unordered_map<int64_t, std::uint32_t> candidateCounts;
        std::string candidateError;
        if (loadFromPrefix(candidatePrefix, candidateCounts, candidateError) != LedgerLoadResult::Valid) continue;
        const std::uint64_t candidateOccurrences = std::accumulate(
            candidateCounts.begin(), candidateCounts.end(), std::uint64_t{0},
            [](std::uint64_t total, const auto& entry) { return total + entry.second; });
        if (recoveredPrefix.empty() || candidateOccurrences > recoveredOccurrences) {
            recoveredCounts = std::move(candidateCounts);
            recoveredPrefix = candidatePrefix;
            recoveredOccurrences = candidateOccurrences;
        }
    }
    if (!recoveredPrefix.empty()) {
        awardedItemCounts_ = std::move(recoveredCounts);
        PersistAwardedItemCounts();
        Logger::Log(LogLevel::File, "[AP] Recovered mis-keyed schema-1 entitlement ledger; item types:",
                    awardedItemCounts_.size(), "occurrences:", recoveredOccurrences, "old prefix:",
                    recoveredPrefix, "new prefix:", localSavePrefix_);
        return true;
    }

    if (LoadSavedValue(CONNECTION_SAVE_PREFIX + "Version")) {
        lastError_ = "This save has no recoverable AP item ledger; refusing historical item replay";
        Logger::Log(LogLevel::File, "[AP] ", lastError_, " prefix:", localSavePrefix_);
        return false;
    }

    // A genuinely fresh story save must have a durable empty ledger before connection details are written. That
    // distinction lets future loads fail closed instead of treating missing state as permission to replay history.
    PersistAwardedItemCounts();
    Logger::Log(LogLevel::File, "[AP] Initialized empty schema-1 entitlement ledger; prefix:", localSavePrefix_);
    return true;
}

void Archipelago::PersistAwardedItemCounts() const {
    if (localSavePrefix_.empty()) return;
    std::vector<std::pair<int64_t, std::uint32_t>> counts(awardedItemCounts_.begin(), awardedItemCounts_.end());
    std::ranges::sort(counts);
    int32_t entry = 0;
    for (const auto& [itemId, count] : counts) {
        const std::string prefix = localSavePrefix_ + "EntitlementLedger" + std::to_string(entry++);
        SaveLocalInt64(prefix + "Item", itemId);
        SaveLocalValue(prefix + "Count", static_cast<int32_t>(count));
    }
    // Publish metadata last. If the game exits during an entry write, the previous smaller count remains a valid
    // ledger and the partially written suffix is ignored instead of making the save impossible to reconnect.
    SaveLocalValue(localSavePrefix_ + ENTITLEMENT_LEDGER_VERSION_VALUE, ENTITLEMENT_LEDGER_VERSION);
    SaveLocalValue(localSavePrefix_ + ENTITLEMENT_LEDGER_COUNT_VALUE, static_cast<int32_t>(counts.size()));
}

void Archipelago::ProcessReceivedItems() {
    if (processingReceivedItems_ || receivedItemGrantPending_ ||
        std::chrono::steady_clock::now() < receivedItemRetryAt_) {
        return;
    }
    processingReceivedItems_ = true;
    struct ProcessingGuard {
        bool& active;
        ~ProcessingGuard() { active = false; }
    } processingGuard{processingReceivedItems_};

    if (!ap || !GameManager::Instance().CanReceiveItems()) {
        return;
    }
    if (!localProgressLoaded_) return;

    // Repair ledger-acknowledged progression before newer deliveries. Otherwise
    // an unresolved later item can indefinitely starve current-save recovery.
    ReconcileReceivedProgressionInventory();
    if (receivedItemGrantPending_ || !receivedProgressionInventoryReconciled_) {
        return;
    }
    std::unordered_map<int64_t, std::uint32_t> occurrences;
    const APClient::NetworkItem* itemToGrant = nullptr;
    for (const auto& [index, item] : receivedItems_) {
        const std::uint32_t occurrence = ++occurrences[item.item];
        if (occurrence > awardedItemCounts_[item.item]) {
            itemToGrant = &item;
            break;
        }
    }
    if (!itemToGrant) return;

    const auto& item = *itemToGrant;
    const int64_t itemIndex = item.index;
    const auto progressiveItemName = bloodstained::items::ResolveProgressiveItem(
        static_cast<std::uint64_t>(item.item), awardedItemCounts_[item.item]);
    const auto nativeItemName = progressiveItemName ? progressiveItemName : GetNativeItemName(item.item);
    const std::string itemName = nativeItemName ? std::string(*nativeItemName) : std::string();
    const std::string displayItemName = GetDisplayItemName(item.item);
    const std::string savePrefix = localSavePrefix_;
    const uint64_t generation = receivedItemGeneration_;
    Logger::Log(LogLevel::File, "[AP] Attempting received item grant:", displayItemName, "ID:", item.item,
                "index:", itemIndex, "awarded occurrences:", awardedItemCounts_[item.item]);

    receivedItemGrantPending_ = true;
    if (!GivePlayerItem(itemName, false,
                        [this, itemIndex, itemId = item.item, itemName, savePrefix, generation](ItemGrantResult result) {
                            CompleteReceivedItem(itemIndex, itemId, itemName, savePrefix, generation, result);
                        })) {
        receivedItemGrantPending_ = false;
        receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_RETRY_DELAY;
        Logger::Log(LogLevel::File, "[AP] Delaying unresolved received item:", itemName, "index:", itemIndex);
    }
}

void Archipelago::CompleteReceivedItem(int64_t itemIndex, int64_t itemId, const std::string& itemName,
                                       const std::string& savePrefix, uint64_t generation, ItemGrantResult result) {
    if (generation != receivedItemGeneration_) {
        Logger::Log(LogLevel::File, "[AP] Ignoring stale received item result:", itemName, "index:", itemIndex);
        return;
    }

    receivedItemGrantPending_ = false;
    if (localSavePrefix_ != savePrefix) {
        Logger::Log(LogLevel::File, "[AP] Discarding received item result after save or slot change:", itemName,
                    "index:", itemIndex);
        return;
    }

    if (result == ItemGrantResult::Rejected) {
        receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_RETRY_DELAY;
        Logger::Log(LogLevel::File, "[AP] Native item grant rejected; retaining for retry:", itemName,
                    "index:", itemIndex);
        return;
    }

    awardedItemCounts_[itemId]++;
    PersistAwardedItemCounts();
    InGameTracker::Instance().InvalidateReachability("AP received item committed");
    receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_INTERVAL;
    if (result == ItemGrantResult::AtCapacity) {
        Logger::Log(LogLevel::File, "[AP] Received item already at native inventory capacity:", itemName,
                    "committed index:", itemIndex);
    } else if (result == ItemGrantResult::Unsupported) {
        Logger::Log(LogLevel::File, "[AP] Skipped received item unsupported by this game installation:", itemName,
                    "committed index:", itemIndex);
    } else {
        Logger::Log(LogLevel::File, "[AP] Received item grant succeeded:", itemName, "committed index:", itemIndex);
    }
}

void Archipelago::ReconcileReceivedProgressionInventory() {
    if (receivedProgressionInventoryReconciled_ || receivedItems_.empty() ||
        !GameManager::Instance().CanReceiveItems()) {
        return;
    }

    std::unordered_map<int64_t, std::uint32_t> occurrences;
    for (const auto& [index, item] : receivedItems_) {
        if (++occurrences[item.item] > awardedItemCounts_[item.item]) continue;

        const auto nativeItemName = GetNativeItemName(item.item);
        if (!nativeItemName) continue;
        std::string itemName(*nativeItemName);
        const auto dlcOwnership = GameManager::Instance().GetPaidDlcItemOwnership(itemName);
        if (dlcOwnership == DlcOwnership::NotOwned) continue;
        if (dlcOwnership == DlcOwnership::NotReady) {
            receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_RETRY_DELAY;
            return;
        }
        const auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
        if (!itemId) continue;

        const bool isShardOrSkill = IsShardOrSkillItem(*itemId);
        const bool isNativeTraversal = std::any_of(
            bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS.begin(),
            bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS.end(),
            [&itemName](const auto& traversalItem) { return traversalItem.native_id == itemName; });
        const bool isProgression = isNativeTraversal ||
                                   (item.flags & APClient::ItemFlags::FLAG_ADVANCEMENT) != 0;
        if ((!isShardOrSkill && !isProgression) || GameManager::Instance().CheckAllInventories(*itemId)) {
            continue;
        }

        const uint64_t generation = receivedItemGeneration_;
        receivedItemGrantPending_ = true;
        Logger::Log(LogLevel::File, "[AP] Repairing acknowledged but missing progression item or shard:",
                    itemName, "index:", index);
        GameManager::Instance().GivePlayerItem(
            *itemId, true, isShardOrSkill ? static_cast<int>(shardDropInitialGrade_) : 1,
            [this, generation, itemName, index](ItemGrantResult result) {
                if (generation != receivedItemGeneration_) return;
                receivedItemGrantPending_ = false;
                if (result == ItemGrantResult::Rejected) {
                    receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_RETRY_DELAY;
                    Logger::Log(LogLevel::File,
                                "[AP] Missing progression item or shard repair rejected; retry pending:",
                                itemName, "index:", index);
                    return;
                }
                receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_INTERVAL;
                if (result == ItemGrantResult::Unsupported) {
                    Logger::Log(LogLevel::File,
                                "[AP] Skipped missing progression repair unsupported by this game installation:",
                                itemName, "index:", index);
                } else {
                    Logger::Log(LogLevel::File, "[AP] Missing progression item or shard repair succeeded:",
                                itemName, "index:", index);
                }
            });
        return;
    }
    receivedProgressionInventoryReconciled_ = true;
}

std::string Archipelago::GetStateAsString() {
    auto state = Archipelago::Instance().GetConnectionState();
    switch (state) {
        case ArchipelagoConnectionState::Disconnected:
            return "Disconnected";
        case ArchipelagoConnectionState::Connecting:
            return "Connecting";
        case ArchipelagoConnectionState::Connected:
            return "Connected";
        case ArchipelagoConnectionState::SlotConnected:
            return "Slot Connected";
        case ArchipelagoConnectionState::SocketDisconnectedError:
            return "Socket Disconnected";
        case ArchipelagoConnectionState::ConnectionRefusedError:
            return "Connection Refused";
        case ArchipelagoConnectionState::InvalidSlotError:
            return "Invalid Slot";
        default:
            return "Unknown";
    }
}

void Archipelago::BaelDefeated() {
    auto goalStatus = APClient::ClientStatus::GOAL;
    if (ap) {
        ap->StatusUpdate(goalStatus);
    }
}

void Archipelago::InvokeDeathLink() {
    if (!ap) return;
    if (deathLinkMode_ == DeathLinkMode::Off) return;
    deathtime = ap->get_server_time();
    int causeOfDeathNum = std::rand() % deathReasons_.size();
    std::list<std::string>::iterator it = deathReasons_.begin();
    advance(it, causeOfDeathNum);
    json data{
        {"time", deathtime},
        {"cause", *it},
        {"source", ap->get_slot()},
    };
    ap->Bounce(data, {}, {}, {"DeathLink"});
    ap->poll();
}

void Archipelago::SetDeathLinkMode(DeathLinkMode deathLinkMode) {
    if (deathLinkMode < DeathLinkMode::Off || deathLinkMode > DeathLinkMode::GameOver) return;
    if (deathLinkMode_ == deathLinkMode) return;

    const bool wasEnabled = deathLinkMode_ != DeathLinkMode::Off;
    const bool isEnabled = deathLinkMode != DeathLinkMode::Off;
    deathLinkMode_ = deathLinkMode;
    if (!isEnabled) pendingDeathlink_ = false;

    if (ap && wasEnabled != isEnabled) {
        std::list<std::string> tags;
        if (isEnabled) tags.push_back("DeathLink");
        const bool updated = ap->ConnectUpdate(false, itemsHandling_, true, tags);
        Logger::Log(LogLevel::File, "[AP] Updated DeathLink server tag; enabled:", isEnabled,
                    "accepted:", updated);
    }

    Logger::Log(LogLevel::File, "[AP] DeathLink consequence mode:", static_cast<int32_t>(deathLinkMode_));
    if (state_ == ArchipelagoConnectionState::SlotConnected) SaveConnectionInfo();
}

bool Archipelago::GivePlayerItem(const std::string& itemName, bool shouldDisplay,
                                 std::function<void(ItemGrantResult)> completion) {
    auto instance = GameManager::Instance;
    Logger::Log("Giving player item:", itemName);
    if (itemName.empty()) return false;
    if (itemName == "Nothing") {
        ThreadQueue::Instance().Enqueue([completion = std::move(completion)]() {
            if (completion) completion(ItemGrantResult::Granted);
        });
        return true;
    }
    if (itemName.starts_with("Max")) {
        std::string maxStat = itemName;
        instance().GivePlayerMaxStatItem(maxStat, shouldDisplay);
        ThreadQueue::Instance().Enqueue([completion = std::move(completion)]() {
            if (completion) completion(ItemGrantResult::Granted);
        });
        return true;
        // Check for the bit coins (8 bit coin etc)
    } else if (std::isdigit(itemName[0]) && !itemName.starts_with("8") && !itemName.starts_with("16") &&
               !itemName.starts_with("32")) {
        int amount = std::stoi(itemName.substr(0, itemName.size() - 1));
        instance().GivePlayerCoin(amount, shouldDisplay);
        ThreadQueue::Instance().Enqueue([completion = std::move(completion)]() {
            if (completion) completion(ItemGrantResult::Granted);
        });
        return true;
    }

    auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
    if (itemId.has_value()) {
        if (IsShardOrSkillItem(itemId.value())) {
            // Shards don't normally show in chat, here we override
            instance().GivePlayerItem(itemId.value(), true, static_cast<int>(shardDropInitialGrade_),
                                      std::move(completion));
            return true;
        }
    }

    if (itemId.has_value()) {
        instance().GivePlayerItem(itemId.value(), shouldDisplay, 1, std::move(completion));
        return true;
    } else {
        return false;
    }
}

bool Archipelago::Connect(const std::string& slotName, const std::string& password, const std::string uri,
                          DeathLinkMode deathLinkMode) {
    if (!GameManager::Instance().IsPlayerLoadedInGame()) {
        Logger::Log("Player is not loaded in game");
        return false;
    }
    if (!MainMenuStatus::Instance().IsStaticPakReady()) {
        lastError_ = "BloodstainedAP.pak 1.1.0 is missing, invalid, or conflicts with Randomizer.pak";
        Logger::Log(LogLevel::Error, "[AP] ", lastError_);
        return false;
    }

    slotName_ = slotName;
    password_ = password;
    const std::string serverUri = bloodstained::connection::PrepareServerUri(uri, APClient::DEFAULT_URI);
    currentUri_ = serverUri;
    deathLinkMode_ = deathLinkMode;
    Logger::Log(LogLevel::File, "[AP] Connecting slot:", slotName_, "server:",
                serverUri);
    ResetLocalLocationCache();
    localSavePrefix_.clear();
    slotData_ = json{};
    worldLocationSnapshotReady_ = false;
    localProgressLoaded_ = false;
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    receivedItems_.clear();
    receivedProgressionInventoryReconciled_ = false;
    HookManager::ResetShardDropPolicy();
    InGameTracker::Instance().ResetConnection();

    Logger::Log(LogLevel::Debug, "[AP]", "Connecting Player: ", slotName_, "with uri: ", serverUri);
    UpdateState(ArchipelagoConnectionState::Connecting);

    ap.reset();
    const std::string uuidHost(bloodstained::connection::RemoveWebSocketScheme(serverUri));
    std::string uuid = ap_get_uuid(UUID_FILE, uuidHost);

    ap.reset(new APClient(uuid, GAME_NAME, serverUri, CERT_STORE));
    game_seed = ap->get_seed();

    ap_slot_connect_sent = false;
    ap->set_socket_connected_handler([this]() {
        AbortPassword();
        ap_slot_connect_sent = false;
        UpdateState(ArchipelagoConnectionState::Connected);
        Logger::Log(LogLevel::File, "[AP] Socket connected; requesting slot connection for:", slotName_);
        Logger::Log(LogLevel::Debug, "[AP]", "Player: ", slotName_, "connected to server successfully");
    });

    ap->set_room_info_handler([this]() {
        if (game_seed.empty()) {
            Logger::Log("[AP] Waiting for game...");
        } else if (strncmp(game_seed.c_str(), ap->get_seed().c_str(), MAX_SEED_LENGTH) != 0) {
            Logger::Log("Bad seed connection");
        }
        Logger::Log(LogLevel::Debug, "[AP]", "Room information retrieved successfully");
    });

    ap->set_print_handler([this](const std::string& msg) { Logger::Log("[AP] Raw print: ", msg); });

    ap->set_print_json_handler([](const APClient::PrintJSONArgs& args) {
        if (args.type != "ItemSend" || !args.item || !args.receiving || !ap) return;
        int player = ap->get_player_number();
        if (args.item->player != player && *args.receiving != player) return;

        std::string notification = ap->render_json(args.data);
        if (!notification.empty()) GameManager::Instance().SendInGameNotification(notification, 1023);
    });

    ap->set_items_received_handler([this](const std::list<APClient::NetworkItem>& items) {
        bool progressionChanged = false;
        if (!items.empty() && items.front().index == 0) {
            // A batch beginning at zero is an authoritative history replay.
            // Replace the snapshot so server rollbacks and shortened histories
            // cannot leave stale tail entries in the entitlement calculation.
            receivedItems_.clear();
        }
        for (const auto& item : items) {
            receivedItems_.insert_or_assign(item.index, item);
            const auto nativeItemName = GetNativeItemName(item.item);
            if (!nativeItemName) {
                Logger::Log(LogLevel::File, "[AP] Received unknown Bloodstained item ID:", item.item,
                            "index:", item.index);
                continue;
            }
            const std::string itemName(*nativeItemName);
            const bool isNativeTraversal = std::any_of(
                bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS.begin(),
                bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS.end(),
                [&itemName](const auto& traversalItem) { return traversalItem.native_id == itemName; });
            const bool isProgression = isNativeTraversal ||
                                       (item.flags & APClient::ItemFlags::FLAG_ADVANCEMENT) != 0;
            progressionChanged |= isProgression;
        }
        if (progressionChanged) {
            // ReceivedItems can be delivered in multiple batches. A previous
            // batch may have completed reconciliation before this progression
            // item arrived, so every such batch must reopen the repair scan.
            receivedProgressionInventoryReconciled_ = false;
            InGameTracker::Instance().InvalidateReachability("AP progression item received");
        }
        InGameTracker::Instance().MarkInventorySynchronized();
        ProcessReceivedItems();
    });

    ap->set_location_checked_handler([this](const std::list<int64_t>& locations) {
        if (locations.size() > 16) {
            Logger::Log(LogLevel::File, "[AP] Server reported checked-location snapshot; count:", locations.size());
            return;
        }

        std::string locationNames;
        for (int64_t location : locations) {
            if (!locationNames.empty()) locationNames += ", ";
            locationNames += ap->get_location_name(location, GAME_NAME);
        }
        Logger::Log(LogLevel::File, "[AP] Server reported checked locations:", locationNames);
    });

    ap->set_data_package_changed_handler([this](const json& data) {
        Logger::Log("[AP] Data package loaded for game: ", GAME_NAME);
        ProcessReceivedItems();
        SendMissingClearedLocations();
        HookManager::ApplyShardDropPolicy();
    });

    ap->set_slot_connected_handler([this](const json& slotData) {
        Logger::Log(LogLevel::File, "[AP] Slot connected:", slotName_, "team:", ap->get_team_number(), "player:",
                    ap->get_player_number());
        Logger::Log("SLOT CONNECTED");
        Logger::Log(slotData.dump());

        slotData_ = slotData;
        localSavePrefix_ = "AP_" + ap->get_seed() + "_" + std::to_string(ap->get_team_number()) + "_" +
                           std::to_string(ap->get_player_number()) + "_";
        if (!ValidateRequiredDlcLocations() || !ValidateSlotAndSave(slotData) || !LoadLocalProgress()) {
            Logger::Log(LogLevel::File, "[AP] Rejecting slot for active save:", lastError_);
            GameManager::Instance().SendInGameNotification(lastError_, 1023);
            UpdateState(ArchipelagoConnectionState::InvalidSlotError);
            return;
        }
        worldLocationSnapshotReady_ = true;
        UpdateState(ArchipelagoConnectionState::SlotConnected);
        SendMissingClearedLocations();
        ReconcileCompletedBossShardLocations();
        HookManager::ApplyShardDropPolicy();
        SaveConnectionInfo();

        if (!ApplyConnectedEnemyDropShuffle(ap->get_seed(),
                                            static_cast<std::uint32_t>(ap->get_player_number()))) {
            GameManager::Instance().SendInGameNotification(
                "Enemy drop shuffle could not be applied.", 1023);
        }

        if (slotData.contains("drop_experience_multiplier") && !slotData.at("drop_experience_multiplier").is_null()) {
            auto value = slotData.at("drop_experience_multiplier").get<float>();
            GameManager::Instance().GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute::GainExpRate, value);
        }

        if (slotData.contains("drop_item_multiplier") && !slotData.at("drop_item_multiplier").is_null()) {
            auto value = slotData.at("drop_item_multiplier").get<float>();
            GameManager::Instance().GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute::DropItemRate, value);
        }

        if (slotData.contains("drop_money_multiplier") && !slotData.at("drop_money_multiplier").is_null()) {
            auto value = slotData.at("drop_money_multiplier").get<float>();
            GameManager::Instance().GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute::DropMoneyRate, value);
        }

        if (slotData.contains("drop_shard_multiplier") && !slotData["drop_shard_multiplier"].is_null()) {
            auto value = slotData.at("drop_shard_multiplier").get<float>();
            GameManager::Instance().GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute::DropShardRate, value);
        }

        if (slotData.contains("drop_shard_initial_grade") && !slotData["drop_shard_initial_grade"].is_null()) {
            shardDropInitialGrade_ = slotData.at("drop_shard_initial_grade").get<int64_t>();
        }

        ProcessReceivedItems();

        Logger::Log("[AP] Loaded ", ap->get_missing_locations().size(), " missing and ",
                    ap->get_checked_locations().size(), " checked locations");
    });

    // Handlers for disconnection
    ap->set_socket_disconnected_handler([this]() {
        Logger::Log(LogLevel::File, "[AP] Socket disconnected; automatic reconnect pending for:", slotName_);
        Logger::Log(LogLevel::Debug, "[AP]", "socket disconnected");
        AbortPassword();
        HookManager::ApplyShardDropPolicy();
        ap_slot_connect_sent = false;
        UpdateState(ArchipelagoConnectionState::Disconnected);
    });

    ap->set_slot_disconnected_handler([this]() {
        Logger::Log(LogLevel::File, "[AP] Slot disconnected:", slotName_);
        Logger::Log(LogLevel::Debug, "[AP]", "Player: ", slotName_, "disconnected");
        HookManager::ApplyShardDropPolicy();
        UpdateState(ArchipelagoConnectionState::Disconnected);
        ap_slot_connect_sent = false;
    });

    ap->set_slot_refused_handler([this](const std::list<std::string>& reasons_list) {
        std::string reasons;
        for (const auto& reason : reasons_list) {
            if (!reasons.empty()) reasons += ", ";
            reasons += reason;
        }
        Logger::Log(LogLevel::File, "[AP] Slot connection refused:", slotName_, "reasons:", reasons);
        Logger::Log("Slot:", slotName_, "couldn't connect");
        UpdateState(ArchipelagoConnectionState::InvalidSlotError);
    });

    ap->set_socket_error_handler([this, slotName](const std::string& error) {
        Logger::Log(LogLevel::File, "[AP] Socket error for slot:", slotName, "error:", error);
        Logger::Log(LogLevel::Debug, "[AP]", "Player: ", slotName, "disconnected. Error: ", error);
        UpdateState(ArchipelagoConnectionState::SocketDisconnectedError);
    });

    ap->set_bounced_handler([this, slotName](const json& cmd) {
        if (deathLinkMode_ == DeathLinkMode::Off) return;

        const auto tagsIt = cmd.find("tags");
        if (tagsIt == cmd.end() || !tagsIt->is_array() ||
            std::find(tagsIt->begin(), tagsIt->end(), "DeathLink") == tagsIt->end()) {
            return;
        }

        const auto dataIt = cmd.find("data");
        if (dataIt == cmd.end() || !dataIt->is_object()) {
            Logger::Log(LogLevel::File, "[AP] Ignored malformed DeathLink packet");
            return;
        }

        const auto sourceIt = dataIt->find("source");
        const auto causeIt = dataIt->find("cause");
        const std::string source = sourceIt != dataIt->end() && sourceIt->is_string()
                                       ? sourceIt->get<std::string>()
                                       : "???";
        if (source == slotName) return;

        const std::string cause = causeIt != dataIt->end() && causeIt->is_string()
                                      ? causeIt->get<std::string>()
                                      : "???";
        GameManager::Instance().SendInGameNotification(source + ": " + cause, 346);
        Logger::Log(LogLevel::File, "[AP] Received DeathLink from:", source, "cause:", cause);
        pendingDeathlink_ = true;
    });

    return true;
}

void Archipelago::AbortPassword() { awaiting_password = false; }

void Archipelago::Disconnect() {
    Logger::Log(LogLevel::File, "[AP] Manual disconnect:", slotName_);
    HookManager::ResetShardDropPolicy();
    UpdateState(ArchipelagoConnectionState::Disconnected);
    ap.reset();
    ap_slot_connect_sent = false;
    receivedItems_.clear();
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    localSavePrefix_.clear();
    localProgressLoaded_ = false;
    receivedProgressionInventoryReconciled_ = false;
    slotData_ = json{};
    worldLocationSnapshotReady_ = false;
    InGameTracker::Instance().ResetConnection();
}

void Archipelago::Shutdown() {
    InGameTracker::Instance().ResetConnection();
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    if (!ap) return;

    Logger::Log(LogLevel::File, "[AP] Gracefully closing connection during game shutdown:", slotName_);
    UpdateState(ArchipelagoConnectionState::Disconnected);
    ap_slot_connect_sent = false;
    ap.reset();
}

void Archipelago::Poll() {
    if (polling_.test_and_set()) return;
    struct PollGuard {
        std::atomic_flag& flag;
        ~PollGuard() { flag.clear(); }
    } pollGuard{polling_};

    try {
        if (ap) ap->poll();
        if (state_ == ArchipelagoConnectionState::Connected && !ap_slot_connect_sent) {
            ConnectSlot();
        }
        ProcessReceivedItems();
    } catch (const std::exception& exception) {
        Logger::Log(LogLevel::File, "[AP] Network poll failed; disconnecting safely:", exception.what());
        Disconnect();
    } catch (...) {
        Logger::Log(LogLevel::File, "[AP] Network poll failed with an unknown exception; disconnecting safely");
        Disconnect();
    }
}

void Archipelago::UpdateState(ArchipelagoConnectionState newState) {
    if (state_ == newState) return;
    state_ = newState;
}
