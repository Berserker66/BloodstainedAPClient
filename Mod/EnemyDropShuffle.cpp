#include "EnemyDropShuffle.h"

#include <Engine_classes.hpp>
#include <Engine_parameters.hpp>
#include <ProjectBlood_classes.hpp>

#include <string>
#include <unordered_map>

#include "EnemyDropShuffleLogic.h"
#include "Logger.h"
#include "Utils.h"

namespace {

SDK::UDataTable* originalDropTable = nullptr;
std::unordered_map<std::string, SDK::FPBDropRateMasterData> originalRows;

SDK::int32 RandomIntegerFromStream(SDK::FRandomStream& stream, SDK::int32 maxExclusive) {
    static SDK::UFunction* function = nullptr;
    if (!function) {
        function = SDK::UKismetMathLibrary::StaticClass()->GetFunction(
            "KismetMathLibrary", "RandomIntegerFromStream");
    }

    SDK::Params::KismetMathLibrary_RandomIntegerFromStream params{};
    params.Max = maxExclusive;
    params.Stream = stream;

    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    SDK::UKismetMathLibrary::GetDefaultObj()->ProcessEvent(function, &params);
    function->FunctionFlags = flags;

    // The generated SDK wrapper omits this copy-back even though FRandomStream
    // advances its mutable Seed through the const-reference UFunction argument.
    stream = params.Stream;
    return params.ReturnValue;
}

SDK::FPBDropRateMasterData* FindDropRow(SDK::UDataTable* table, const std::string& rowName) {
    if (!table) return nullptr;
    for (const auto& pair : table->RowMap) {
        if (pair.Key().ToString() == rowName) {
            return reinterpret_cast<SDK::FPBDropRateMasterData*>(pair.Value());
        }
    }
    return nullptr;
}

void ClearOrdinaryDrops(SDK::FPBDropRateMasterData& row) {
    const SDK::FName none = FNameFromString("None");
    row.RareItemId = none;
    row.RareItemQuantity = 0;
    row.RareItemRate = 0.0f;
    row.CommonItemId = none;
    row.CommonItemQuantity = 0;
    row.CommonRate = 0.0f;
    row.RareIngredientId = none;
    row.RareIngredientQuantity = 0;
    row.RareIngredientRate = 0.0f;
    row.CommonIngredientId = none;
    row.CommonIngredientQuantity = 0;
    row.CommonIngredientRate = 0.0f;
}

void SetSlot(SDK::FPBDropRateMasterData& row, std::size_t slot, std::string_view item, float rate) {
    const SDK::FName itemId = FNameFromString(std::string(item));
    switch (slot) {
        case 0:
            row.RareItemId = itemId;
            row.RareItemQuantity = 1;
            row.RareItemRate = rate;
            break;
        case 1:
            row.CommonItemId = itemId;
            row.CommonItemQuantity = 1;
            row.CommonRate = rate;
            break;
        case 2:
            row.RareIngredientId = itemId;
            row.RareIngredientQuantity = 1;
            row.RareIngredientRate = rate;
            break;
        case 3:
            row.CommonIngredientId = itemId;
            row.CommonIngredientQuantity = 1;
            row.CommonIngredientRate = rate;
            break;
    }
}

void CopyOrdinaryDrops(const SDK::FPBDropRateMasterData& source, SDK::FPBDropRateMasterData& target) {
    target.RareItemId = source.RareItemId;
    target.RareItemQuantity = source.RareItemQuantity;
    target.RareItemRate = source.RareItemRate;
    target.CommonItemId = source.CommonItemId;
    target.CommonItemQuantity = source.CommonItemQuantity;
    target.CommonRate = source.CommonRate;
    target.RareIngredientId = source.RareIngredientId;
    target.RareIngredientQuantity = source.RareIngredientQuantity;
    target.RareIngredientRate = source.RareIngredientRate;
    target.CommonIngredientId = source.CommonIngredientId;
    target.CommonIngredientQuantity = source.CommonIngredientQuantity;
    target.CommonIngredientRate = source.CommonIngredientRate;
}

void RestoreOriginalRows(SDK::UDataTable* table) {
    if (!table || table != originalDropTable) return;
    for (const auto& [rowName, original] : originalRows) {
        auto* row = FindDropRow(table, rowName);
        if (row) CopyOrdinaryDrops(original, *row);
    }
}

void PrepareOriginalRows(
    SDK::UDataTable* table,
    const std::unordered_map<std::string, SDK::FPBDropRateMasterData*>& primaryRows) {
    if (table == originalDropTable) {
        RestoreOriginalRows(table);
        return;
    }

    originalDropTable = table;
    originalRows.clear();
    for (const auto& pair : table->RowMap) {
        const std::string rowName = pair.Key().ToString();
        const std::size_t separator = rowName.find('_');
        if (separator == std::string::npos ||
            !primaryRows.contains(rowName.substr(0, separator))) {
            continue;
        }
        auto* row = reinterpret_cast<SDK::FPBDropRateMasterData*>(pair.Value());
        if (row) originalRows.emplace(rowName, *row);
    }
}

}  // namespace

void EnemyDropShuffle::Reset() {
    auto* dropManager = SDK::UPBDropManager::GetDropManager();
    if (!dropManager) return;
    RestoreOriginalRows(dropManager->DropTable);
}

bool EnemyDropShuffle::Apply(std::uint32_t seed, int version) {
    if (version != bloodstained::enemy_drop_shuffle::VERSION) {
        Logger::Log(LogLevel::File, "[DropShuffle] Unsupported algorithm version:", version);
        return false;
    }

    auto* dropManager = SDK::UPBDropManager::GetDropManager();
    if (!dropManager || !dropManager->DropTable) {
        Logger::Log(LogLevel::File, "[DropShuffle] Drop table is not ready");
        return false;
    }

    SDK::FRandomStream stream =
        SDK::UKismetMathLibrary::MakeRandomStream(static_cast<SDK::int32>(seed));
    auto result = bloodstained::enemy_drop_shuffle::Generate(
        [&stream](std::uint32_t maxExclusive) -> std::uint32_t {
            return static_cast<std::uint32_t>(
                RandomIntegerFromStream(stream, static_cast<SDK::int32>(maxExclusive)));
        });
    if (!result.coversVanillaDrops) {
        Logger::Log(LogLevel::File, "[DropShuffle] Coverage validation failed; table left unchanged");
        return false;
    }

    std::unordered_map<std::string, SDK::FPBDropRateMasterData*> primaryRows;
    for (const auto& assignment : result.enemies) {
        const std::string enemy(assignment.enemy);
        auto* row = FindDropRow(dropManager->DropTable, enemy + "_Shard");
        if (!row) {
            Logger::Log(LogLevel::File, "[DropShuffle] Missing required drop row:", enemy + "_Shard");
            return false;
        }
        primaryRows.emplace(enemy, row);
    }
    PrepareOriginalRows(dropManager->DropTable, primaryRows);

    for (const auto& assignment : result.enemies) {
        auto* row = primaryRows.at(std::string(assignment.enemy));
        ClearOrdinaryDrops(*row);
        const float rate = assignment.enemy == "N3090" || assignment.enemy == "N3099"
                               ? bloodstained::enemy_drop_shuffle::DROP_RATE * 0.5f
                               : bloodstained::enemy_drop_shuffle::DROP_RATE;
        for (std::size_t slot = 0; slot < assignment.items.size(); ++slot) {
            if (assignment.active[slot]) SetSlot(*row, slot, assignment.items[slot], rate);
        }
    }

    // Match True Randomizer's behavior for alternate/event-specific rows of
    // the same enemy while leaving shard and special-drop fields untouched.
    for (const auto& pair : dropManager->DropTable->RowMap) {
        const std::string rowName = pair.Key().ToString();
        const std::size_t separator = rowName.find('_');
        if (separator == std::string::npos) continue;
        const std::string enemy = rowName.substr(0, separator);
        const auto primary = primaryRows.find(enemy);
        if (primary == primaryRows.end() || rowName == enemy + "_Shard") continue;
        auto* row = reinterpret_cast<SDK::FPBDropRateMasterData*>(pair.Value());
        if (row) CopyOrdinaryDrops(*primary->second, *row);
    }

    Logger::Log(LogLevel::File, "[DropShuffle] Applied seed:", seed, "version:", version,
                "enemies:", result.enemies.size());
    return true;
}
