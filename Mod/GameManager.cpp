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
#include <format>
#include <utility>

#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "Logger.h"
#include "ThreadQueue.h"
#include "Utils.h"

#define PLAYER_NAME "Chr_P0000_C_0"

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

bool GameManager::PopulateDisplayToItemIdTable() {
    auto itemTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ItemMaster);
    auto inventory = GameManager::Instance().Player()->CharacterInventory;
    auto text = SDK::UKismetTextLibrary::Conv_StringToText(FStringFromString("ahhhhhh"));
    // SDK::FPBItemCatalogData* realItemData = (SDK::FPBItemCatalogData*)pair.Value();

    for (auto& pair : itemTable->RowMap) {
        pair.Value();
        std::string itemId = pair.Key().ToString();
        auto itemName = FNameFromString(itemId);
        SDK::FPBItemCatalogData itemData = SDK::FPBItemCatalogData();

        inventory->GetItemDataById(itemName, &itemData);
        if (itemData.Name.ToString().empty()) continue;
        if (itemData.Name.ToString().starts_with("AP_")) continue;
        if (DisplayNameToItemId.count(itemData.Name.ToString()) >= 1) continue;

        std::string displayName = itemData.Name.ToString();
        DisplayNameToItemId[displayName] = itemId;
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
    ThreadQueue::Instance().Enqueue([this] { GameManager::Instance().PopulateDisplayToItemIdTable(); });
    // GameManager::Instance().PopulateDisplayToItemIdTable();
    Logger::Log(DisplayNameToItemId["Knife"]);
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
        SDK::FPBItemCatalogData itemData{};
        inventory->GetItemDataById(itemName, &itemData);

        auto itemInInventory = gameManager.CheckAllInventories(name);
        const int previousCount = itemInInventory ? itemInInventory->Num : 0;
        if (itemInInventory.has_value()) {
            Logger::Log("Player has", itemInInventory->Num, "of", name);
            if (itemInInventory->Num >= itemInInventory->MaxNum) {
                auto iconId = inventory->GetItemIcon(itemInInventory->ID);
                gameManager.SendInGameNotification("Limit reached: " + itemData.Name.ToString(), iconId);
                if (completion) completion(ItemGrantResult::AtCapacity);
                return;
            }
        }

        const bool nativeGrantAccepted = inventory->GetItemWithDisplay(itemName, count, shouldDisplay);
        const auto itemAfterGrant = gameManager.CheckAllInventories(name);
        const bool inventoryUpdated = itemAfterGrant && itemAfterGrant->Num > previousCount;
        if (nativeGrantAccepted && !inventoryUpdated) {
            Logger::Log(LogLevel::File, "[AP] Native item grant reported success without updating inventory:",
                        name, "previous count:", previousCount,
                        "current count:", itemAfterGrant ? itemAfterGrant->Num : 0);
        }
        if (completion) {
            completion(nativeGrantAccepted && inventoryUpdated ? ItemGrantResult::Granted : ItemGrantResult::Rejected);
        }
    });
}

bool GameManager::CanKillPlayer() {
    auto instance = (SDK::UPBGameInstance*)(GameInstance());
    auto player = (SDK::APB_Chr_PlayerRoot_C*)(Player());
    auto controller = (SDK::APBPlayerController*)(PlayerController());
    auto hud = controller->MyHUD;
    ThreadQueue::Instance().Enqueue([this, instance, player, hud]() {
        auto gamemodeType = instance->GetGameModeType();
        if (gamemodeType == SDK::EPBGameModeType::Normal || gamemodeType == SDK::EPBGameModeType::RandomizerMode ||
            gamemodeType == SDK::EPBGameModeType::SpeedRunMode)
            return false;
        if (!SDK::UKismetSystemLibrary::IsValid(instance->LoadingManagerInstance)) return false;
        if (instance->LoadingManagerInstance->IsLoadingScreenVisible()) return false;
        if (!IsPlayerLoadedInGame()) return false;
        if (player->Killed) return false;
        if (player->CurrentryWarpingByWarpRoom) return false;
        if (!SDK::UKismetSystemLibrary::IsValid(hud)) return false;
    });
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
