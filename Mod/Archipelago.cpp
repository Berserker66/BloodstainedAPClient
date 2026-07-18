#pragma once
#include "Archipelago.h"

#include <Basic.hpp>
#include <Engine_classes.hpp>
#include <ProjectBlood_classes.hpp>
#include <ProjectBlood_structs.hpp>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>

#include "GameManager.h"
#include "HookManager.h"
#include "Logger.h"
#include "ThreadQueue.h"
#include "Utils.h"
#include "apclient.hpp"
#include "apuuid.hpp"

std::unique_ptr<APClient> ap;
bool ap_slot_connect_sent = false;
double deathtime = -1;
bool awaiting_password = false;

const std::string GAME_NAME = "Bloodstained: Ritual of the Night";
std::string game_seed;
const int MAX_SEED_LENGTH = 32;

const std::string ITEM_INDEX_SAVE_VALUE = "ItemIndex";
const std::string CONNECTION_SAVE_PREFIX = "AP_LastConnection_";
const std::string CLEARED_LOCATION_SAVE_PREFIX = "AP_ClearedLocation_";
const int32_t CONNECTION_SAVE_VERSION = 1;
const size_t MAX_CONNECTION_FIELD_LENGTH = 1024;
const int32_t MAX_CLEARED_LOCATIONS = 4096;

#define UUID_FILE "uuid"
#define CERT_STORE "cacert.pem"

using json = nlohmann::json;

static std::optional<int32_t> LoadSavedValue(const std::string& saveValueName);
static std::optional<std::string> LoadSavedString(const std::string& saveValueName);

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
            _connected = ap->ConnectSlot(slotName_, password_, itemsHandling_, tags);
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
    if (!ap || !IsConnected() || !ap->is_data_package_valid()) return ItemLookupResult::NotReady;
    return ap->get_item_id(itemName) == APClient::INVALID_NAME_ID ? ItemLookupResult::UnknownItem
                                                                 : ItemLookupResult::KnownItem;
}

std::unordered_map<std::string, std::uint32_t> Archipelago::GetTrackerInventory() const {
    std::unordered_map<std::string, std::uint32_t> inventory;
    if (!ap || !IsConnected() || !ap->is_data_package_valid()) return inventory;

    for (const auto& [index, item] : receivedItems_) {
        std::string itemName = ap->get_item_name(item.item, ap->get_game());
        inventory[itemName]++;
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
    if (!ap || !IsConnected() || !ap->is_data_package_valid()) return false;
    const int64_t locationId = ap->get_location_id(locationName);
    return locationId != APClient::INVALID_NAME_ID && static_cast<std::uint64_t>(locationId) == expectedId &&
           ap->get_missing_locations().contains(locationId);
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

    int64_t apLocationId = ap->get_location_id(locationWithoutPrefix);
    if (apLocationId != APClient::INVALID_NAME_ID && locationExistsInWorld(apLocationId)) {
        resolution.exists = true;
        if (missingLocations.contains(apLocationId)) resolution.missing.insert(apLocationId);
    } else {
        for (int i = 0; i <= 3; i++) {
            std::string fullLocation = locationWithoutPrefix + "." + std::to_string(i);
            apLocationId = ap->get_location_id(fullLocation);
            if (apLocationId != APClient::INVALID_NAME_ID && locationExistsInWorld(apLocationId)) {
                resolution.exists = true;
                if (missingLocations.contains(apLocationId)) resolution.missing.insert(apLocationId);
            }
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
    Logger::Log(LogLevel::File, "[AP] Journaled cleared location:", locationId, "index:", index);
}

size_t Archipelago::SendMissingClearedLocations() {
    if (!ap || !IsConnected() || !ap->is_data_package_valid()) return 0;

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
    ap->LocationChecks(std::list<int64_t>(locations.begin(), locations.end()));
    Logger::Log("[AP] Reconciled ", locations.size(), " missing location checks");
    return locations.size();
}

LocationCheckResult Archipelago::SendLocationChecks(const std::string& locationId) {
    if (!locationId.starts_with("AP_")) return LocationCheckResult::UnknownLocation;
    RecordClearedLocation(locationId);
    if (!ap || !IsConnected() || !ap->is_data_package_valid()) return LocationCheckResult::NotReady;

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

    lastReceivedItemIndex_ = itemIndex;
    lastQueuedItemIndex_ = lastReceivedItemIndex_;
    localProgressLoaded_ = !legacyReceivedItemIndex_.has_value();
    receivedShardsReconciled_ = false;
    if (localProgressLoaded_) {
        Logger::Log("[AP] Loaded local item index:", lastReceivedItemIndex_, "prefix:", localSavePrefix_);
    }
}

void Archipelago::ProcessReceivedItems() {
    if (processingReceivedItems_) return;
    processingReceivedItems_ = true;
    struct ProcessingGuard {
        bool& active;
        ~ProcessingGuard() { active = false; }
    } processingGuard{processingReceivedItems_};

    if (!ap || !ap->is_data_package_valid() || !GameManager::Instance().CanReceiveItems()) {
        return;
    }
    TryMigrateLegacyProgress();
    if (!localProgressLoaded_) return;

    for (auto itemIt = pendingReceivedItems_.begin(); itemIt != pendingReceivedItems_.end();) {
        const auto& item = itemIt->second;
        if (item.index <= lastQueuedItemIndex_) {
            itemIt = pendingReceivedItems_.erase(itemIt);
            continue;
        }

        Logger::Log("[AP] Item - player:", item.player, " location:", item.location, " item:", item.item,
                    " index:", item.index, " local index:", lastReceivedItemIndex_);
        std::string itemId = ap->get_item_name(item.item, ap->get_game());
        if (!GivePlayerItem(itemId, false)) {
            Logger::Log(LogLevel::Warning, "[AP] Delaying unknown received item:", itemId, "index:", item.index);
            break;
        }

        int64_t itemIndex = item.index;
        std::string savePrefix = localSavePrefix_;
        std::string saveValueName = savePrefix + ITEM_INDEX_SAVE_VALUE;
        ThreadQueue::Instance().Enqueue([this, itemIndex, savePrefix, saveValueName]() {
            SaveLocalValue(saveValueName, static_cast<int32_t>(itemIndex));
            if (localSavePrefix_ == savePrefix) lastReceivedItemIndex_ = itemIndex;
        });
        lastQueuedItemIndex_ = item.index;
        itemIt = pendingReceivedItems_.erase(itemIt);
    }

    ReconcileReceivedShards();
}

void Archipelago::TryMigrateLegacyProgress() {
    if (!legacyReceivedItemIndex_ || receivedItems_.empty()) return;

    bool hasPreviouslyReceivedInventory = false;
    std::unordered_set<std::string> matchingItemIds;
    for (const auto& [index, item] : receivedItems_) {
        if (index > *legacyReceivedItemIndex_) continue;
        const std::string itemName = ap->get_item_name(item.item, ap->get_game());
        const auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
        if (itemId && GameManager::Instance().CheckAllInventories(*itemId)) {
            matchingItemIds.insert(*itemId);
            const bool isShard = GameManager::Instance().ItemHasItemCategories(
                *itemId, {SDK::ECarriedCatalog::AllShard, SDK::ECarriedCatalog::TriggerShard,
                          SDK::ECarriedCatalog::DirectionalShard, SDK::ECarriedCatalog::EffectiveShard,
                          SDK::ECarriedCatalog::EnchantShard, SDK::ECarriedCatalog::FamiliarShard});
            if (isShard || matchingItemIds.size() >= 3) {
                hasPreviouslyReceivedInventory = true;
                break;
            }
        }
    }

    lastReceivedItemIndex_ = hasPreviouslyReceivedInventory ? *legacyReceivedItemIndex_ : -1;
    lastQueuedItemIndex_ = lastReceivedItemIndex_;
    SaveLocalValue(localSavePrefix_ + ITEM_INDEX_SAVE_VALUE, static_cast<int32_t>(lastReceivedItemIndex_));
    Logger::Log("[AP]", hasPreviouslyReceivedInventory ? "Migrated legacy item index:" : "Ignored legacy item index:",
                *legacyReceivedItemIndex_, "loaded index:", lastReceivedItemIndex_);
    legacyReceivedItemIndex_.reset();
    localProgressLoaded_ = true;
}

void Archipelago::ReconcileReceivedShards() {
    if (receivedShardsReconciled_ || receivedItems_.empty() || !GameManager::Instance().CanReceiveItems()) return;

    std::unordered_set<std::string> repairedShardIds;
    for (const auto& [index, item] : receivedItems_) {
        if (index > lastReceivedItemIndex_) continue;

        std::string itemName = ap->get_item_name(item.item, ap->get_game());
        const auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
        if (!itemId || repairedShardIds.contains(*itemId) ||
            !GameManager::Instance().ItemHasItemCategories(
                *itemId, {SDK::ECarriedCatalog::AllShard, SDK::ECarriedCatalog::TriggerShard,
                          SDK::ECarriedCatalog::DirectionalShard, SDK::ECarriedCatalog::EffectiveShard,
                          SDK::ECarriedCatalog::EnchantShard, SDK::ECarriedCatalog::FamiliarShard}) ||
            GameManager::Instance().CheckAllInventories(*itemId)) {
            continue;
        }

        Logger::Log(LogLevel::Warning, "[AP] Repairing acknowledged but missing shard:", itemName, "index:", index);
        GameManager::Instance().GivePlayerItem(*itemId, true, shardDropInitialGrade_);
        repairedShardIds.insert(*itemId);
    }
    receivedShardsReconciled_ = true;
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

bool Archipelago::GivePlayerItem(std::string& itemName, bool shouldDisplay) {
    auto instance = GameManager::Instance;
    Logger::Log("Giving player item:", itemName);
    if (itemName.empty()) return false;
    if (itemName == "Nothing") return true;
    if (itemName.starts_with("Max")) {
        instance().GivePlayerMaxStatItem(itemName, shouldDisplay);
        return true;
        // Check for the bit coins (8 bit coin etc)
    } else if (std::isdigit(itemName[0]) && !itemName.starts_with("8") && !itemName.starts_with("16") &&
               !itemName.starts_with("32")) {
        int amount = std::stoi(itemName.substr(0, itemName.size() - 1));
        instance().GivePlayerCoin(amount, shouldDisplay);
        return true;
    }

    auto itemId = GameManager::Instance().GetIdFromDisplayName(itemName);
    if (itemId.has_value()) {
        if (instance().ItemHasItemCategories(
                itemId.value(), {SDK::ECarriedCatalog::AllShard, SDK::ECarriedCatalog::TriggerShard,
                                 SDK::ECarriedCatalog::DirectionalShard, SDK::ECarriedCatalog::EffectiveShard,
                                 SDK::ECarriedCatalog::EnchantShard, SDK::ECarriedCatalog::FamiliarShard})) {
            // Shards don't normally show in chat, here we override
            instance().GivePlayerItem(itemId.value(), true, shardDropInitialGrade_);
            return true;
        }
    }

    if (itemId.has_value()) {
        instance().GivePlayerItem(itemId.value(), shouldDisplay);
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
    lastReceivedItemIndex_ = -1;
    lastQueuedItemIndex_ = -1;
    pendingReceivedItems_.clear();
    receivedItems_.clear();
    receivedShardsReconciled_ = false;
    HookManager::ResetCompatibilityShardMasterData();

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
        for (const auto& item : items) {
            pendingReceivedItems_.insert_or_assign(item.index, item);
            receivedItems_.insert_or_assign(item.index, item);
        }
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
        UpdateState(ArchipelagoConnectionState::SlotConnected);
        SendMissingClearedLocations();
        HookManager::ApplyCompatibilityShardMasterData();
        SaveConnectionInfo();

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
    localSavePrefix_.clear();
    localProgressLoaded_ = false;
    legacyReceivedItemIndex_.reset();
    receivedShardsReconciled_ = false;
}

void Archipelago::Shutdown() {
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
