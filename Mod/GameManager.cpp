#pragma once
#include "GameManager.h"

#include <Basic.hpp>
#include <PBInterfaceHUDBP_classes.hpp>
#include <PB_Chr_PlayerRoot_classes.hpp>
#include <PB_Chr_Root_classes.hpp>
#include <ProjectBlood_classes.hpp>
#include <ProjectBlood_structs.hpp>
#include <UnrealContainers.hpp>
#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "Archipelago.h"
#include "Logger.h"
#include "ThreadQueue.h"
#include "TrackerData.generated.h"
#include "Utils.h"

#define PLAYER_NAME "Chr_P0000_C_0"

namespace {

constexpr std::string_view WAYSTONE_ITEM_ID = "Waystone";
constexpr int WAYSTONE_QUANTITY = 5;

struct FreeUpdateItemBinding {
    std::string_view nativeName;
    SDK::ECarriedCatalog category;
};

// These free-update items are part of the randomizer's ordinary item pool, but
// vanilla grants them automatically when a new save is created. Keep their
// expected categories here so the new-game grant can be suppressed temporarily
// without affecting later Archipelago deliveries.
constexpr std::array FREE_UPDATE_ITEM_BINDINGS = {
    FreeUpdateItemBinding{"PirateGun1", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"PirateGun3", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"PirateGun5", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"PirateSword1", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"PirateSword3", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"PirateSword5", SDK::ECarriedCatalog::Weapon},
    FreeUpdateItemBinding{"ShantaeBandana", SDK::ECarriedCatalog::Head},
    FreeUpdateItemBinding{"ShantaeOutfit1", SDK::ECarriedCatalog::Body},
    FreeUpdateItemBinding{"ShantaeOutfit3", SDK::ECarriedCatalog::Body},
    FreeUpdateItemBinding{"ShantaeOutfit5", SDK::ECarriedCatalog::Body},
    FreeUpdateItemBinding{"ShantaeVest", SDK::ECarriedCatalog::Accessory1},
    FreeUpdateItemBinding{"ShantaeTiara", SDK::ECarriedCatalog::Accessory1},
    FreeUpdateItemBinding{"JourneyScarf", SDK::ECarriedCatalog::Muffler},
    FreeUpdateItemBinding{"Fireball", SDK::ECarriedCatalog::DirectionalShard},
};

const bloodstained::tracker::generated::PaidDlcItemBinding* FindPaidDlcItem(std::string_view itemId) {
    const auto& bindings = bloodstained::tracker::generated::PAID_DLC_ITEM_BINDINGS;
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [itemId](const auto& entry) {
        return entry.native_name == itemId;
    });
    return binding == bindings.end() ? nullptr : &*binding;
}

SDK::ECarriedCatalog NativeCategory(
    bloodstained::tracker::generated::ItemCatalogCategory category) {
    using Category = bloodstained::tracker::generated::ItemCatalogCategory;
    switch (category) {
        case Category::WEAPON: return SDK::ECarriedCatalog::Weapon;
        case Category::HEAD: return SDK::ECarriedCatalog::Head;
        case Category::BODY: return SDK::ECarriedCatalog::Body;
        case Category::ACCESSORY1: return SDK::ECarriedCatalog::Accessory1;
        case Category::MUFFLER: return SDK::ECarriedCatalog::Muffler;
        case Category::TRIGGERSHARD: return SDK::ECarriedCatalog::TriggerShard;
        case Category::FAMILIARSHARD: return SDK::ECarriedCatalog::FamiliarShard;
    }
    return SDK::ECarriedCatalog::None;
}

bool IsShardCategory(SDK::ECarriedCatalog category) {
    return category == SDK::ECarriedCatalog::TriggerShard ||
           category == SDK::ECarriedCatalog::EffectiveShard ||
           category == SDK::ECarriedCatalog::DirectionalShard ||
           category == SDK::ECarriedCatalog::EnchantShard ||
           category == SDK::ECarriedCatalog::FamiliarShard ||
           category == SDK::ECarriedCatalog::AllShard;
}

DlcOwnership PreparePaidDlcCatalogRow(const std::string& itemId) {
    const auto* binding = FindPaidDlcItem(itemId);
    if (!binding) return DlcOwnership::NotApplicable;
    const auto ownership = GameManager::Instance().GetDlcOwnership(binding->dlc_key);
    if (ownership != DlcOwnership::Owned) return ownership;

    auto* itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    if (!itemTable) return DlcOwnership::NotReady;
    const auto expectedCategory = NativeCategory(binding->category);
    const auto expectedItemId = FNameFromString(itemId);
    for (const auto& pair : itemTable->RowMap) {
        // FName identity is case-insensitive, while ToString() preserves whichever spelling was registered
        if (pair.Key() != expectedItemId) continue;
        auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
        if (!row) return DlcOwnership::NotReady;
        if (row->ItemType == SDK::ECarriedCatalog::None) {
            // The static pak intentionally disables paid inventory rows. Restore the native category only for
            // an owner of the matching DLC; the game supplies the corresponding models, icons, and behavior.
            row->ItemType = expectedCategory;
            Logger::Log(LogLevel::File, "[AP] Restored owned paid-DLC ItemMaster category:", itemId,
                        "DLC:", binding->dlc_key);
        }
        return row->ItemType == expectedCategory ? DlcOwnership::Owned : DlcOwnership::NotReady;
    }
    return DlcOwnership::NotReady;
}

void RecoverArchipelagoPlaceholderShards() {
    auto* player = GameManager::Instance().Player();
    if (!player || !player->CharacterInventory) return;
    auto* inventory = player->CharacterInventory;

    std::vector<SDK::FName> placeholders;
    auto collect = [&placeholders](const SDK::TArray<SDK::FPBItemCatalogData>& items) {
        for (const auto& item : items) {
            if (item.ID.ToString().starts_with("AP_")) {
                placeholders.push_back(item.ID);
            }
        }
    };
    collect(inventory->myTriggerShards);
    collect(inventory->myEffectiveShards);
    collect(inventory->myDirectionalShards);
    collect(inventory->myEnchantShards);
    collect(inventory->myFamiliarShards);
    collect(inventory->mySkills);

    int recovered = 0;
    for (const auto& itemId : placeholders) {
        if (Archipelago::Instance().SendLocationChecks(itemId.ToString()) != LocationCheckResult::UnknownLocation) {
            recovered++;
        }
    }
    if (recovered > 0) {
        Logger::Log(LogLevel::File, "[AP] Recovered location checks from retained placeholder shards:", recovered);
    }
}

}  // namespace

GameManager& GameManager::Instance() {
    static GameManager instance;
    return instance;
}

bool GameManager::IsInstanceValid(SDK::UObject* object, const char* c_str) {
    if (!object) {
        std::string formatted = std::format("waiting for {} to initialize", c_str);
        Logger::Log(formatted);
        return false;
    }
    return true;
}

bool GameManager::IsPlayerLoadedInGame() {
    auto playerController = GameManager::Instance().PlayerController();
    if (!playerController || !playerController->Class) return false;
    auto playerControllerName = playerController->GetName();
    if (playerControllerName != "PBPlayerController_0") return false;
    auto player = (SDK::APB_Chr_PlayerRoot_C*)playerController->Pawn;
    // This method is used by PostInit's worker-thread polling loop. A generated
    // UKismetSystemLibrary::IsValid call goes through ProcessEvent, so restrict
    // the early readiness check to raw UObject state.
    if (!player || !player->Class) return false;
    return true;
}

DlcOwnership GameManager::GetDlcOwnership(std::string_view dlcKey) const {
    auto* dlcManager = SDK::UPBDLCManager::GetInstance();
    if (!dlcManager || !dlcManager->HasFinishedFindAllDLCs()) return DlcOwnership::NotReady;
    return dlcManager->HasDLC(FNameFromString(std::string(dlcKey)))
               ? DlcOwnership::Owned
               : DlcOwnership::NotOwned;
}

DlcOwnership GameManager::GetPaidDlcItemOwnership(std::string_view nativeItemId) const {
    const auto* binding = FindPaidDlcItem(nativeItemId);
    return binding ? GetDlcOwnership(binding->dlc_key) : DlcOwnership::NotApplicable;
}

bool GameManager::IsPaidDlcShard(std::string_view nativeItemId) const {
    const auto* binding = FindPaidDlcItem(nativeItemId);
    return binding && binding->is_shard;
}

bool GameManager::PrimeOwnedPaidDlcCatalogRows() {
    if (suppressRandomizedDlcCatalogForNewGame_) return false;
    if (paidDlcCatalogPrimed_) return true;

    auto* itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    if (!itemTable) return false;

    int restoredRows = 0;
    int unresolvedRows = 0;
    std::string firstUnresolvedRow;
    for (const auto& binding : bloodstained::tracker::generated::PAID_DLC_ITEM_BINDINGS) {
        const auto ownership = GetDlcOwnership(binding.dlc_key);
        if (ownership == DlcOwnership::NotReady) return false;
        if (ownership != DlcOwnership::Owned) continue;

        const auto expectedCategory = NativeCategory(binding.category);
        const auto expectedItemId = FNameFromString(std::string(binding.native_name));
        bool found = false;
        for (const auto& pair : itemTable->RowMap) {
            if (pair.Key() != expectedItemId) continue;
            found = true;
            auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
            if (!row) {
                unresolvedRows++;
                if (firstUnresolvedRow.empty()) firstUnresolvedRow = std::string(binding.native_name);
                break;
            }
            if (row->ItemType != expectedCategory) {
                // Story inventory regeneration consults ItemMaster while loading. Restore every owned row at
                // the title screen so native DLC inventory survives that regeneration, not just future AP grants.
                row->ItemType = expectedCategory;
                restoredRows++;
            }
            break;
        }
        if (!found) {
            unresolvedRows++;
            if (firstUnresolvedRow.empty()) firstUnresolvedRow = std::string(binding.native_name);
        }
    }

    if (unresolvedRows == 0) {
        paidDlcCatalogPrimed_ = true;
        Logger::Log(LogLevel::File, "[AP] Primed owned paid-DLC ItemMaster rows before story load; restored:",
                    restoredRows);
    } else if (!paidDlcCatalogPendingLogged_) {
        // DLC packages may finish contributing their rows over multiple title ticks. Keep retrying, but do not
        // let one late row prevent already available rows in later packs from being restored or recovered.
        paidDlcCatalogPendingLogged_ = true;
        Logger::Log(LogLevel::File, "[AP] Paid-DLC ItemMaster priming remains partial; unresolved rows:",
                    unresolvedRows, "first:", firstUnresolvedRow, "restored this pass:", restoredRows);
    }
    return true;
}

void GameManager::SuppressRandomizedDlcCatalogRowsForNewGame() {
    suppressRandomizedDlcCatalogForNewGame_ = true;
    paidDlcCatalogPrimed_ = false;
    paidDlcCatalogPendingLogged_ = false;

    auto* itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    if (!itemTable) {
        Logger::Log(LogLevel::Error,
                    "[AP] Could not suppress randomized DLC starter items because ItemMaster is unavailable");
        return;
    }

    int suppressedRows = 0;
    for (const auto& binding : bloodstained::tracker::generated::PAID_DLC_ITEM_BINDINGS) {
        const auto expectedItemId = FNameFromString(std::string(binding.native_name));
        for (const auto& pair : itemTable->RowMap) {
            if (pair.Key() != expectedItemId) continue;
            auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
            if (row && row->ItemType != SDK::ECarriedCatalog::None) {
                row->ItemType = SDK::ECarriedCatalog::None;
                suppressedRows++;
            }
            break;
        }
    }

    for (const auto& binding : FREE_UPDATE_ITEM_BINDINGS) {
        const auto expectedItemId = FNameFromString(std::string(binding.nativeName));
        for (const auto& pair : itemTable->RowMap) {
            if (pair.Key() != expectedItemId) continue;
            auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
            if (row && row->ItemType != SDK::ECarriedCatalog::None) {
                row->ItemType = SDK::ECarriedCatalog::None;
                suppressedRows++;
            }
            break;
        }
    }

    Logger::Log(LogLevel::File,
                "[AP] Suppressed vanilla randomized-DLC starter grants for new save; disabled rows:",
                suppressedRows);
}

void GameManager::RestoreRandomizedDlcCatalogRowsAfterNewGameInit() {
    if (!suppressRandomizedDlcCatalogForNewGame_) return;
    suppressRandomizedDlcCatalogForNewGame_ = false;

    auto* itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    int restoredFreeRows = 0;
    if (itemTable) {
        for (const auto& binding : FREE_UPDATE_ITEM_BINDINGS) {
            const auto expectedItemId = FNameFromString(std::string(binding.nativeName));
            for (const auto& pair : itemTable->RowMap) {
                if (pair.Key() != expectedItemId) continue;
                auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
                if (row && row->ItemType == SDK::ECarriedCatalog::None) {
                    row->ItemType = binding.category;
                    restoredFreeRows++;
                }
                break;
            }
        }
    }
    Logger::Log(LogLevel::File,
                "[AP] Restored free-update ItemMaster rows after new-save initialization:",
                restoredFreeRows);
    PrimeOwnedPaidDlcCatalogRows();
}

bool GameManager::PopulateDisplayToItemIdTable() {
    auto itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    auto inventory = GameManager::Instance().Player()->CharacterInventory;
    auto text = SDK::UKismetTextLibrary::Conv_StringToText(FStringFromString("ahhhhhh"));
    // SDK::FPBItemCatalogData* realItemData = (SDK::FPBItemCatalogData*)pair.Value();

    for (auto& pair : itemTable->RowMap) {
        pair.Value();
        std::string itemId = pair.Key().ToString();
        // Protocol bindings use authoritative native catalog IDs whenever one exists. Accepting identity
        // mappings also keeps visually identical level variants distinct (their localized names are shared).
        DisplayNameToItemId[itemId] = itemId;
        auto itemName = FNameFromString(itemId);
        SDK::FPBItemCatalogData itemData = SDK::FPBItemCatalogData();

        inventory->GetItemDataById(itemName, &itemData);
        std::string lowerItemId = itemId;
        std::ranges::transform(lowerItemId, lowerItemId.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerItemId.find("swordwhip") != std::string::npos ||
            lowerItemId.find("sword_whip") != std::string::npos) {
            Logger::Log(LogLevel::File, "[AP] Live Sword Whip ItemMaster candidate; row:", itemId,
                        "resolved ID:", itemData.ID.ToString(), "display:", itemData.Name.ToString(),
                        "category:", static_cast<int>(itemData.itemCategory), "quantity:", itemData.Num,
                        "maximum:", itemData.MaxNum);
        }
        if (itemData.Name.ToString().empty()) continue;
        if (itemData.Name.ToString().starts_with("AP_")) continue;
        if (DisplayNameToItemId.count(itemData.Name.ToString()) >= 1) continue;

        std::string displayName = itemData.Name.ToString();
        DisplayNameToItemId[displayName] = itemId;
    }

    // Do not depend on the active localization or RowMap iteration order for protocol items. The APWorld
    // exporter supplies the canonical display name and native catalog ID for every item in its catalog.
    for (const auto& binding : bloodstained::tracker::generated::ITEM_BINDINGS) {
        const std::string nativeId(binding.native_name);
        DisplayNameToItemId[nativeId] = nativeId;
        DisplayNameToItemId[std::string(binding.display_name)] = nativeId;
    }

    // The crossover catalog also contains COL_Zangetsuto. Both rows can resolve
    // to the same display name, while Archipelago progression specifically uses
    // the story weapon whose native ID is Swordsman. RowMap iteration order must
    // not decide which one received items grant or reconciliation inspects.
    // Canonical Archipelago names come from data/translation/Item.json and can
    // differ from the active runtime localization. Keep required disambiguation
    // and known non-localized names deterministic here.
    DisplayNameToItemId["Carnot's Rebuke"] = "SteamFlatWideEnd";
    DisplayNameToItemId["Zangetsuto"] = "Swordsman";
    return true;
}

std::optional<std::string> GameManager::GetIdFromDisplayName(const std::string& itemId) {
    auto it = DisplayNameToItemId.find(itemId);
    if (it == DisplayNameToItemId.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool GameManager::Init() {
    if (!GameManager::IsInstanceValid(GameManager::Engine(), "Engine")) return false;
    if (!GameManager::IsInstanceValid(GameManager::World(), "World")) return false;
    Logger::Log("Game Manager initialized successfully");
    initCompleted = true;
    return true;
}

bool GameManager::PostInit() {
    if (!GameManager::IsInstanceValid(GameManager::GameInstance(), "GameInstance")) return false;
    if (!GameManager::IsInstanceValid(GameManager::PlayerController(), "PlayerController")) return false;
    // Access ingame Table

    // Wait for the player to load in
    if (!GameManager::Instance().IsPlayerLoadedInGame()) return false;
    // ProcessNamePool();
    Logger::Log("Populated Display Table");
    Sleep(200);
    ThreadQueue::Instance().Enqueue([this] {
        // A brand-new story reaches this path without OnLoadGameCompletely. Its
        // vanilla starter grant pass is over, so re-enable the temporarily
        // suppressed rows before the first AP delivery can be processed.
        RestoreRandomizedDlcCatalogRowsAfterNewGameInit();
        RecoverArchipelagoPlaceholderShards();
        GameManager::Instance().ApplyWaystoneSafety();
        GameManager::Instance().PopulateDisplayToItemIdTable();
    });
    // GameManager::Instance().PopulateDisplayToItemIdTable();
    Logger::Log("Game Manager POST initialized successfully");
    postInitCompleted = true;
    return true;
}

bool GameManager::IsInitialized() { return initCompleted && postInitCompleted ? true : false; }

bool GameManager::CanReceiveItems() {
    if (!IsPlayerLoadedInGame()) return false;
    auto* player = Player();
    return player && player->CharacterInventory && !DisplayNameToItemId.empty();
}

void GameManager::CheckBossSoftlock() {
    Logger::Log("Softlock fix triggered");
    auto instance = (SDK::UPBGameInstance*)GameManager::Instance().GameInstance();
    auto currentBoss = instance->CurrentBoss;
    auto roomId = instance->pRoomManager->GetCurrentRoomId();

    if (currentBoss) {
        std::string boss = currentBoss->GetBossId().ToString();
        if (!boss.empty() && currentBoss->GetHitPoint() <= 0) {
            ThreadQueue::Instance().Enqueue([currentBoss]() { currentBoss->EndBossBattle(true); });
            currentBoss->EndBossBattle(true);
        }
        if (!GameManager::Instance().RoomIsBossRoom(roomId.ToString())) return;

        auto nextRoom = GameManager::Instance().GetNextRoomFromBossRoomId(roomId.ToString());
        if (nextRoom.empty()) return;

        std::wstring nextRoomWideName(nextRoom.begin(), nextRoom.end());
        auto nextRoomName = SDK::UKismetStringLibrary::Conv_StringToName(nextRoomWideName.c_str());

        std::string none = "None";
        std::wstring wideName(none.begin(), none.end());
        auto noneName = SDK::UKismetStringLibrary::Conv_StringToName(wideName.c_str());

        SDK::FLinearColor blackFade = {0.0f, 0.0f, 0.0f, 1.0f};
        instance->pRoomManager->Warp(nextRoomName, true, true, noneName, blackFade);
    }
}

void GameManager::ProcessNamePool() {
    for (UC::uint32 i = 0; i < SDK::UObject::GObjects->Num(); i++) {
        SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
        if (!obj) continue;

        std::string Name = obj->Name.ToString();
        UC::int32 Index = obj->Name.ComparisonIndex;

        NameLookup[Name] = Index;
    }
}

SDK::FName GameManager::FindName(std::string name) {
    auto it = NameLookup.find(name);
    if (it != NameLookup.end()) {
        ProcessNamePool();
        it = NameLookup.find(name);
    }

    if (it != NameLookup.end()) {
        SDK::FName foundName(it->second);
        return foundName;
    }

    Logger::Log("Item not found");
    return SDK::FName();
}

void GameManager::GivePlayerMaxStatItem(std::string& maxStat, bool shouldDisplay) {
    auto player = GameManager::Instance().Player();

    ThreadQueue::Instance().Enqueue([player, maxStat, shouldDisplay]() {
        std::wstring wideName = std::wstring(maxStat.begin(), maxStat.end());
        SDK::APBPlayerController* controller = (SDK::APBPlayerController*)GameManager::Instance().PlayerController();
        SDK::APBInterfaceHUDBP_C* hud = (SDK::APBInterfaceHUDBP_C*)controller->MyHUD;

        auto itemName = SDK::UKismetStringLibrary::Conv_StringToName(wideName.c_str());
        player->CharacterInventory->UseConsumable(itemName, false);
        if (shouldDisplay) {
            UC::FString displayString = FStringFromString(maxStat);
            hud->DisplayItemNameWindow(displayString, 1022);
        }
    });
}

void GameManager::GivePlayerCoin(SDK::int32 amount, bool shouldDisplay) {
    if (amount != 50 && amount != 100 && amount != 500 && amount != 1000 && amount != 2000) return;

    ThreadQueue::Instance().Enqueue([amount, shouldDisplay] {
        SDK::APBPlayerController* controller = (SDK::APBPlayerController*)GameManager::Instance().PlayerController();
        SDK::APBInterfaceHUDBP_C* hud = (SDK::APBInterfaceHUDBP_C*)controller->MyHUD;
        SDK::APB_Chr_Root_C* player = (SDK::APB_Chr_Root_C*)GameManager::Instance().Player();

        auto dropCoin = GameManager::Instance().CoinLookup[amount];
        auto coinIconId = player->CharacterInventory->GetCoinIcon(dropCoin);
        auto coinString = std::to_string(amount) + "G";

        if (shouldDisplay) {
            UC::FString displayString = FStringFromString(coinString);
            hud->DisplayItemNameWindow(displayString, coinIconId);
        }

        SDK::UPBGameInstance* instance = (SDK::UPBGameInstance*)GameManager::Instance().GameInstance();
        instance->AddTotalCoin(amount);
    });
}

void GameManager::SendInGameNotification(std::string notification, float iconId) {
    ThreadQueue::Instance().Enqueue([notification, iconId]() {
        SDK::APBPlayerController* controller = (SDK::APBPlayerController*)GameManager::Instance().PlayerController();
        SDK::APBInterfaceHUDBP_C* hud = (SDK::APBInterfaceHUDBP_C*)controller->MyHUD;
        SDK::APB_Chr_Root_C* player = (SDK::APB_Chr_Root_C*)GameManager::Instance().Player();

        UC::FString displayString = FStringFromString(notification);
        hud->DisplayItemNameWindow(displayString, iconId);
    });
}

std::optional<SDK::FPBItemCatalogData> GameManager::PlayerHasItem(
    const SDK::TArray<SDK::FPBItemCatalogData>& itemsArray, const std::string& itemName) {
    std::string lowerItemName = itemName;
    std::transform(lowerItemName.begin(), lowerItemName.end(), lowerItemName.begin(), ::tolower);

    for (auto& item : itemsArray) {
        std::string itemId = item.ID.ToString();
        std::transform(itemId.begin(), itemId.end(), itemId.begin(), ::tolower);
        if (itemId == lowerItemName) {
            Logger::Log("Player has item already");
            return item;
        }
    }
    return std::nullopt;
}

std::optional<SDK::FPBItemCatalogData> GameManager::CheckAllInventories(const std::string& itemName) {
    auto* player = static_cast<SDK::APB_Chr_Root_C*>(GameManager::Instance().Player());
    if (!player || !player->CharacterInventory) return std::nullopt;
    auto* inv = player->CharacterInventory;

    for (auto& arr : {inv->myWeapons, inv->myBullets, inv->myArmors, inv->myHeadGears, inv->myAccessories,
                      inv->myMufflers, inv->myConsumables, inv->myFoodstuffs, inv->myKeyItems, inv->myBooks,
                      inv->myTriggerShards, inv->myEffectiveShards, inv->myDirectionalShards, inv->myEnchantShards,
                      inv->myFamiliarShards, inv->mySkills}) {
        auto result = PlayerHasItem(arr, itemName);
        if (result.has_value()) return result;
    }

    return std::nullopt;
}

void GameManager::ApplyWaystoneSafety() {
    bool shopPolicyChanged = false;
    if (auto* itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster)) {
        for (const auto& pair : itemTable->RowMap) {
            if (pair.Key().ToString() != WAYSTONE_ITEM_ID) continue;
            auto* row = reinterpret_cast<SDK::FPBItemMasterData*>(pair.Value());
            if (!row) break;
            shopPolicyChanged = row->buyPrice != 0 || row->sellPrice != 0 ||
                                row->Producted.ToString() != "None";
            if (shopPolicyChanged) {
                row->buyPrice = 0;
                row->sellPrice = 0;
                row->Producted = FNameFromString("None");
            }
            break;
        }
    }

    auto* player = static_cast<SDK::APB_Chr_Root_C*>(Player());
    if (!player || !player->CharacterInventory) return;

    auto* inventory = player->CharacterInventory;
    bool inventoryPolicyChanged = false;
    auto normalizeWaystone = [&]() {
        for (auto& item : inventory->myConsumables) {
            if (item.ID.ToString() != WAYSTONE_ITEM_ID) continue;
            const bool needsNormalization = item.Num != WAYSTONE_QUANTITY || item.buyPrice != 0 ||
                                            item.sellPrice != 0 || item.ProductFlag.ToString() != "None";
            inventoryPolicyChanged = inventoryPolicyChanged || needsNormalization;
            if (needsNormalization) {
                item.Num = WAYSTONE_QUANTITY;
                item.buyPrice = 0;
                item.sellPrice = 0;
                item.ProductFlag = FNameFromString("None");
            }
            return true;
        }
        return false;
    };

    bool waystoneFound = normalizeWaystone();
    bool nativeGrantAccepted = false;
    if (!waystoneFound) {
        // A fresh save does not materialize zero-count consumables in myConsumables. Create the entry through the
        // native inventory path without displaying a pickup, then normalize its quantity and shop metadata.
        nativeGrantAccepted = inventory->GetItemWithDisplay(
            FNameFromString(std::string(WAYSTONE_ITEM_ID)), WAYSTONE_QUANTITY, false);
        waystoneFound = normalizeWaystone();
        inventoryPolicyChanged = inventoryPolicyChanged || waystoneFound;
    }

    if (!waystoneFound) {
        Logger::Log(LogLevel::File, "[AP] Could not create infinite Waystones; native grant accepted:",
                    nativeGrantAccepted);
    } else if (shopPolicyChanged || inventoryPolicyChanged) {
        Logger::Log(LogLevel::File, "[AP] Applied infinite Waystones; quantity:", WAYSTONE_QUANTITY,
                    "buying and selling disabled");
    }
}

bool GameManager::TryUseWaystone() {
    auto* player = static_cast<SDK::APB_Chr_Root_C*>(Player());
    if (!player || !player->CharacterInventory || player->Killed) return false;

    ApplyWaystoneSafety();
    return player->CharacterInventory->UseConsumable(FNameFromString(std::string(WAYSTONE_ITEM_ID)), false);
}

bool GameManager::UnlockEquipmentInShop(const std::string& itemName) {
    auto* player = static_cast<SDK::APB_Chr_Root_C*>(Player());
    if (!player || !player->CharacterInventory) return false;

    constexpr int flyingEdgeBuyPrice = 10000;
    constexpr int flyingEdgeSellPrice = 1000;
    auto unlockIn = [&](SDK::TArray<SDK::FPBItemCatalogData>& items) {
        for (auto& item : items) {
            if (item.ID.ToString() != itemName) continue;

            // Flying Edge is ordinary, consumable crafting equipment, but the base game
            // gives it a zero shop price because it was never intended to be renewable.
            if (itemName == "RemoteDart" && item.buyPrice <= 0) {
                item.buyPrice = flyingEdgeBuyPrice;
                item.sellPrice = flyingEdgeSellPrice;
            }
            if (item.buyPrice <= 0) return false;

            item.IsCrafted = true;
            return true;
        }
        return false;
    };

    auto* inventory = player->CharacterInventory;
    return unlockIn(inventory->myWeapons) || unlockIn(inventory->myArmors) ||
           unlockIn(inventory->myHeadGears) || unlockIn(inventory->myAccessories) ||
           unlockIn(inventory->myMufflers);
}

bool GameManager::ItemHasItemCategory(const std::string& itemName, SDK::ECarriedCatalog category) {
    SDK::UPBGameInstance* inst = (SDK::UPBGameInstance*)GameManager::Instance().GameInstance();
    auto itemFName = FNameFromString(itemName);
    auto player = GameManager::Instance().Player();
    auto inventory = player->CharacterInventory;

    SDK::FPBItemCatalogData itemData = SDK::FPBItemCatalogData();
    inventory->GetItemDataById(itemFName, &itemData);

    if (itemData.itemCategory == category) {
        return true;
    }

    return false;
}

bool GameManager::ItemHasItemCategories(const std::string& itemName,
                                        std::initializer_list<SDK::ECarriedCatalog> categories) {
    SDK::UPBGameInstance* inst = (SDK::UPBGameInstance*)GameManager::Instance().GameInstance();
    auto itemFName = FNameFromString(itemName);
    auto inventory = GameManager::Instance().Player()->CharacterInventory;

    SDK::FPBItemCatalogData itemData;
    inventory->GetItemDataById(itemFName, &itemData);

    for (auto cat : categories) {
        if (itemData.itemCategory == cat) return true;
    }
    return false;
}

void GameManager::GivePlayerItem(const std::string& name, bool shouldDisplay, int count,
                                 std::function<void(ItemGrantResult)> completion) {
    ThreadQueue::Instance().Enqueue([name, shouldDisplay, count, completion = std::move(completion)]() {
        auto& gameManager = GameManager::Instance();
        if (!gameManager.CanReceiveItems()) {
            if (completion) completion(ItemGrantResult::Rejected);
            return;
        }

        auto* inventory = gameManager.Player()->CharacterInventory;
        auto itemName = FNameFromString(name);
        const auto preparation = PreparePaidDlcCatalogRow(name);
        if (preparation != DlcOwnership::NotApplicable && preparation != DlcOwnership::Owned) {
            // Missing entitlement is permanent and must not block later deliveries; unavailable native state is
            // transient and remains retryable.
            if (preparation == DlcOwnership::NotOwned) {
                Logger::Log(LogLevel::File, "[AP] Paid-DLC item is unsupported by this installation:", name);
            }
            if (completion) {
                completion(preparation == DlcOwnership::NotOwned ? ItemGrantResult::Unsupported
                                                                : ItemGrantResult::Rejected);
            }
            return;
        }
        SDK::FPBItemCatalogData itemData{};
        inventory->GetItemDataById(itemName, &itemData);

        auto itemInInventory = gameManager.CheckAllInventories(name);
        const int previousCount = itemInInventory ? itemInInventory->Num : 0;
        const bool isShard = gameManager.IsPaidDlcShard(name) || IsShardCategory(itemData.itemCategory);
        const int previousGrade = isShard ? inventory->GetShardGradeWithPossession(itemName) : 0;
        constexpr int MAX_SHARD_GRADE = 9;
        if (isShard && previousGrade >= MAX_SHARD_GRADE) {
            auto iconId = inventory->GetItemIcon(itemName);
            gameManager.SendInGameNotification("Limit reached: " + itemData.Name.ToString(), iconId);
            Logger::Log(LogLevel::File, "[AP] Received shard already at native grade capacity:", name,
                        "grade:", previousGrade);
            if (completion) completion(ItemGrantResult::AtCapacity);
            return;
        }
        if (itemInInventory.has_value()) {
            Logger::Log("Player has", itemInInventory->Num, "of", name);
            if (!isShard && itemInInventory->Num >= itemInInventory->MaxNum) {
                auto iconId = inventory->GetItemIcon(itemInInventory->ID);
                gameManager.SendInGameNotification("Limit reached: " + itemData.Name.ToString(), iconId);
                if (completion) completion(ItemGrantResult::AtCapacity);
                return;
            }
        }

        bool nativeGrantAccepted = inventory->GetItemWithDisplay(itemName, count, shouldDisplay);
        auto itemAfterGrant = gameManager.CheckAllInventories(name);
        const int currentGrade = isShard ? inventory->GetShardGradeWithPossession(itemName) : 0;
        bool inventoryUpdated = isShard ? currentGrade > previousGrade
                                        : itemAfterGrant && itemAfterGrant->Num > previousCount;

        // Some special equipment (notably the IGA DLC Sword Whip) is present in ItemMaster and has a valid
        // protocol mapping, but rejects the ordinary display-grant route. Native pickups for those rows use
        // GetItemWrap instead. Only try that route after proving the first call did not change inventory so a
        // false return value from GetItemWithDisplay can never result in a duplicate award.
        if (!inventoryUpdated && name == "SwordWhip" && count > 0) {
            const bool wrappedGrantAccepted = inventory->GetItemWrap(itemName, gameManager.Player());
            itemAfterGrant = gameManager.CheckAllInventories(name);
            inventoryUpdated = itemAfterGrant && itemAfterGrant->Num > previousCount;
            nativeGrantAccepted = nativeGrantAccepted || wrappedGrantAccepted;
            Logger::Log(LogLevel::File, "[AP] Native wrapped-item grant fallback:", name,
                        "accepted:", wrappedGrantAccepted, "inventory updated:", inventoryUpdated);
        }
        if (nativeGrantAccepted && !inventoryUpdated) {
            Logger::Log(LogLevel::File, "[AP] Native item grant reported success without updating inventory:",
                        name, "previous count:", previousCount,
                        "current count:", itemAfterGrant ? itemAfterGrant->Num : 0,
                        "previous grade:", previousGrade, "current grade:", currentGrade);
        }
        if (nativeGrantAccepted && inventoryUpdated && gameManager.UnlockEquipmentInShop(name)) {
            Logger::Log(LogLevel::File, "[AP] Unlocked received equipment in shop:", name);
        }
        if (completion) {
            completion(nativeGrantAccepted && inventoryUpdated ? ItemGrantResult::Granted : ItemGrantResult::Rejected);
        }
    });
}

bool GameManager::CanKillPlayer() {
    auto* instance = static_cast<SDK::UPBGameInstance*>(GameInstance());
    auto* player = static_cast<SDK::APB_Chr_PlayerRoot_C*>(Player());
    auto* controller = static_cast<SDK::APBPlayerController*>(PlayerController());
    if (!instance || !player || !controller) return false;

    const auto gameModeType = instance->GetGameModeType();
    if (gameModeType != SDK::EPBGameModeType::Normal &&
        gameModeType != SDK::EPBGameModeType::RandomizerMode &&
        gameModeType != SDK::EPBGameModeType::SpeedRunMode) {
        return false;
    }
    if (!SDK::UKismetSystemLibrary::IsValid(instance->LoadingManagerInstance)) return false;
    if (instance->LoadingManagerInstance->IsLoadingScreenVisible()) return false;
    if (!IsPlayerLoadedInGame() || player->Killed || player->CurrentryWarpingByWarpRoom) return false;
    if (!SDK::UKismetSystemLibrary::IsValid(controller->MyHUD)) return false;
    return true;
}

void GameManager::KillPlayer() {
    auto player = static_cast<SDK::APB_Chr_PlayerRoot_C*>(GameManager::Instance().Player());
    ThreadQueue::Instance().Enqueue([player] { player->Kill(); });
}

void GameManager::GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute attribute, float multiplier) {
    auto player = static_cast<SDK::APB_Chr_PlayerRoot_C*>(GameManager::Instance().Player());

    ThreadQueue::Instance().Enqueue([this, player, attribute, multiplier]() {
        player->CharacterInventory->SetSpecialAttribute(attribute, multiplier);
    });
}
