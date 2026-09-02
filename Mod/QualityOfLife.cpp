#include "QualityOfLife.h"

#include <chrono>
#include <ProjectBlood_classes.hpp>
#include <ProjectBlood_parameters.hpp>

#include <array>
#include <limits>
#include <string>
#include <unordered_map>

#include "GameManager.h"
#include "Logger.h"
#include "QualityOfLifeLogic.h"
#include "Utils.h"

namespace {

constexpr const char* AUTO_SELL_WASTED_SHARDS_SAVE_KEY = "AP_QoL_AutoSellWastedShards";

struct BountyMarkerReplacement {
    const char* questId;
    const char* enemyId;
    const char* invalidRoom;
    const char* validRoom;
};

// Audited against the cooked enemy actors in the base, Normal, and Hard room layers.
constexpr std::array BOUNTY_MARKER_REPLACEMENTS = {
    BountyMarkerReplacement{"Quest_Enemy01", "N3003", "m02VIL_002", "m03ENT_001"},
    BountyMarkerReplacement{"Quest_Enemy05", "N3011", "m03ENT_01", "m03ENT_011"},
    BountyMarkerReplacement{"Quest_Enemy08", "N3111", "m05SAN_021", "m03ENT_024"},
    BountyMarkerReplacement{"Quest_Enemy11", "N3110", "m06KNG_015", "m06KNG_000"},
    BountyMarkerReplacement{"Quest_Enemy14", "N2010", "m05SAN_001", "m12SND_001"},
    BountyMarkerReplacement{"Quest_Enemy14", "N2010", "m05SAN_012", "m12SND_012"},
    BountyMarkerReplacement{"Quest_Enemy14", "N2010", "m05SAN_013", "m12SND_013"},
};

constexpr const char* HARD_ONLY_MARKER_QUEST = "Quest_Enemy13";
constexpr const char* HARD_ONLY_MARKER_ENEMY = "N3056";
constexpr const char* HARD_ONLY_MARKER_ROOM = "m11UGD_018";

struct PendingShardSale {
    SDK::FName shardId;
    SDK::int32 previousGrade = 0;
    SDK::int32 excessQuantity = 0;
    SDK::int32 basePrice = 0;
    SDK::int32 adjustedPrice = 0;
    bool repeatedApSale = false;
    std::string displayName;
    float iconId = 1023.0f;
};

thread_local std::unordered_map<void*, PendingShardSale> pendingShardSales;

void AwardPendingShardSale(const PendingShardSale& sale) {
    if (sale.adjustedPrice <= 0) {
        Logger::Log(LogLevel::File,
                    sale.repeatedApSale
                        ? "[QoL] Discarded repeated AP shard without gold due to invalid sell price:"
                        : "[QoL] Skipped excess shard sale due to invalid sell price:",
                    sale.shardId.ToString(), "base:", sale.basePrice, "adjusted:", sale.adjustedPrice);
        return;
    }

    if (sale.excessQuantity > std::numeric_limits<SDK::int32>::max() / sale.adjustedPrice) {
        Logger::Log(LogLevel::File,
                    sale.repeatedApSale
                        ? "[QoL] Discarded repeated AP shard without gold due to price overflow:"
                        : "[QoL] Skipped excess shard sale due to price overflow:",
                    sale.shardId.ToString(), "quantity:", sale.excessQuantity, "price:", sale.adjustedPrice);
        return;
    }

    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (!gameInstance) {
        Logger::Log(LogLevel::File,
                    sale.repeatedApSale
                        ? "[QoL] Discarded repeated AP shard without gold because no game instance was available:"
                        : "[QoL] Skipped excess shard sale because no game instance was available:",
                    sale.shardId.ToString());
        return;
    }

    const SDK::int32 awardedGold = sale.excessQuantity * sale.adjustedPrice;
    gameInstance->AddTotalCoin(awardedGold);
    const std::string shardName = sale.displayName.empty() ? sale.shardId.ToString() : sale.displayName;
    GameManager::Instance().SendInGameNotification(
        "Shard " + shardName + " was sold for " + std::to_string(awardedGold) + "G", sale.iconId);
    Logger::Log(LogLevel::File,
                sale.repeatedApSale ? "[QoL] Auto-sold repeated AP shard at spawn:"
                                    : "[QoL] Auto-sold excess shard:",
                sale.shardId.ToString(), "previous grade:", sale.previousGrade, "quantity:",
                sale.excessQuantity, "base price:", sale.basePrice, "adjusted price:", sale.adjustedPrice,
                "awarded:", awardedGold);
}

}  // namespace

QualityOfLife& QualityOfLife::Instance() {
    static QualityOfLife instance;
    return instance;
}

void QualityOfLife::LoadSettings() {
    SDK::int32 savedValue = 0;
    bool hasSavedValue = false;
    SDK::UPBGameInstance::GetSavedValue(FNameFromString(AUTO_SELL_WASTED_SHARDS_SAVE_KEY), &savedValue,
                                        &hasSavedValue);
    const bool enabled = hasSavedValue && savedValue != 0;
    autoSellWastedShards_.store(enabled);
    Logger::Log(LogLevel::File, "[QoL] Loaded auto-sell wasted shards:", enabled);
}

void QualityOfLife::SetAutoSellWastedShardsEnabled(bool enabled) {
    autoSellWastedShards_.store(enabled);
    SDK::UPBGameInstance::SetSavedValue(FNameFromString(AUTO_SELL_WASTED_SHARDS_SAVE_KEY), enabled ? 1 : 0);
    Logger::Log(LogLevel::File, "[QoL] Set auto-sell wasted shards:", enabled);
}

void QualityOfLife::AcceptAvailableBountyHunts(bool logWhenNoChanges) {
    auto* questManager = SDK::UPBQuestManager::GetQuestManager();
    auto* questTable = questManager ? questManager->GetQuestTable() : nullptr;
    if (!questManager || !questTable) {
        if (logWhenNoChanges) {
            Logger::Log(LogLevel::File,
                        "[QoL] Could not auto-accept bounty hunts because quest data is unavailable");
        }
        return;
    }

    SDK::int32 acceptedCount = 0;
    for (const auto& row : questTable->RowMap) {
        const SDK::FName questId = row.Key();
        const std::string questName = questId.ToString();
        if (!questName.starts_with("Quest_Enemy")) continue;

        const auto* questData = reinterpret_cast<const SDK::FPBQuestMasterData*>(row.Value());
        if (!questData || questData->QuestType != SDK::EQuestType::Enemy) continue;
        if (questManager->IsAccepted(questId) || questManager->IsDone(questId)) continue;
        if (!questManager->IsAcceptable(questId)) continue;

        questManager->StartAccept(questId);
        acceptedCount++;
    }

    if (acceptedCount > 0 || logWhenNoChanges) {
        Logger::Log(LogLevel::File, "[QoL] Auto-accepted available bounty hunts:", acceptedCount);
    }

    // Accepted quests persist their own copy of EnemyLocations. Repair every invalid
    // shipped marker in place so this covers both old saves and quests accepted above.
    SDK::int32 repairedMarkerCount = 0;
    for (auto& accepted : questManager->Accepted) {
        const std::string questId = accepted.QuestID.ToString();
        const std::string enemyId = accepted.EnemyID01.ToString();

        for (const auto& replacement : BOUNTY_MARKER_REPLACEMENTS) {
            if (questId != replacement.questId || enemyId != replacement.enemyId) continue;

            SDK::int32 invalidIndex = -1;
            bool alreadyHasValidRoom = false;
            for (SDK::int32 index = 0; index < accepted.EnemyLocations.Num(); ++index) {
                const std::string room = accepted.EnemyLocations[index].ToString();
                if (room == replacement.invalidRoom) invalidIndex = index;
                if (room == replacement.validRoom) alreadyHasValidRoom = true;
            }
            if (invalidIndex < 0) continue;

            if (alreadyHasValidRoom) {
                accepted.EnemyLocations.Remove(invalidIndex);
            } else {
                accepted.EnemyLocations[invalidIndex] = FNameFromString(replacement.validRoom);
            }
            repairedMarkerCount++;
        }

        // This room contains Archdemons only in the Hard overlay. Keeping the marker
        // produces a false scroll on Normal, so omit it rather than advertise a
        // difficulty-dependent target.
        if (questId == HARD_ONLY_MARKER_QUEST && enemyId == HARD_ONLY_MARKER_ENEMY) {
            for (SDK::int32 index = accepted.EnemyLocations.Num() - 1; index >= 0; --index) {
                if (accepted.EnemyLocations[index].ToString() != HARD_ONLY_MARKER_ROOM) continue;
                accepted.EnemyLocations.Remove(index);
                repairedMarkerCount++;
            }
        }
    }

    if (repairedMarkerCount > 0) {
        Logger::Log(LogLevel::File, "[QoL] Repaired invalid bounty hunt map markers:",
                    repairedMarkerCount);
    }
}

void QualityOfLife::TickAutoAcceptBountyHunts() {
    using namespace std::chrono_literals;
    static auto nextScan = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now < nextScan) return;
    nextScan = now + 1s;
    AcceptAvailableBountyHunts(false);
}

void QualityOfLife::SellRepeatedShardNow(const SDK::FName& vanillaShardId) {
    auto* player = GameManager::Instance().Player();
    if (!player || !player->CharacterInventory) {
        Logger::Log(LogLevel::File,
                    "[QoL] Discarded repeated AP shard without gold because inventory is unavailable:",
                    vanillaShardId.ToString());
        return;
    }

    auto* inventory = player->CharacterInventory;
    SDK::FPBItemCatalogData itemData{};
    inventory->GetItemDataById(vanillaShardId, &itemData);
    PendingShardSale sale{vanillaShardId,
                          inventory->GetShardGradeWithPossession(vanillaShardId),
                          bloodstained::qol::ShardSaleQuantity(0, 1, true),
                          itemData.sellPrice,
                          SDK::UPBShopManager::GetAdjustedSellPrice(itemData.sellPrice),
                          true};
    sale.displayName = itemData.Name.ToString();
    sale.iconId = inventory->GetItemIcon(vanillaShardId);
    AwardPendingShardSale(sale);
}

void QualityOfLife::ProcessEventBefore(SDK::UObject* obj, SDK::UFunction* func, void* params) {
    if (!obj || !func) return;

    const std::string functionName = func->Name.GetRawString();
    if (!obj->Class || obj->Class->Name.ToString() != "PurpleShard_C") return;

    auto* shard = static_cast<SDK::AShardBase*>(obj);
    if (functionName == "ReceiveEndPlay" || functionName == "ReceiveDestroyed") {
        pendingShardSales.erase(obj);
        return;
    }

    if (params && functionName == "OnReceiveEvent") {
        const auto* eventParams = static_cast<SDK::Params::PBInterface_EventListener_OnReceiveEvent*>(params);
        if (eventParams->eventKey.ToString() != "OnGetShardEnd") return;

        auto pending = pendingShardSales.find(obj);
        if (pending == pendingShardSales.end()) return;
        const PendingShardSale sale = pending->second;
        pendingShardSales.erase(pending);
        AwardPendingShardSale(sale);
        return;
    }

    if (functionName != "ReceiveBeginPlay") return;
    if (!autoSellWastedShards_.load()) return;

    auto* player = GameManager::Instance().Player();
    if (!player || !player->CharacterInventory) return;
    auto* inventory = player->CharacterInventory;
    const SDK::FName saleShardId = shard->ShardId;
    const SDK::int32 previousGrade = inventory->GetShardGradeWithPossession(saleShardId);
    const SDK::int32 excessQuantity = bloodstained::qol::ShardSaleQuantity(previousGrade, 1, false);
    if (excessQuantity <= 0) return;

    SDK::FPBItemCatalogData itemData{};
    inventory->GetItemDataById(saleShardId, &itemData);
    const std::string displayName = itemData.Name.ToString();
    const float iconId = inventory->GetItemIcon(saleShardId);
    const SDK::int32 basePrice = itemData.sellPrice;
    const SDK::int32 adjustedPrice = SDK::UPBShopManager::GetAdjustedSellPrice(basePrice);
    if (basePrice <= 0 || adjustedPrice <= 0) {
        Logger::Log(LogLevel::File, "[QoL] Skipped excess shard with invalid sell price:",
                    saleShardId.ToString(), "base:", basePrice, "adjusted:", adjustedPrice);
        return;
    }

    pendingShardSales[obj] = PendingShardSale{saleShardId, previousGrade, excessQuantity, basePrice,
                                               adjustedPrice, false};
    pendingShardSales[obj].displayName = displayName;
    pendingShardSales[obj].iconId = iconId;
}
