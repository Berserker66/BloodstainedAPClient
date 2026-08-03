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
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>
#include <utility>

#include "ClientVersion.h"
#include "EnemyDropShuffle.h"
#include "EnemyDropShuffleLogic.h"
#include "GameManager.h"
#include "HookManager.h"
#include "InGameTracker.h"
#include "Logger.h"
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

const std::string ITEM_INDEX_SAVE_VALUE = "ItemIndex";
const std::string ITEM_LEDGER_VERSION_VALUE = "ItemLedgerVersion";
const std::string ITEM_LEDGER_OBSERVED_COUNT_VALUE = "ItemLedgerObservedCount";
const std::string ITEM_LEDGER_AWARDED_COUNT_VALUE = "ItemLedgerAwardedCount";
const std::string CONNECTION_SAVE_PREFIX = "AP_LastConnection_";
const std::string CLEARED_LOCATION_SAVE_PREFIX = "AP_ClearedLocation_";
const std::string ENEMY_DROP_SHUFFLE_SEED_VALUE = "AP_EnemyDropShuffleSeed";
const std::string ENEMY_DROP_SHUFFLE_VERSION_VALUE = "AP_EnemyDropShuffleVersion";
const int32_t CONNECTION_SAVE_VERSION = 1;
const int32_t ITEM_LEDGER_VERSION = 1;
const int32_t MAX_ITEM_LEDGER_ENTRIES = 4096;
const size_t MAX_CONNECTION_FIELD_LENGTH = 1024;
const int32_t MAX_CLEARED_LOCATIONS = 4096;
const std::chrono::milliseconds ITEM_GRANT_INTERVAL(100);
const std::chrono::milliseconds ITEM_GRANT_RETRY_DELAY(500);

// These enemies award their shard exactly once, as part of their boss defeat sequence. If the client was not able
// to observe that short-lived shard actor, the save's completed-boss flag is the authoritative durable evidence
// that the corresponding location was cleared.
constexpr std::array<std::string_view, 13> BOSS_SHARD_ENEMY_IDS = {
    "N1001", "N1002", "N1003", "N1004", "N1005", "N1006", "N1008",
    "N2001", "N2004", "N2006", "N2007", "N2012", "N2013",
};

#define UUID_FILE "uuid"
#define CERT_STORE "cacert.pem"

using json = nlohmann::json;

static std::optional<int32_t> LoadSavedValue(const std::string& saveValueName);
static std::optional<std::string> LoadSavedString(const std::string& saveValueName);

static std::optional<int64_t> LoadInt64(const std::string& name) {
    const auto low = LoadSavedValue(name + "Low");
    const auto high = LoadSavedValue(name + "High");
    if (!low || !high) return std::nullopt;
    const uint64_t value = static_cast<uint32_t>(*low) | (static_cast<uint64_t>(static_cast<uint32_t>(*high)) << 32);
    return static_cast<int64_t>(value);
}

static bool IsShardOrSkillItem(const std::string& itemId) {
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
    return nativeName ? std::string(*nativeName) : "Unknown item " + std::to_string(itemId);
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
            if (wantsDeathlink_) {
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

ItemLookupResult Archipelago::GetItemLookupResult(const std::string& itemName) const {
    if (!ap || !IsConnected()) return ItemLookupResult::NotReady;
    return GetItemId(itemName) ? ItemLookupResult::KnownItem : ItemLookupResult::UnknownItem;
}

std::unordered_map<std::string, std::uint32_t> Archipelago::GetTrackerInventory() const {
    std::unordered_map<std::string, std::uint32_t> inventory;
    if (!ap || !IsConnected()) return inventory;

    for (const auto& [index, item] : receivedItems_) {
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

void Archipelago::ResetLocalLocationCache() {
    clearedLocations_.clear();
    clearedLocationsLoaded_ = false;
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

size_t Archipelago::SendMissingClearedLocations() {
    if (!ap || !IsConnected()) return 0;

    LoadClearedLocations();
    const auto missingLocations = ap->get_missing_locations();
    const auto checkedLocations = ap->get_checked_locations();
    std::set<int64_t> locations;
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
        ap->LocationChecks(std::list<int64_t>(locations.begin(), locations.end()));
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
    for (const std::string_view enemyId : BOSS_SHARD_ENEMY_IDS) {
        const std::string locationName = std::string(enemyId) + "_Shard";
        const auto locationId = GetLocationId(locationName);
        // Do not journal boss state for an older or differently configured world which has no such check.
        if (!locationId || !missingLocations.contains(*locationId)) continue;

        if (!gameInstance->IsCompletedBoss(FNameFromString(std::string(enemyId)))) continue;

        const std::string nativeLocationName = "AP_" + locationName;
        Logger::Log(LogLevel::File, "[AP] Recovered completed boss shard location from save:",
                    nativeLocationName);
        SendLocationChecks(nativeLocationName);
    }
}

LocationCheckResult Archipelago::SendLocationChecks(const std::string& locationId) {
    if (!locationId.starts_with("AP_")) return LocationCheckResult::UnknownLocation;
    RecordClearedLocation(locationId);
    if (!ap || !IsConnected()) return LocationCheckResult::NotReady;

    const auto missingLocations = ap->get_missing_locations();
    const auto checkedLocations = ap->get_checked_locations();
    auto requestedLocation = ResolveLocation(locationId, missingLocations, checkedLocations);
    SendMissingClearedLocations();

    if (!requestedLocation.exists) return LocationCheckResult::UnknownLocation;
    return requestedLocation.missing.empty() ? LocationCheckResult::AlreadyChecked : LocationCheckResult::Sent;
}

void Archipelago::Sync() {
    if (IsConnected() && ap) {
        pendingReceivedItems_.clear();
        LoadLocalProgress();
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

    SaveLocalValue(CONNECTION_SAVE_PREFIX + "DeathLink", wantsDeathlink_ ? 1 : 0);
    SaveLocalValue(CONNECTION_SAVE_PREFIX + "Version", CONNECTION_SAVE_VERSION);
    Logger::Log("[AP] Saved successful connection info to the current save");
}

void Archipelago::ApplySavedEnemyDropShuffle() {
    // The drop data table can survive a trip through the title screen. Restore
    // the PAK-owned rows first so loading an older/non-AP save cannot inherit
    // the previous save's slot-specific shuffle.
    EnemyDropShuffle::Reset();
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
    auto version = LoadSavedValue(CONNECTION_SAVE_PREFIX + "Version");
    if (!version || *version != CONNECTION_SAVE_VERSION) return std::nullopt;

    auto uri = LoadSavedString(CONNECTION_SAVE_PREFIX + "Uri");
    auto slotName = LoadSavedString(CONNECTION_SAVE_PREFIX + "Slot");
    auto password = LoadSavedString(CONNECTION_SAVE_PREFIX + "Password");
    auto deathLink = LoadSavedValue(CONNECTION_SAVE_PREFIX + "DeathLink");
    if (!uri || !slotName || !password || !deathLink || slotName->empty()) return std::nullopt;

    return ArchipelagoConnectionInfo{*uri, *slotName, *password, *deathLink != 0};
}

void Archipelago::LoadLocalProgress() {
    if (!ap || localSavePrefix_.empty()) return;

    legacyReceivedItemIndex_.reset();
    LoadItemLedger();
    if (itemLedgerLoaded_) {
        lastReceivedItemIndex_ = observedItemIdsByIndex_.empty() ? -1 : observedItemIdsByIndex_.rbegin()->first;
        lastQueuedItemIndex_ = lastReceivedItemIndex_;
        localProgressLoaded_ = true;
        receivedItemGrantPending_ = false;
        receivedItemRetryAt_ = {};
        receivedItemGeneration_++;
        receivedProgressionInventoryReconciled_ = false;
        Logger::Log(LogLevel::File, "[AP] Loaded item entitlement ledger; observed indices:",
                    observedItemIdsByIndex_.size(), "awarded item types:", awardedItemCounts_.size(),
                    "prefix:", localSavePrefix_);
        return;
    }

    auto getLocalValue = [this](const std::string& name, int32_t defaultValue) {
        std::string saveValueName = localSavePrefix_ + name;
        std::wstring wideSaveValueName(saveValueName.begin(), saveValueName.end());
        auto saveValueId = SDK::UKismetStringLibrary::Conv_StringToName(wideSaveValueName.c_str());
        int32_t value = defaultValue;
        bool isValid = false;
        SDK::UPBGameInstance::GetSavedValue(saveValueId, &value, &isValid);
        return isValid ? value : defaultValue;
    };

    const int32_t missingValue = std::numeric_limits<int32_t>::min();
    int32_t itemIndex = getLocalValue(ITEM_INDEX_SAVE_VALUE, missingValue);
    if (itemIndex == missingValue) {
        const auto savedConnection = LoadSavedConnectionInfo();
        auto* saveManager = SDK::UPBSaveManager::GetInstance();
        if (savedConnection && savedConnection->slotName == slotName_ && saveManager) {
            const int32_t saveSlotIndex = saveManager->GetLastUsedSaveSlotIndex();
            const std::string slotSuffix = "Save" + std::to_string(saveSlotIndex) + "_";
            const std::size_t suffixPosition = localSavePrefix_.rfind(slotSuffix);
            if (suffixPosition != std::string::npos) {
                const std::string legacyPrefix = localSavePrefix_.substr(0, suffixPosition);
                std::string legacyValueName = legacyPrefix + ITEM_INDEX_SAVE_VALUE;
                std::wstring wideLegacyValueName(legacyValueName.begin(), legacyValueName.end());
                auto legacyValueId = SDK::UKismetStringLibrary::Conv_StringToName(wideLegacyValueName.c_str());
                bool legacyValueValid = false;
                SDK::UPBGameInstance::GetSavedValue(legacyValueId, &itemIndex, &legacyValueValid);
                if (legacyValueValid) {
                    legacyReceivedItemIndex_ = itemIndex;
                    itemIndex = -1;
                    Logger::Log("[AP] Found legacy item index pending save validation:",
                                *legacyReceivedItemIndex_, "save slot:", saveSlotIndex);
                } else {
                    itemIndex = -1;
                }
            }
        } else {
            itemIndex = -1;
        }
        if (itemIndex == missingValue) itemIndex = -1;
    }

    if (itemIndex >= 0 && !legacyReceivedItemIndex_) {
        legacyReceivedItemIndex_ = itemIndex;
        itemIndex = -1;
        Logger::Log(LogLevel::File, "[AP] Found pre-ledger item index pending entitlement migration:",
                    *legacyReceivedItemIndex_);
    }

    lastReceivedItemIndex_ = itemIndex;
    lastQueuedItemIndex_ = lastReceivedItemIndex_;
    localProgressLoaded_ = !legacyReceivedItemIndex_.has_value();
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    receivedProgressionInventoryReconciled_ = false;
    if (localProgressLoaded_) {
        Logger::Log("[AP] Loaded local item index:", lastReceivedItemIndex_, "prefix:", localSavePrefix_);
    }
}

void Archipelago::LoadItemLedger() {
    const bool sameInMemoryLedger = loadedLedgerPrefix_ == localSavePrefix_;
    if (!sameInMemoryLedger) {
        observedItemIdsByIndex_.clear();
        awardedItemCounts_.clear();
    }

    itemLedgerLoaded_ = sameInMemoryLedger;
    const auto version = LoadSavedValue(localSavePrefix_ + ITEM_LEDGER_VERSION_VALUE);
    if (!version || *version != ITEM_LEDGER_VERSION) {
        loadedLedgerPrefix_ = localSavePrefix_;
        return;
    }
    itemLedgerLoaded_ = true;

    const int32_t observedCount =
        LoadSavedValue(localSavePrefix_ + ITEM_LEDGER_OBSERVED_COUNT_VALUE).value_or(0);
    if (observedCount >= 0 && observedCount <= MAX_ITEM_LEDGER_ENTRIES) {
        for (int32_t entry = 0; entry < observedCount; entry++) {
            const std::string prefix = localSavePrefix_ + "ItemLedgerObserved" + std::to_string(entry);
            const auto index = LoadInt64(prefix + "Index");
            const auto itemId = LoadInt64(prefix + "Item");
            if (index && itemId && *index >= 0) observedItemIdsByIndex_.insert_or_assign(*index, *itemId);
        }
    } else {
        Logger::Log(LogLevel::File, "[AP] Ignoring invalid observed-item ledger count:", observedCount);
    }

    const int32_t awardedCount =
        LoadSavedValue(localSavePrefix_ + ITEM_LEDGER_AWARDED_COUNT_VALUE).value_or(0);
    if (awardedCount >= 0 && awardedCount <= MAX_ITEM_LEDGER_ENTRIES) {
        for (int32_t entry = 0; entry < awardedCount; entry++) {
            const std::string prefix = localSavePrefix_ + "ItemLedgerAwarded" + std::to_string(entry);
            const auto itemId = LoadInt64(prefix + "Item");
            const int32_t count = LoadSavedValue(prefix + "Count").value_or(0);
            if (itemId && count > 0) {
                auto& inMemoryCount = awardedItemCounts_[*itemId];
                inMemoryCount = std::max(inMemoryCount, static_cast<std::uint32_t>(count));
            }
        }
    } else {
        Logger::Log(LogLevel::File, "[AP] Ignoring invalid awarded-item ledger count:", awardedCount);
    }
    loadedLedgerPrefix_ = localSavePrefix_;
}

void Archipelago::PersistObservedItemLedger() const {
    if (localSavePrefix_.empty()) return;
    SaveLocalValue(localSavePrefix_ + ITEM_LEDGER_VERSION_VALUE, ITEM_LEDGER_VERSION);
    SaveLocalValue(localSavePrefix_ + ITEM_LEDGER_OBSERVED_COUNT_VALUE,
                   static_cast<int32_t>(observedItemIdsByIndex_.size()));
    int32_t entry = 0;
    for (const auto& [index, itemId] : observedItemIdsByIndex_) {
        const std::string prefix = localSavePrefix_ + "ItemLedgerObserved" + std::to_string(entry++);
        SaveLocalInt64(prefix + "Index", index);
        SaveLocalInt64(prefix + "Item", itemId);
    }
}

void Archipelago::PersistAwardedItemCounts() const {
    if (localSavePrefix_.empty()) return;
    std::vector<std::pair<int64_t, std::uint32_t>> counts(awardedItemCounts_.begin(), awardedItemCounts_.end());
    std::ranges::sort(counts);
    SaveLocalValue(localSavePrefix_ + ITEM_LEDGER_VERSION_VALUE, ITEM_LEDGER_VERSION);
    SaveLocalValue(localSavePrefix_ + ITEM_LEDGER_AWARDED_COUNT_VALUE, static_cast<int32_t>(counts.size()));
    int32_t entry = 0;
    for (const auto& [itemId, count] : counts) {
        const std::string prefix = localSavePrefix_ + "ItemLedgerAwarded" + std::to_string(entry++);
        SaveLocalInt64(prefix + "Item", itemId);
        SaveLocalValue(prefix + "Count", static_cast<int32_t>(count));
    }
}

void Archipelago::UpdateObservedItemLedger() {
    if (localSavePrefix_.empty()) return;
    std::map<int64_t, int64_t> serverLedger;
    for (const auto& [index, item] : receivedItems_) serverLedger.insert_or_assign(index, item.item);
    if (serverLedger == observedItemIdsByIndex_) return;

    for (const auto& [index, itemId] : serverLedger) {
        const auto previous = observedItemIdsByIndex_.find(index);
        if (previous != observedItemIdsByIndex_.end() && previous->second != itemId) {
            Logger::Log(LogLevel::File, "[AP] Server item history changed at index:", index,
                        "previous ID:", previous->second, "current ID:", itemId);
        }
    }
    observedItemIdsByIndex_ = std::move(serverLedger);
    itemLedgerLoaded_ = true;
    loadedLedgerPrefix_ = localSavePrefix_;
    PersistObservedItemLedger();
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
    TryMigrateLegacyProgress();
    if (!localProgressLoaded_) return;

    // Repair acknowledged progression before newer deliveries. Otherwise one
    // unresolved later item can indefinitely starve recovery of a required item
    // such as Zangetsuto that an older client already committed as received.
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
    const auto nativeItemName = GetNativeItemName(item.item);
    const std::string itemName = nativeItemName ? std::string(*nativeItemName) : std::string();
    const std::string displayItemName = GetDisplayItemName(item.item);
    const std::string savePrefix = localSavePrefix_;
    const uint64_t generation = receivedItemGeneration_;
    Logger::Log(LogLevel::File, "[AP] Attempting received item grant:", displayItemName, "ID:", item.item,
                "index:", itemIndex, "committed index:", lastReceivedItemIndex_);

    receivedItemGrantPending_ = true;
    lastQueuedItemIndex_ = itemIndex;
    if (!GivePlayerItem(itemName, false,
                        [this, itemIndex, itemId = item.item, itemName, savePrefix, generation](ItemGrantResult result) {
                            CompleteReceivedItem(itemIndex, itemId, itemName, savePrefix, generation, result);
                        })) {
        receivedItemGrantPending_ = false;
        lastQueuedItemIndex_ = lastReceivedItemIndex_;
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
    lastQueuedItemIndex_ = lastReceivedItemIndex_;
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
    itemLedgerLoaded_ = true;
    loadedLedgerPrefix_ = localSavePrefix_;
    PersistAwardedItemCounts();
    lastReceivedItemIndex_ = std::max(lastReceivedItemIndex_, itemIndex);
    SaveLocalValue(savePrefix + ITEM_INDEX_SAVE_VALUE, static_cast<int32_t>(lastReceivedItemIndex_));
    lastQueuedItemIndex_ = itemIndex;
    receivedItemRetryAt_ = std::chrono::steady_clock::now() + ITEM_GRANT_INTERVAL;
    if (result == ItemGrantResult::AtCapacity) {
        Logger::Log(LogLevel::File, "[AP] Received item already at native inventory capacity:", itemName,
                    "committed index:", itemIndex);
    } else {
        Logger::Log(LogLevel::File, "[AP] Received item grant succeeded:", itemName, "committed index:", itemIndex);
    }
}

void Archipelago::TryMigrateLegacyProgress() {
    if (!legacyReceivedItemIndex_ || receivedItems_.empty()) return;

    bool hasPreviouslyReceivedInventory = false;
    std::unordered_set<std::string> matchingItemIds;
    for (const auto& [index, item] : receivedItems_) {
        if (index > *legacyReceivedItemIndex_) continue;
        const auto nativeItemName = GetNativeItemName(item.item);
        if (!nativeItemName) continue;
        const std::string itemName(*nativeItemName);
        const auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
        if (itemId && GameManager::Instance().CheckAllInventories(*itemId)) {
            matchingItemIds.insert(*itemId);
            const bool isShard = IsShardOrSkillItem(*itemId);
            if (isShard || matchingItemIds.size() >= 3) {
                hasPreviouslyReceivedInventory = true;
                break;
            }
        }
    }

    lastReceivedItemIndex_ = hasPreviouslyReceivedInventory ? *legacyReceivedItemIndex_ : -1;
    lastQueuedItemIndex_ = lastReceivedItemIndex_;
    if (hasPreviouslyReceivedInventory) {
        for (const auto& [index, item] : receivedItems_) {
            if (index > *legacyReceivedItemIndex_) break;
            awardedItemCounts_[item.item]++;
        }
    }
    itemLedgerLoaded_ = true;
    loadedLedgerPrefix_ = localSavePrefix_;
    UpdateObservedItemLedger();
    PersistAwardedItemCounts();
    SaveLocalValue(localSavePrefix_ + ITEM_INDEX_SAVE_VALUE, static_cast<int32_t>(lastReceivedItemIndex_));
    Logger::Log("[AP]", hasPreviouslyReceivedInventory ? "Migrated legacy item index:" : "Ignored legacy item index:",
                *legacyReceivedItemIndex_, "loaded index:", lastReceivedItemIndex_);
    legacyReceivedItemIndex_.reset();
    localProgressLoaded_ = true;
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
                Logger::Log(LogLevel::File, "[AP] Missing progression item or shard repair succeeded:",
                            itemName, "index:", index);
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
    if (!wantsDeathlink_) return;
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

bool Archipelago::Connect(const std::string& slotName, const std::string& password, const std::string uri = "",
                          const bool& wantsDeathlink = false) {
    if (!GameManager::Instance().IsPlayerLoadedInGame()) {
        Logger::Log("Player is not loaded in game");
        return false;
    }

    slotName_ = slotName;
    password_ = password;
    std::string normalizedUri = uri;
    if (normalizedUri.starts_with("ws://")) normalizedUri.erase(0, 5);
    if (normalizedUri.starts_with("wss://")) normalizedUri.erase(0, 6);
    currentUri_ = normalizedUri;
    wantsDeathlink_ = wantsDeathlink;
    Logger::Log(LogLevel::File, "[AP] Connecting slot:", slotName_, "server:",
                normalizedUri.empty() ? APClient::DEFAULT_URI : normalizedUri);
    ResetLocalLocationCache();
    localSavePrefix_.clear();
    localProgressLoaded_ = false;
    itemLedgerLoaded_ = false;
    lastReceivedItemIndex_ = -1;
    lastQueuedItemIndex_ = -1;
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    pendingReceivedItems_.clear();
    receivedItems_.clear();
    receivedProgressionInventoryReconciled_ = false;
    HookManager::ResetCompatibilityShardMasterData();
    InGameTracker::Instance().ResetConnection();

    Logger::Log(LogLevel::Debug, "[AP]", "Connecting Player: ", slotName_, "with uri: ", normalizedUri);
    UpdateState(ArchipelagoConnectionState::Connecting);

    ap.reset();
    std::string uri_without_scheme = normalizedUri.empty() ? APClient::DEFAULT_URI : normalizedUri;

    std::string uuid = ap_get_uuid(UUID_FILE, uri_without_scheme);

    ap.reset(new APClient(uuid, GAME_NAME, normalizedUri.empty() ? APClient::DEFAULT_URI : normalizedUri, CERT_STORE));
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
        UpdateObservedItemLedger();
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
        HookManager::ApplyCompatibilityShardMasterData();
    });

    ap->set_slot_connected_handler([this](const json& slotData) {
        Logger::Log(LogLevel::File, "[AP] Slot connected:", slotName_, "team:", ap->get_team_number(), "player:",
                    ap->get_player_number());
        Logger::Log("SLOT CONNECTED");
        Logger::Log(slotData.dump());

        slotData_ = slotData;
        auto* saveManager = SDK::UPBSaveManager::GetInstance();
        const int32_t saveSlotIndex = saveManager ? saveManager->GetLastUsedSaveSlotIndex() : -1;
        localSavePrefix_ = "AP_" + ap->get_seed() + "_" + std::to_string(ap->get_team_number()) + "_" +
                           std::to_string(ap->get_player_number()) + "_Save" + std::to_string(saveSlotIndex) + "_";
        LoadLocalProgress();
        UpdateObservedItemLedger();
        UpdateState(ArchipelagoConnectionState::SlotConnected);
        SendMissingClearedLocations();
        ReconcileCompletedBossShardLocations();
        HookManager::ApplyCompatibilityShardMasterData();
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
        HookManager::ResetCompatibilityShardMasterData();
        ap_slot_connect_sent = false;
        UpdateState(ArchipelagoConnectionState::Disconnected);
    });

    ap->set_slot_disconnected_handler([this]() {
        Logger::Log(LogLevel::File, "[AP] Slot disconnected:", slotName_);
        Logger::Log(LogLevel::Debug, "[AP]", "Player: ", slotName_, "disconnected");
        HookManager::ResetCompatibilityShardMasterData();
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
        if (wantsDeathlink_) {
            Logger::Log("Recieved DeathLink");
            auto tagsIt = cmd.find("tags");
            auto dataIt = cmd.find("data");
            if (tagsIt != cmd.end() && tagsIt->is_array() &&
                std::find(tagsIt->begin(), tagsIt->end(), "DeathLink") != tagsIt->end()) {
                if (dataIt != cmd.end() && dataIt->is_object()) {
                    json data = *dataIt;
                    if (data["source"].get<std::string>() != slotName) {
                        std::string source =
                            data["source"].is_string() ? data["source"].get<std::string>().c_str() : "???";
                        std::string cause =
                            data["cause"].is_string() ? data["cause"].get<std::string>().c_str() : "???";
                        GameManager::Instance().SendInGameNotification(source + ": " + cause, 346);  // 346 for skull id
                        Logger::Log("Died by the hands of " + source + " : " + cause);
                        pendingDeathlink_ = true;
                    }
                } else {
                    Logger::Log("Bad deathlink packet!");
                }
            }
        }
    });

    return true;
}

void Archipelago::AbortPassword() { awaiting_password = false; }

void Archipelago::Disconnect() {
    Logger::Log(LogLevel::File, "[AP] Manual disconnect:", slotName_);
    HookManager::ResetCompatibilityShardMasterData();
    UpdateState(ArchipelagoConnectionState::Disconnected);
    ap.reset();
    ap_slot_connect_sent = false;
    pendingReceivedItems_.clear();
    receivedItems_.clear();
    receivedItemGrantPending_ = false;
    receivedItemRetryAt_ = {};
    receivedItemGeneration_++;
    localSavePrefix_.clear();
    localProgressLoaded_ = false;
    legacyReceivedItemIndex_.reset();
    receivedProgressionInventoryReconciled_ = false;
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

    if (ap) ap->poll();
    if (state_ == ArchipelagoConnectionState::Connected && !ap_slot_connect_sent) {
        ConnectSlot();
    }
    ProcessReceivedItems();
}

void Archipelago::UpdateState(ArchipelagoConnectionState newState) {
    if (state_ == newState) return;
    state_ = newState;
}
