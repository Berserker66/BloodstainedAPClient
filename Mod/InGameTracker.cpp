#include "InGameTracker.h"

#include <CommonItemIcon_classes.hpp>
#include <CommonItemIcon_parameters.hpp>
#include <MapLocationBlueprint_classes.hpp>
#include <MapManageBlueprint_classes.hpp>
#include <ProjectBlood_classes.hpp>
#include <TreasureLocationBlueprint_classes.hpp>
#include <UMG_classes.hpp>
#include <UMG_parameters.hpp>
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "Archipelago.h"
#include "GameManager.h"
#include "Logger.h"
#include "Resource.h"
#include "Tracker.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

using bloodstained::tracker::Difficulty;
using bloodstained::tracker::Tracker;
using bloodstained::tracker::generated::LocationType;

Difficulty GetDifficulty() {
    switch (SDK::UPBGameInstance::GetGameLevel()) {
        case SDK::EPBGameLevel::Hard:
            return Difficulty::HARD;
        case SDK::EPBGameLevel::Nightmare:
            return Difficulty::NIGHTMARE;
        default:
            return Difficulty::NORMAL;
    }
}

std::string NormalizeTreasureId(std::string treasureId) {
    if (treasureId.size() >= 2 && treasureId[treasureId.size() - 2] == '.' &&
        std::isdigit(static_cast<unsigned char>(treasureId.back()))) {
        treasureId.resize(treasureId.size() - 2);
    }
    std::ranges::transform(treasureId, treasureId.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return treasureId;
}

SDK::FName NameFromString(std::string_view value) {
    std::wstring wideValue(value.begin(), value.end());
    return SDK::UKismetStringLibrary::Conv_StringToName(wideValue.c_str());
}

void SetVisible(SDK::UWidget* widget) {
    if (!widget) return;

    static SDK::UFunction* function = widget->Class->GetFunction("Widget", "SetVisibility");
    SDK::Params::Widget_SetVisibility parameters{};
    parameters.InVisibility = SDK::ESlateVisibility::HitTestInvisible;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    widget->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

void SetImageColor(SDK::UImage* image, const SDK::FLinearColor& color) {
    if (!image) return;

    static SDK::UFunction* function = image->Class->GetFunction("Image", "SetColorAndOpacity");
    SDK::Params::Image_SetColorAndOpacity parameters{};
    parameters.InColorAndOpacity = color;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    image->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

std::optional<SDK::FSlateBrush> CreateShardMarkerBrush(SDK::UObject* worldContext) {
    auto* player = GameManager::Instance().Player();
    if (!player || !player->CharacterInventory) return std::nullopt;

    SDK::FPBItemCatalogData shardData{};
    player->CharacterInventory->GetItemDataById(NameFromString("SummonDurahanMaHead"), &shardData);
    if (shardData.ID.ToString().empty()) return std::nullopt;

    static SDK::UFunction* createFunction =
        SDK::UWidgetBlueprintLibrary::StaticClass()->GetFunction("WidgetBlueprintLibrary", "Create");
    SDK::Params::WidgetBlueprintLibrary_Create createParameters{};
    createParameters.WorldContextObject = worldContext;
    createParameters.WidgetType = SDK::UCommonItemIcon_C::StaticClass();
    createParameters.OwningPlayer = GameManager::Instance().PlayerController();
    const auto createFlags = createFunction->FunctionFlags;
    createFunction->FunctionFlags |= 0x400;
    SDK::UWidgetBlueprintLibrary::GetDefaultObj()->ProcessEvent(createFunction, &createParameters);
    createFunction->FunctionFlags = createFlags;

    auto* widget = static_cast<SDK::UCommonItemIcon_C*>(createParameters.ReturnValue);
    if (!widget) return std::nullopt;
    static SDK::UFunction* setIconFunction = widget->Class->GetFunction("CommonItemIcon_C", "SetIconByData");
    SDK::Params::CommonItemIcon_C_SetIconByData setIconParameters{};
    setIconParameters.Input = shardData;
    widget->ProcessEvent(setIconFunction, &setIconParameters);
    if (!widget->icon) return std::nullopt;
    return widget->icon->Brush;
}

void SetImageBrush(SDK::UImage* image, const SDK::FSlateBrush& brush) {
    static SDK::UFunction* function = image->Class->GetFunction("Image", "SetBrush");
    SDK::Params::Image_SetBrush parameters{};
    parameters.InBrush = brush;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    image->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

SDK::UTexture2D* CreateEmbeddedTexture(SDK::UObject* worldContext, int resourceId, std::string_view description) {
    const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) {
        Logger::Log("[Tracker] Could not find embedded ", description, " texture");
        return nullptr;
    }

    const HGLOBAL loadedResource = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    auto* resourceBytes = static_cast<SDK::uint8*>(LockResource(loadedResource));
    if (!loadedResource || !resourceBytes || resourceSize == 0) {
        Logger::Log("[Tracker] Could not read embedded ", description, " texture");
        return nullptr;
    }

    SDK::TArray<SDK::uint8> textureBytes(resourceBytes, static_cast<SDK::int32>(resourceSize),
                                         static_cast<SDK::int32>(resourceSize));
    auto* texture = SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(worldContext, textureBytes);
    if (!texture) Logger::Log("[Tracker] Unreal could not import embedded ", description, " texture");
    return texture;
}

void SetImageTexture(SDK::UImage* image, SDK::UTexture2D* texture) {
    if (!image || !texture) return;

    static SDK::UFunction* function = image->Class->GetFunction("Image", "SetBrushFromTexture");
    SDK::Params::Image_SetBrushFromTexture parameters{};
    parameters.Texture = texture;
    parameters.bMatchSize = false;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    image->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

}  // namespace

InGameTracker& InGameTracker::Instance() {
    static InGameTracker instance;
    return instance;
}

void InGameTracker::ApplyMapMarkers(void* mapWidget) {
    auto* archipelago = Archipelago::ConnectedInstance();
    auto* map = static_cast<SDK::UMapManageBlueprint_C*>(mapWidget);
    if (!archipelago || !map) return;

    Tracker tracker;
    tracker.SetInventory(archipelago->GetTrackerInventory());
    const Difficulty difficulty = GetDifficulty();
    const auto reachableLocations =
        tracker.GetReachableMissingLocations(difficulty, archipelago->GetMissingLocationIds());

    std::unordered_set<std::string> treasureIds;
    std::unordered_set<std::string> markedRooms;
    std::unordered_set<std::string> wallRooms;
    std::unordered_map<std::string, std::string> enemyByRoom;
    for (const auto* location : reachableLocations) {
        if (!archipelago->IsMissingLocation(std::string(location->name), location->id)) continue;
        if (location->type == LocationType::CHEST) {
            treasureIds.insert(NormalizeTreasureId(std::string(location->name)));
        } else if (location->type == LocationType::WALL) {
            markedRooms.insert(std::string(location->room));
            wallRooms.insert(std::string(location->room));
        } else if (location->type == LocationType::ENEMY) {
            std::string enemyId(location->name.substr(0, location->name.size() - std::string_view("_Shard").size()));
            for (const std::string_view room : tracker.GetReachableEnemyRooms(*location, difficulty)) {
                markedRooms.insert(std::string(room));
                enemyByRoom.insert_or_assign(std::string(room), enemyId);
            }
        }
    }

    std::size_t treasureMarkers = 0;
    std::string markedTreasureIds;
    auto* chestMarkerTexture =
        treasureIds.empty() ? nullptr : CreateEmbeddedTexture(map, IDR_CHEST_MARKER_PNG, "chest marker");
    for (auto* marker : map->TreasureMarkerList) {
        if (!marker || !treasureIds.contains(NormalizeTreasureId(marker->treasureID.ToString()))) continue;
        SetVisible(marker);
        SetVisible(marker->Image_30);
        if (chestMarkerTexture) SetImageTexture(marker->Image_30, chestMarkerTexture);
        SetImageColor(marker->Image_30, chestMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                         : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        if (!markedTreasureIds.empty()) markedTreasureIds += ", ";
        markedTreasureIds += marker->treasureID.ToString();
        treasureMarkers++;
    }

    const auto shardMarkerBrush = enemyByRoom.empty() ? std::nullopt : CreateShardMarkerBrush(map);
    auto* wallMarkerTexture =
        wallRooms.empty() ? nullptr : CreateEmbeddedTexture(map, IDR_WALL_MARKER_PNG, "wall marker");
    std::size_t roomMarkers = 0;
    for (const auto& markerEntry : map->RoomMarkerMap) {
        std::string roomId = markerEntry.Key().ToString();
        auto* marker = markerEntry.Value();
        if (!marker || !markedRooms.contains(roomId)) continue;

        SetVisible(marker);
        SetVisible(marker->Image_71);
        auto enemy = enemyByRoom.find(roomId);
        if (enemy != enemyByRoom.end()) {
            marker->EnemyType = NameFromString(enemy->second);
            if (shardMarkerBrush) SetImageBrush(marker->Image_71, *shardMarkerBrush);
            SetImageColor(marker->Image_71, shardMarkerBrush ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                           : SDK::FLinearColor{1.0f, 0.15f, 0.8f, 1.0f});
        } else {
            if (wallMarkerTexture) SetImageTexture(marker->Image_71, wallMarkerTexture);
            SetImageColor(marker->Image_71, wallMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                             : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        }
        roomMarkers++;
    }

    Logger::Log("[Tracker] Marked ", treasureMarkers, " treasure icons and ", roomMarkers,
                " wall/enemy rooms from ", reachableLocations.size(), " reachable missing checks");
    if (!markedTreasureIds.empty()) {
        Logger::Log(LogLevel::Debug, "[Tracker] In-logic treasure IDs: ", markedTreasureIds);
    }
}
