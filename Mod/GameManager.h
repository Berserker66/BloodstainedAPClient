#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN64
#define WINVER 0x0600
#define _WIN32_WINNT 0x0600
#endif

#include <Windows.h>

#include <ProjectBlood_structs.hpp>
#include <UnrealContainers.hpp>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "CoreUObject_classes.hpp"
#include "Engine_classes.hpp"
#include "PB_Chr_Root_classes.hpp"
#include "ProjectBlood_classes.hpp"

enum class ItemGrantResult { Granted, AtCapacity, Unsupported, Rejected };
enum class DlcOwnership { NotApplicable, Owned, NotOwned, NotReady };

class GameManager {
   public:
    static GameManager& Instance();

    static constexpr float HP_INCREASE = 20.0f;
    static constexpr float MP_INCREASE = 10.0f;
    static constexpr int32_t CAPACITY_INCREASE = 5.0;

    static constexpr float DEFAULT_HP = 120.0f;
    static constexpr float DEFAULT_MP = 80.f;
    static constexpr int32_t DEFAULT_CAPACITY = 0;

    // Instances
    SDK::UWorld* World() const { return SDK::UWorld::GetWorld(); };
    SDK::UEngine* Engine() const { return SDK::UEngine::GetEngine(); };
    // Startup polling runs on the plugin worker thread. Do not use
    // UGameplayStatics::GetGameInstance here: the generated SDK wrapper invokes
    // UObject::ProcessEvent, which is unsafe while the engine and hook chain are
    // still being initialized. UWorld owns the same pointer directly.
    SDK::UGameInstance* GameInstance() const {
        auto* world = GameManager::Instance().World();
        return world ? world->OwningGameInstance : nullptr;
    };
    SDK::APBPlayerController* PlayerController() const {
        auto* gameInstance = GameManager::Instance().GameInstance();
        if (!gameInstance || gameInstance->LocalPlayers.Num() <= 0 || !gameInstance->LocalPlayers[0]) return nullptr;
        return static_cast<SDK::APBPlayerController*>(gameInstance->LocalPlayers[0]->PlayerController);
    };
    SDK::APB_Chr_Root_C* Player() const {
        auto* playerController = PlayerController();
        return playerController ? static_cast<SDK::APB_Chr_Root_C*>(playerController->Pawn) : nullptr;
    };
    SDK::UPBRoomManager* RoomManager() const {
        auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
        return gameInstance ? gameInstance->pRoomManager : nullptr;
    };

    bool Init();
    bool PostInit();
    bool IsInitialized();
    bool IsInstanceValid(SDK::UObject* object, const char* str);
    bool PopulateDisplayToItemIdTable();
    bool CanReceiveItems();
    DlcOwnership GetDlcOwnership(std::string_view dlcKey) const;
    DlcOwnership GetPaidDlcItemOwnership(std::string_view nativeItemId) const;
    bool IsPaidDlcShard(std::string_view nativeItemId) const;
    bool PrimeOwnedPaidDlcCatalogRows();
    void SuppressRandomizedDlcCatalogRowsForNewGame();
    void RestoreRandomizedDlcCatalogRowsAfterNewGameInit();

    void GivePlayerItem(const std::string& name, bool shouldDisplay = true, int count = 1,
                        std::function<void(ItemGrantResult)> completion = {});
    void GivePlayerCoin(SDK::int32 amount, bool shouldDisplay = true);
    void GivePlayerMaxStatItem(std::string& maxStat, bool shouldDisplay = true);
    void GivePlayerStatusMultiplier(SDK::EPBEquipSpecialAttribute attribute, float multiplier);
    std::optional<SDK::FPBItemCatalogData> PlayerHasItem(const SDK::TArray<SDK::FPBItemCatalogData>& itemsArray,
                                                         const std::string& itemName);
    std::optional<SDK::FPBItemCatalogData> CheckAllInventories(const std::string& itemName);
    bool UnlockEquipmentInShop(const std::string& itemName);
    void ApplyWaystoneSafety();
    bool TryUseWaystone();

    bool CanKillPlayer();
    void KillPlayer();
    void SendInGameNotification(std::string notification, float iconId);
    bool IsPlayerDead() const { return isPlayerDead; };
    void PlayerDied() { isPlayerDead = true; };
    void PlayerAlive() { isPlayerDead = false; };
    void CheckBossSoftlock();
    bool RoomIsBossRoom(const std::string& roomId) const { return bossRooms.count(roomId); };
    std::optional<std::string> GetIdFromDisplayName(const std::string& itemId);
    void NotifyOnNewObject(const SDK::UObject* obj, SDK::UFunction* func, void* params);
    void ProcessEvent(const SDK::UObject* obj, SDK::UFunction* func, void* params);
    SDK::FName FindName(std::string name);
    bool ItemHasItemCategory(const std::string& itemName, SDK::ECarriedCatalog category);
    bool ItemHasItemCategories(const std::string& itemName, std::initializer_list<SDK::ECarriedCatalog> categories);
    bool IsPlayerLoadedInGame();
    SDK::FPBItemCatalogData* PlayerHasShard(SDK::FName shardId);
    std::unordered_map<std::string, UC::int32> NameLookup;
    std::unordered_map<UC::int32, SDK::EDropCoin> CoinLookup = {{50, SDK::EDropCoin::D50},
                                                                {100, SDK::EDropCoin::D100},
                                                                {500, SDK::EDropCoin::D500},
                                                                {1000, SDK::EDropCoin::D1000},
                                                                {2000, SDK::EDropCoin::D2000}};

    std::string GetBossRoomIdFromBossName(const std::string& bossName) const { return BossNameToRoomId.at(bossName); };
    std::string GetNextRoomFromBossRoomId(const std::string& roomId) const {
        auto it = BossRoomIdToNextRoom.find(roomId);
        return it != BossRoomIdToNextRoom.end() ? it->second : "";
    }
    bool IsRoomBossRoom(const std::string& roomId) { return bossRooms.contains(roomId); };
    std::unordered_set<std::string> GetBossRooms() const { return bossRooms; };
    std::unordered_set<std::string> GetBossNames() const { return bossNames; };

   private:
    GameManager() = default;
    ~GameManager() = default;
    GameManager(const GameManager&) = delete;
    GameManager& operator=(const GameManager&) = delete;

    void ProcessNamePool();
    bool playerLoaded;
    bool initCompleted;
    bool postInitCompleted;
    bool isPlayerDead = false;
    bool paidDlcCatalogPrimed_ = false;
    bool paidDlcCatalogPendingLogged_ = false;
    bool suppressRandomizedDlcCatalogForNewGame_ = false;

    // m17RVA_008
    const std::unordered_set<std::string> bossRooms = {"m01SIP_000", "m01SIP_022", "m09TRN_002",
                                                       "m07LIB_011", "m08TWR_019", "m05SAN_023",
                                                       "m18ICE_018", "m05SAN_012", "m17RVA_008"
    };

    const std::unordered_set<std::string> bossNames = {"Vepar",     "Craftwork", "Andrealphus", "Twin Dragons",
                                                       "Bloodless", "Train",     "Gremory",     "Orobas"
    };

    const std::unordered_map<std::string, std::string> BossNameToRoomId = {
        {"Vepar", "m01SIP_022"},        {"Craftwork", "m05SAN_012"}, {"Andrealphus", "m07LIB_011"},
        {"Twin Dragons", "m08TWR_019"}, {"Bloodless", "m05SAN_023"}, {"Train", "m09TRN_002"},
        {"Gremory", "m18ICE_018"},     {"Orobas", "m17RVA_008"}
    };

    // Bosses that softlock need a map to the next room
    const std::unordered_map<std::string, std::string> BossRoomIdToNextRoom = {
        {"m01SIP_022", "m02VIL_000"},
        {"m09TRN_002", "m09TRN_003"},
    };

    std::unordered_map<std::string, std::string> DisplayNameToItemId;
};
