#include "QualityOfLife.h"

#include <ProjectBlood_classes.hpp>
#include <ProjectBlood_parameters.hpp>

#include <limits>
#include <string>
#include <unordered_map>

#include "GameManager.h"
#include "Logger.h"
#include "QualityOfLifeLogic.h"
#include "Utils.h"

namespace {

constexpr const char* AUTO_SELL_WASTED_SHARDS_SAVE_KEY = "AP_QoL_AutoSellWastedShards";

struct PendingShardSale {
    SDK::FName shardId;
    SDK::int32 previousGrade = 0;
    SDK::int32 excessQuantity = 0;
    SDK::int32 basePrice = 0;
    SDK::int32 adjustedPrice = 0;
};

thread_local std::unordered_map<void*, PendingShardSale> pendingShardSales;

void AwardPendingShardSale(const PendingShardSale& sale) {
    if (sale.excessQuantity > std::numeric_limits<SDK::int32>::max() / sale.adjustedPrice) {
        Logger::Log(LogLevel::File, "[QoL] Skipped excess shard sale due to price overflow:",
                    sale.shardId.ToString(), "quantity:", sale.excessQuantity, "price:", sale.adjustedPrice);
        return;
    }

    auto* gameInstance = static_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
    if (!gameInstance) {
        Logger::Log(LogLevel::File, "[QoL] Skipped excess shard sale without a game instance:",
                    sale.shardId.ToString());
        return;
    }

    const SDK::int32 awardedGold = sale.excessQuantity * sale.adjustedPrice;
    gameInstance->AddTotalCoin(awardedGold);
    Logger::Log(LogLevel::File, "[QoL] Auto-sold excess shard:", sale.shardId.ToString(), "previous grade:",
                sale.previousGrade, "quantity:", sale.excessQuantity, "base price:", sale.basePrice,
                "adjusted price:", sale.adjustedPrice, "awarded:", awardedGold);
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

void QualityOfLife::ProcessEventBefore(SDK::UObject* obj, SDK::UFunction* func, void* params) {
    if (!autoSellWastedShards_.load() || !obj || !func) {
        return;
    }

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

    auto* player = GameManager::Instance().Player();
    if (!player || !player->CharacterInventory) return;
    auto* inventory = player->CharacterInventory;
    const SDK::int32 previousGrade = inventory->GetShardGradeWithPossession(shard->ShardId);
    const SDK::int32 excessQuantity = bloodstained::qol::ExcessShardQuantity(previousGrade, 1);
    if (excessQuantity <= 0) return;

    SDK::FPBItemCatalogData itemData{};
    inventory->GetItemDataById(shard->ShardId, &itemData);
    const SDK::int32 basePrice = itemData.sellPrice;
    const SDK::int32 adjustedPrice = SDK::UPBShopManager::GetAdjustedSellPrice(basePrice);
    if (basePrice <= 0 || adjustedPrice <= 0) {
        Logger::Log(LogLevel::File, "[QoL] Skipped excess shard with invalid sell price:",
                    shard->ShardId.ToString(), "base:", basePrice, "adjusted:", adjustedPrice);
        return;
    }

    pendingShardSales[obj] =
        PendingShardSale{shard->ShardId, previousGrade, excessQuantity, basePrice, adjustedPrice};
}
