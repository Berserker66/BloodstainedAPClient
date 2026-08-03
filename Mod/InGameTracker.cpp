#include "InGameTracker.h"

#include <Engine_classes.hpp>
#include <MapLocationBlueprint_classes.hpp>
#include <MapManageBlueprint_classes.hpp>
#include <MiniMapBlueprint_classes.hpp>
#include <PBBronzeTreasureBox_BP_classes.hpp>
#include <ProjectBlood_classes.hpp>
#include <TreasureLocationMinimapBlueprint_classes.hpp>
#include <TreasureLocationBlueprint_classes.hpp>
#include <TotalMapBlueprint_classes.hpp>
#include <UMG_classes.hpp>
#include <UMG_parameters.hpp>
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

constexpr std::string_view TRACKER_DISPLAY_MODE_SAVE_KEY = "AP_TrackerDisplayMode";
// Opt-in troubleshooting for native/AP chest identity and placement. Keep disabled in release builds; unlike the
// discarded global object scans, this only inspects the current room when the minimap target set is rebuilt.
constexpr bool ENABLE_TRACKER_DIAGNOSTICS = false;
// The minimap collectible prototype publishes plain pixel rectangles to the DX11 Present hook. It never creates,
// reparents, borrows, or registers Unreal widgets, so the Blueprint remains the sole owner of its marker arrays.
// Render collectible targets outside the Blueprint-owned minimap marker arrays.
// Startup safety is provided by GameManager's raw-pointer readiness path, so this
// code is not reached until the native minimap has completed its own Tick.
constexpr bool ENABLE_SCREEN_SPACE_MINIMAP_COLLECTIBLES = true;
// Detached minimap widgets do not enter the live Slate tree, while borrowing or registering native treasure widgets
// corrupts Blueprint-owned state and can crash during startup. Keep all experimental minimap collectibles disabled
// until they are implemented outside the native widget arrays (for example, a render-thread screen-space overlay).
constexpr bool ENABLE_EXPERIMENTAL_MINIMAP_COLLECTIBLES = false;
// MiniMapBlueprint owns and rewrites every TreasureIconList entry during Tick. Borrowing even an inactive entry causes
// the Blueprint and the tracker to alternately reclaim it every frame, producing duplicate/orange chest clusters and
// eventually corrupting widget state. Keep native marker pooling permanently disabled.
constexpr bool ENABLE_NATIVE_MINIMAP_MARKER_POOL = false;
std::unordered_set<SDK::UTreasureLocationMinimapBlueprint_C*> BORROWED_MINIMAP_MARKERS;

std::array<SDK::uint8, 156> GHOST_MAP_TILE_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00, 0x10, 0x08, 0x06, 0x00, 0x00, 0x00, 0x05, 0xCF, 0x1F,
    0xEF, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xAE, 0xCE, 0x1C, 0xE9, 0x00, 0x00,
    0x00, 0x04, 0x67, 0x41, 0x4D, 0x41, 0x00, 0x00, 0xB1, 0x8F, 0x0B, 0xFC, 0x61, 0x05, 0x00, 0x00,
    0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0E, 0xC3, 0x00, 0x00, 0x0E, 0xC3, 0x01, 0xC7,
    0x6F, 0xA8, 0x64, 0x00, 0x00, 0x00, 0x31, 0x49, 0x44, 0x41, 0x54, 0x48, 0x4B, 0x63, 0x38, 0x73,
    0xE6, 0xCC, 0x66, 0x7A, 0x62, 0x06, 0x74, 0x01, 0x5A, 0x63, 0xB8, 0x85, 0xCB, 0x96, 0x2D, 0xF3,
    0xA5, 0x25, 0x1E, 0xB5, 0x90, 0xEA, 0x78, 0xD4, 0x42, 0xAA, 0xE3, 0x51, 0x0B, 0xA9, 0x8E, 0x47,
    0xA0, 0x85, 0xF4, 0xC2, 0x74, 0xB7, 0x10, 0x00, 0xAA, 0xE1, 0x75, 0x7D, 0xE9, 0x08, 0x53, 0x1B,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

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

SDK::FName NameFromString(std::string_view value);

const bloodstained::tracker::generated::LocationData* FindChestLocationByNativeId(
    std::string_view normalizedNativeId) {
    for (const auto& binding : bloodstained::tracker::generated::LOCATION_BINDINGS) {
        if (NormalizeTreasureId(std::string(binding.native_name)) != normalizedNativeId) continue;
        for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
            if (location.id == binding.id && location.type == LocationType::CHEST) return &location;
        }
    }
    return nullptr;
}

bool HasAuthoritativeChestPosition(std::string_view normalizedNativeId) {
    const auto* location = FindChestLocationByNativeId(normalizedNativeId);
    return location && location->has_map_position;
}

SDK::FName NameFromString(std::string_view value) {
    std::wstring wideValue(value.begin(), value.end());
    return SDK::UKismetStringLibrary::Conv_StringToName(wideValue.c_str());
}

void SetWidgetVisibility(SDK::UWidget* widget, SDK::ESlateVisibility visibility) {
    if (!widget) return;

    static SDK::UFunction* function = widget->Class->GetFunction("Widget", "SetVisibility");
    SDK::Params::Widget_SetVisibility parameters{};
    parameters.InVisibility = visibility;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    widget->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

void SetVisible(SDK::UWidget* widget) {
    SetWidgetVisibility(widget, SDK::ESlateVisibility::HitTestInvisible);
}

void SetCollapsed(SDK::UWidget* widget) {
    SetWidgetVisibility(widget, SDK::ESlateVisibility::Collapsed);
}

TrackedWidgetHandle TrackWidget(SDK::UWidget* widget) {
    return widget ? TrackedWidgetHandle{widget, widget->Index} : TrackedWidgetHandle{};
}

SDK::UWidget* ResolveWidget(const TrackedWidgetHandle& handle) {
    if (!handle.pointer || handle.objectIndex < 0) return nullptr;
    auto* object = SDK::UObject::GObjects->GetByIndex(handle.objectIndex);
    if (object != handle.pointer || !object->Class) return nullptr;
    return static_cast<SDK::UWidget*>(object);
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

void SetImageBrush(SDK::UImage* image, const SDK::FSlateBrush& brush) {
    static SDK::UFunction* function = image->Class->GetFunction("Image", "SetBrush");
    SDK::Params::Image_SetBrush parameters{};
    parameters.InBrush = brush;
    const auto flags = function->FunctionFlags;
    function->FunctionFlags |= 0x400;
    image->ProcessEvent(function, &parameters);
    function->FunctionFlags = flags;
}

struct NativeImageBrushOverride {
    TrackedWidgetHandle image;
    SDK::FSlateBrush nativeBrush;
};

std::vector<NativeImageBrushOverride> MAIN_MAP_NATIVE_BRUSH_OVERRIDES;
std::vector<NativeImageBrushOverride> MINI_MAP_NATIVE_BRUSH_OVERRIDES;
SDK::int32 RECONCILED_TREASURE_COMPONENT_INDEX = -1;
std::unordered_set<std::string> RECONCILED_FINISHED_TREASURE_IDS;

void RememberNativeImageBrush(std::vector<NativeImageBrushOverride>& overrides, SDK::UImage* image) {
    if (!image) return;
    const auto handle = TrackWidget(image);
    const auto existing = std::find_if(overrides.begin(), overrides.end(), [&](const auto& entry) {
        return entry.image.objectIndex == handle.objectIndex;
    });
    if (existing == overrides.end()) overrides.push_back({handle, image->Brush});
}

void RestoreNativeImageBrushes(std::vector<NativeImageBrushOverride>& overrides) {
    for (const auto& entry : overrides) {
        if (auto* image = static_cast<SDK::UImage*>(ResolveWidget(entry.image))) {
            SetImageBrush(image, entry.nativeBrush);
        }
    }
    overrides.clear();
}

std::size_t ReconcileFinishedTreasureMarkers(Archipelago* archipelago) {
    if (!archipelago) return 0;
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* treasureComponent = hud ? hud->m_MapTreasureIconComponent : nullptr;
    if (!treasureComponent) return 0;

    if (RECONCILED_TREASURE_COMPONENT_INDEX != treasureComponent->Index) {
        RECONCILED_TREASURE_COMPONENT_INDEX = treasureComponent->Index;
        RECONCILED_FINISHED_TREASURE_IDS.clear();
    }

    std::vector<std::pair<SDK::FName, SDK::FVector2D>> finishedLocations;
    for (const auto& entry : treasureComponent->TreasureMarkerMapLocation) {
        const std::string treasureId = entry.Key().ToString();
        if (RECONCILED_FINISHED_TREASURE_IDS.contains(NormalizeTreasureId(treasureId)) ||
            !archipelago->WasLocationClearedLocally(treasureId)) {
            continue;
        }
        finishedLocations.emplace_back(entry.Key(), entry.Value());
    }

    for (const auto& [treasureId, location] : finishedLocations) {
        treasureComponent->AddTreasureLocation(treasureId, location, true);
        RECONCILED_FINISHED_TREASURE_IDS.insert(NormalizeTreasureId(treasureId.ToString()));
    }
    if (!finishedLocations.empty()) {
        Logger::Log(LogLevel::File, "[Tracker] Reconciled native finished state for",
                    finishedLocations.size(), "locally cleared AP treasure markers");
    }
    return finishedLocations.size();
}

template <typename MarkerArray>
void SuppressFinishedTreasureMarkers(const MarkerArray& markers) {
    for (auto* marker : markers) {
        if (!marker ||
            !RECONCILED_FINISHED_TREASURE_IDS.contains(NormalizeTreasureId(marker->treasureID.ToString()))) {
            continue;
        }
        SetCollapsed(marker->Image_30);
        SetCollapsed(marker);
    }
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

void SynchronizeDynamicImage(SDK::UImage* image) {
    if (!image) return;
    // Images added after the minimap Blueprint's normal synchronization/prepass otherwise retain a zero-sized Slate
    // representation even though their UCanvasPanelSlot is valid. Bloodstained exposes this helper for late widgets.
    SDK::UPBUserWidget::ManualSynchronizeProperties(image);
    image->ForceLayoutPrepass();
}

SDK::UTexture2D* CreateGhostMapTileTexture(SDK::UObject* worldContext) {
    SDK::TArray<SDK::uint8> textureBytes(GHOST_MAP_TILE_PNG.data(), static_cast<SDK::int32>(GHOST_MAP_TILE_PNG.size()),
                                         static_cast<SDK::int32>(GHOST_MAP_TILE_PNG.size()));
    auto* texture = SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(worldContext, textureBytes);
    if (!texture) Logger::Log("[Tracker] Unreal could not import the ghost-map tile texture");
    return texture;
}

SDK::UTotalMapBlueprint_C* GetMapForType(SDK::UMapManageBlueprint_C* map, SDK::EDivideMap mapType) {
    switch (mapType) {
        case SDK::EDivideMap::Sip:
            return map->SipMap;
        case SDK::EDivideMap::Alchemy:
            return map->AlchemyMap;
        case SDK::EDivideMap::Main:
            return map->MainMap;
        default:
            return nullptr;
    }
}

struct GhostMapGeometry {
    SDK::FGeometry canvas;
    SDK::FGeometry renderTarget;
    SDK::FVector2D canvasCenter;
    SDK::FVector2D gridOriginOffset;
    SDK::FVector2D markerAnchorCorrection;
    SDK::FVector2D cellSize;
};

std::optional<GhostMapGeometry> GetMapGeometry(SDK::UTotalMapBlueprint_C* map, SDK::UImage* renderTarget) {
    if (!map || !map->ImageParent_Canvas || !renderTarget) return std::nullopt;

    GhostMapGeometry geometry{map->ImageParent_Canvas->GetCachedGeometry(), renderTarget->GetCachedGeometry(),
                              {}, {}, {}, {}};
    const SDK::FVector2D canvasSize = SDK::USlateBlueprintLibrary::GetLocalSize(geometry.canvas);
    const SDK::FVector2D renderSize = SDK::USlateBlueprintLibrary::GetLocalSize(geometry.renderTarget);
    if (canvasSize.X <= 0.0f || canvasSize.Y <= 0.0f || renderSize.X <= 0.0f || renderSize.Y <= 0.0f) {
        return std::nullopt;
    }
    geometry.canvasCenter = canvasSize * 0.5f;

    const SDK::FVector2D absoluteGridOrigin =
        SDK::USlateBlueprintLibrary::TransformVectorLocalToAbsolute(geometry.renderTarget, map->IconPixelOffset);
    geometry.gridOriginOffset =
        SDK::USlateBlueprintLibrary::TransformVectorAbsoluteToLocal(geometry.canvas, absoluteGridOrigin);
    const SDK::FVector2D absoluteCellSize =
        SDK::USlateBlueprintLibrary::TransformVectorLocalToAbsolute(geometry.renderTarget, map->RoomPixelSize);
    geometry.cellSize =
        SDK::USlateBlueprintLibrary::TransformVectorAbsoluteToLocal(geometry.canvas, absoluteCellSize);
    return geometry;
}

std::optional<GhostMapGeometry> GetGhostMapGeometry(SDK::UTotalMapBlueprint_C* map) {
    return GetMapGeometry(map, map ? map->RenderTargetTotal_Image : nullptr);
}

std::optional<GhostMapGeometry> GetMiniMapGeometry(SDK::UTotalMapBlueprint_C* map) {
    return GetMapGeometry(map, map ? map->RenderTargetMini_Image : nullptr);
}

SDK::FVector2D MapRenderToCanvas(const GhostMapGeometry& geometry, const SDK::FVector2D& renderPosition) {
    const SDK::FVector2D absolutePosition =
        SDK::USlateBlueprintLibrary::LocalToAbsolute(geometry.renderTarget, renderPosition);
    return SDK::USlateBlueprintLibrary::AbsoluteToLocal(geometry.canvas, absolutePosition) +
           geometry.canvasCenter + geometry.gridOriginOffset + geometry.markerAnchorCorrection;
}

std::optional<SDK::FVector2D> FindNativeRoomMarkerCorrection(SDK::UMapManageBlueprint_C* map,
                                                             SDK::UPBMapComponent* mapComponent,
                                                             SDK::EDivideMap mapType,
                                                             SDK::UTotalMapBlueprint_C* totalMap,
                                                             const GhostMapGeometry& geometry) {
    std::vector<float> xCorrections;
    std::vector<float> yCorrections;
    for (const auto& markerEntry : map->RoomMarkerMap) {
        const std::string roomName = markerEntry.Key().ToString();
        const auto* room = Tracker::FindRoom(roomName);
        auto* marker = markerEntry.Value();
        if (!room || room->width != 1 || room->height != 1 || !marker || !marker->Image_71) continue;

        const SDK::FName roomId = NameFromString(roomName);
        const SDK::EAreaID area = map->GetMapManager()->RoomIdToAreaId(roomId);
        if (mapComponent->CheckMapType(area) != mapType) continue;

        const SDK::FGeometry markerGeometry = marker->Image_71->GetCachedGeometry();
        const SDK::FVector2D markerSize = SDK::USlateBlueprintLibrary::GetLocalSize(markerGeometry);
        if (markerSize.X <= 0.0f || markerSize.Y <= 0.0f) continue;

        const SDK::FVector2D markerAbsolute =
            SDK::USlateBlueprintLibrary::LocalToAbsolute(markerGeometry, markerSize * 0.5f);
        const SDK::FVector2D markerCenter =
            SDK::USlateBlueprintLibrary::AbsoluteToLocal(geometry.canvas, markerAbsolute);
        const SDK::FVector2D nativeCellCenter = mapComponent->GetRoomCenterInMapPosition(
            mapType, roomId, 1, totalMap->IconPixelOffset, totalMap->canvasSize);
        const SDK::FVector2D correction = markerCenter - MapRenderToCanvas(geometry, nativeCellCenter);
        if (std::abs(correction.X) > geometry.cellSize.X || std::abs(correction.Y) > geometry.cellSize.Y) continue;
        xCorrections.push_back(correction.X);
        yCorrections.push_back(correction.Y);
    }

    if (xCorrections.empty()) return std::nullopt;
    std::sort(xCorrections.begin(), xCorrections.end());
    std::sort(yCorrections.begin(), yCorrections.end());
    const std::size_t middle = xCorrections.size() / 2;
    return SDK::FVector2D{xCorrections[middle], yCorrections[middle]};
}

struct NativeMapAxes {
    SDK::FVector2D x;
    SDK::FVector2D z;
};

constexpr float WALL_MARKER_Z_OFFSET = 0.25f;

float GetLocationMarkerMapZ(const bloodstained::tracker::generated::LocationData& location,
                            std::uint32_t roomHeight) {
    // Item-wall actors place their root a quarter of a native map tile below the visible break point. This is only
    // a few pixels on the total map, but the minimap zoom magnifies it noticeably. Keep the exported actor position
    // authoritative and apply the presentation offset consistently in both map renderers.
    const float markerOffset = location.type == LocationType::WALL ? WALL_MARKER_Z_OFFSET : 0.0f;
    return std::clamp(location.map_z + markerOffset, 0.0f, static_cast<float>(roomHeight));
}

NativeMapAxes FindNativeMapAxes(SDK::UPBMapManager* mapManager,
                                SDK::UPBMapComponent* mapComponent,
                                SDK::EDivideMap mapType,
                                SDK::UTotalMapBlueprint_C* totalMap,
                                const GhostMapGeometry& geometry,
                                std::optional<SDK::EDivideMap> areaMapType = std::nullopt) {
    NativeMapAxes axes{{geometry.cellSize.X, 0.0f}, {0.0f, geometry.cellSize.Y}};
    bool foundX = false;
    bool foundZ = false;
    for (const auto& room : bloodstained::tracker::generated::ROOMS) {
        if (room.out_of_map || (room.width < 2 && room.height < 2)) continue;
        const SDK::FName roomId = NameFromString(room.name);
        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        if (mapComponent->CheckMapType(area) != areaMapType.value_or(mapType)) continue;

        const SDK::FVector2D first = MapRenderToCanvas(
            geometry,
            mapComponent->GetRoomCenterInMapPosition(mapType, roomId, 1, totalMap->IconPixelOffset,
                                                     totalMap->canvasSize));
        if (!foundX && room.width >= 2) {
            const SDK::FVector2D nextX = MapRenderToCanvas(
                geometry,
                mapComponent->GetRoomCenterInMapPosition(mapType, roomId, 2, totalMap->IconPixelOffset,
                                                         totalMap->canvasSize));
            axes.x = nextX - first;
            foundX = std::abs(axes.x.X) + std::abs(axes.x.Y) > 0.01f;
        }
        if (!foundZ && room.height >= 2) {
            const SDK::FVector2D nextZ = MapRenderToCanvas(
                geometry,
                mapComponent->GetRoomCenterInMapPosition(mapType, roomId, room.width + 1,
                                                         totalMap->IconPixelOffset, totalMap->canvasSize));
            axes.z = nextZ - first;
            foundZ = std::abs(axes.z.X) + std::abs(axes.z.Y) > 0.01f;
        }
        if (foundX && foundZ) break;
    }
    return axes;
}

std::optional<SDK::FVector2D> GetStaticLocationMapPosition(
    SDK::UPBMapComponent* mapComponent,
    SDK::EDivideMap mapType,
    SDK::UTotalMapBlueprint_C* totalMap,
    const NativeMapAxes& axes,
    const bloodstained::tracker::generated::LocationData& location) {
    if (!mapComponent || !totalMap || !location.has_map_position) return std::nullopt;
    const auto* room = Tracker::FindRoom(location.room);
    if (!room || room->out_of_map || room->width == 0 || room->height == 0) return std::nullopt;

    const float mapX = std::clamp(location.map_x, 0.0f, static_cast<float>(room->width));
    const float mapZ = std::clamp(location.map_z, 0.0f, static_cast<float>(room->height));
    const std::uint32_t cellX = std::min(static_cast<std::uint32_t>(mapX), room->width - 1u);
    const std::uint32_t cellZ = std::min(static_cast<std::uint32_t>(mapZ), room->height - 1u);
    const std::uint32_t assignment = cellZ * room->width + cellX + 1u;
    const SDK::FVector2D cellCenter = mapComponent->GetRoomCenterInMapPosition(
        mapType, NameFromString(location.room), static_cast<SDK::int32>(assignment),
        totalMap->IconPixelOffset, totalMap->canvasSize);
    return cellCenter + axes.x * (mapX - static_cast<float>(cellX) - 0.5f) +
           axes.z * (mapZ - static_cast<float>(cellZ) - 0.5f);
}

SDK::FVector2D FindNativeRoomMarkerSize(SDK::UMapManageBlueprint_C* map,
                                        SDK::UPBMapComponent* mapComponent,
                                        SDK::EDivideMap mapType,
                                        const GhostMapGeometry& geometry) {
    for (const auto& markerEntry : map->RoomMarkerMap) {
        auto* marker = markerEntry.Value();
        if (!marker || !marker->Image_71) continue;
        const SDK::FName roomId = markerEntry.Key();
        const SDK::EAreaID area = map->GetMapManager()->RoomIdToAreaId(roomId);
        if (mapComponent->CheckMapType(area) != mapType) continue;

        const SDK::FGeometry markerGeometry = marker->Image_71->GetCachedGeometry();
        const SDK::FVector2D localSize = SDK::USlateBlueprintLibrary::GetLocalSize(markerGeometry);
        if (localSize.X <= 0.0f || localSize.Y <= 0.0f) continue;
        const SDK::FVector2D absoluteSize =
            SDK::USlateBlueprintLibrary::TransformVectorLocalToAbsolute(markerGeometry, localSize);
        const SDK::FVector2D canvasSize =
            SDK::USlateBlueprintLibrary::TransformVectorAbsoluteToLocal(geometry.canvas, absoluteSize);
        if (canvasSize.X > 0.0f && canvasSize.Y > 0.0f) return canvasSize;
    }
    return geometry.cellSize * 0.35f;
}

bool HasLaidOutMap(SDK::UMapManageBlueprint_C* map) {
    return GetGhostMapGeometry(map ? map->SipMap : nullptr).has_value() ||
           GetGhostMapGeometry(map ? map->MainMap : nullptr).has_value() ||
           GetGhostMapGeometry(map ? map->AlchemyMap : nullptr).has_value();
}

std::size_t RenderReachableRoomGhosts(SDK::UMapManageBlueprint_C* map,
                                      const std::unordered_set<std::string>& reachableRooms,
                                      std::vector<TrackedWidgetHandle>& spawnedGhostWidgets) {
    auto* mapManager = map->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return 0;

    auto* texture = CreateGhostMapTileTexture(map);
    if (!texture) return 0;

    std::size_t renderedCells = 0;
    std::set<std::tuple<SDK::uint8, SDK::int32, SDK::int32>> renderedPositions;
    std::array<std::optional<GhostMapGeometry>, 4> mapGeometry;
    for (const std::string& roomName : reachableRooms) {
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map) continue;

        bool roomComplete = false;
        const SDK::FName roomId = NameFromString(roomName);
        if (mapManager->IsKnowledgeRoom(roomId, &roomComplete) && roomComplete) continue;

        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        const SDK::EDivideMap mapType = mapComponent->CheckMapType(area);
        auto* totalMap = GetMapForType(map, mapType);
        if (!totalMap || !totalMap->ImageParent_Canvas || totalMap->RoomPixelSize.X <= 0.0f ||
            totalMap->RoomPixelSize.Y <= 0.0f) {
            Logger::Log(LogLevel::Warning, "[Tracker] No active map canvas for reachable room:", roomName);
            continue;
        }

        const auto mapIndex = static_cast<std::size_t>(mapType);
        if (!mapGeometry[mapIndex]) {
            mapGeometry[mapIndex] = GetGhostMapGeometry(totalMap);
            if (!mapGeometry[mapIndex]) continue;
            const auto markerCorrection =
                FindNativeRoomMarkerCorrection(map, mapComponent, mapType, totalMap, *mapGeometry[mapIndex]);
            mapGeometry[mapIndex]->markerAnchorCorrection =
                markerCorrection.value_or(SDK::FVector2D{0.0f, mapGeometry[mapIndex]->cellSize.Y * 0.5f});
        }

        int32_t zOrder = -1;
        if (auto* renderSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(totalMap->RenderTargetTotal_Image)) {
            // The native render target is also our precise per-cell exploration mask. Keeping the ghost layer
            // immediately behind it lets opaque discovered cells hide their ghosts while transparent unexplored
            // cells reveal them. Tracker icons remain above both layers.
            zOrder = renderSlot->GetZOrder() - 1;
        }

        const std::uint32_t cellCount = static_cast<std::uint32_t>(room->width) * room->height;
        for (std::uint32_t cell = 0; cell < cellCount; ++cell) {
            if (!Tracker::IsRoomCellVisible(*room, cell + 1)) continue;
            const SDK::FVector2D nativeCellCenter = mapComponent->GetRoomCenterInMapPosition(
                mapType, roomId, static_cast<SDK::int32>(cell + 1), totalMap->IconPixelOffset, totalMap->canvasSize);
            const SDK::FVector2D cellCenter = MapRenderToCanvas(*mapGeometry[mapIndex], nativeCellCenter);
            const auto positionKey = std::make_tuple(
                static_cast<SDK::uint8>(mapType), static_cast<SDK::int32>(std::lround(cellCenter.X * 100.0f)),
                static_cast<SDK::int32>(std::lround(cellCenter.Y * 100.0f)));
            if (!renderedPositions.insert(positionKey).second) continue;

            auto* image = static_cast<SDK::UImage*>(
                SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), totalMap->ImageParent_Canvas));
            if (!image) continue;
            SetImageTexture(image, texture);
            SetImageColor(image, SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f});

            auto* slot = totalMap->ImageParent_Canvas->AddChildToCanvas(image);
            if (!slot) continue;
            slot->SetAlignment({0.5f, 0.5f});
            slot->SetPosition(cellCenter);
            slot->SetSize(mapGeometry[mapIndex]->cellSize);
            slot->SetZOrder(zOrder);
            SetVisible(image);
            spawnedGhostWidgets.push_back(TrackWidget(image));
            ++renderedCells;
        }
    }
    return renderedCells;
}

std::size_t RenderWallLocationMarkers(SDK::UMapManageBlueprint_C* map,
                                      const std::unordered_set<std::uint64_t>& wallLocationIds,
                                      std::vector<TrackedWidgetHandle>& spawnedMarkerWidgets) {
    if (wallLocationIds.empty()) return 0;

    auto* mapManager = map->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    auto* wallMarkerTexture = CreateEmbeddedTexture(map, IDR_WALL_MARKER_PNG, "wall marker");
    if (!mapManager || !mapComponent) return 0;

    std::size_t wallMarkers = 0;
    std::array<std::optional<GhostMapGeometry>, 4> wallGeometry;
    std::array<std::optional<NativeMapAxes>, 4> wallAxes;
    std::array<std::optional<SDK::FVector2D>, 4> wallMarkerSizes;
    for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
        if (!wallLocationIds.contains(location.id)) continue;
        const auto* room = Tracker::FindRoom(location.room);
        if (!room || room->out_of_map || room->width == 0 || room->height == 0) continue;

        const SDK::FName roomId = NameFromString(location.room);
        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        const SDK::EDivideMap mapType = mapComponent->CheckMapType(area);
        auto* totalMap = GetMapForType(map, mapType);
        if (!totalMap || !totalMap->ImageParent_Canvas) continue;

        const auto mapIndex = static_cast<std::size_t>(mapType);
        if (!wallGeometry[mapIndex]) {
            wallGeometry[mapIndex] = GetGhostMapGeometry(totalMap);
            if (!wallGeometry[mapIndex]) continue;
            wallGeometry[mapIndex]->markerAnchorCorrection =
                FindNativeRoomMarkerCorrection(map, mapComponent, mapType, totalMap, *wallGeometry[mapIndex])
                    .value_or(SDK::FVector2D{0.0f, wallGeometry[mapIndex]->cellSize.Y * 0.5f});
            wallAxes[mapIndex] =
                FindNativeMapAxes(mapManager, mapComponent, mapType, totalMap, *wallGeometry[mapIndex]);
            wallMarkerSizes[mapIndex] =
                FindNativeRoomMarkerSize(map, mapComponent, mapType, *wallGeometry[mapIndex]);
        }

        const float mapX = std::clamp(location.map_x, 0.0f, static_cast<float>(room->width));
        const float mapZ = GetLocationMarkerMapZ(location, room->height);
        const std::uint32_t cellX = std::min(static_cast<std::uint32_t>(mapX), room->width - 1u);
        const std::uint32_t cellZ = std::min(static_cast<std::uint32_t>(mapZ), room->height - 1u);
        const std::uint32_t assignment = cellZ * room->width + cellX + 1u;
        const SDK::FVector2D nativeCellCenter = mapComponent->GetRoomCenterInMapPosition(
            mapType, roomId, static_cast<SDK::int32>(assignment), totalMap->IconPixelOffset, totalMap->canvasSize);
        const SDK::FVector2D cellCenter = MapRenderToCanvas(*wallGeometry[mapIndex], nativeCellCenter);
        const SDK::FVector2D markerCenter =
            cellCenter + wallAxes[mapIndex]->x * (mapX - static_cast<float>(cellX) - 0.5f) +
            wallAxes[mapIndex]->z * (mapZ - static_cast<float>(cellZ) - 0.5f);

        auto* image = static_cast<SDK::UImage*>(
            SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), totalMap->ImageParent_Canvas));
        if (!image) continue;
        if (wallMarkerTexture) SetImageTexture(image, wallMarkerTexture);
        SetImageColor(image, wallMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                               : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        auto* slot = totalMap->ImageParent_Canvas->AddChildToCanvas(image);
        if (!slot) continue;
        slot->SetAlignment({0.5f, 0.5f});
        slot->SetPosition(markerCenter);
        slot->SetSize(*wallMarkerSizes[mapIndex]);
        if (auto* renderSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(totalMap->RenderTargetTotal_Image)) {
            slot->SetZOrder(renderSlot->GetZOrder() + 1);
        }
        SetVisible(image);
        spawnedMarkerWidgets.push_back(TrackWidget(image));
        wallMarkers++;
    }
    return wallMarkers;
}

SDK::FVector2D FindNativeTreasureMarkerSize(SDK::UMapManageBlueprint_C* map,
                                            SDK::UPBMapComponent* mapComponent,
                                            SDK::EDivideMap mapType,
                                            const GhostMapGeometry& geometry) {
    for (auto* marker : map->TreasureMarkerList) {
        if (!marker || !marker->Image_30) continue;
        const SDK::FGeometry markerGeometry = marker->Image_30->GetCachedGeometry();
        const SDK::FVector2D localSize = SDK::USlateBlueprintLibrary::GetLocalSize(markerGeometry);
        if (localSize.X > 0.0f && localSize.Y > 0.0f) {
            const SDK::FVector2D absoluteSize =
                SDK::USlateBlueprintLibrary::TransformVectorLocalToAbsolute(markerGeometry, localSize);
            const SDK::FVector2D canvasSize =
                SDK::USlateBlueprintLibrary::TransformVectorAbsoluteToLocal(geometry.canvas, absoluteSize);
            if (canvasSize.X > 0.0f && canvasSize.Y > 0.0f) return canvasSize;
        }
        if (auto* markerSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker)) {
            const SDK::FVector2D slotSize = markerSlot->GetSize();
            if (slotSize.X > 0.0f && slotSize.Y > 0.0f) return slotSize * 1.25f;
        }
    }
    return FindNativeRoomMarkerSize(map, mapComponent, mapType, geometry);
}

std::size_t RenderSyntheticTreasureMarkers(
    SDK::UMapManageBlueprint_C* map,
    const std::unordered_set<std::string>& treasureIds,
    std::vector<TrackedWidgetHandle>& spawnedMarkerWidgets) {
    if (!map || treasureIds.empty()) return 0;

    auto* mapManager = map->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return 0;

    auto* chestTexture = CreateEmbeddedTexture(map, IDR_CHEST_MARKER_PNG, "chest marker");
    std::array<std::optional<GhostMapGeometry>, 4> geometries;
    std::array<std::optional<NativeMapAxes>, 4> axes;
    std::array<std::optional<SDK::FVector2D>, 4> markerSizes;
    std::size_t rendered = 0;
    for (const auto& treasureId : treasureIds) {
        const auto* location = FindChestLocationByNativeId(treasureId);
        if (!location || !location->has_map_position) continue;
        const SDK::FName roomId = NameFromString(location->room);
        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        const SDK::EDivideMap mapType = mapComponent->CheckMapType(area);
        auto* totalMap = GetMapForType(map, mapType);
        if (!totalMap || !totalMap->ImageParent_Canvas) continue;

        const auto mapIndex = static_cast<std::size_t>(mapType);
        if (mapIndex >= geometries.size()) continue;
        if (!geometries[mapIndex]) {
            geometries[mapIndex] = GetGhostMapGeometry(totalMap);
            if (!geometries[mapIndex]) continue;
            geometries[mapIndex]->markerAnchorCorrection =
                FindNativeRoomMarkerCorrection(map, mapComponent, mapType, totalMap, *geometries[mapIndex])
                    .value_or(SDK::FVector2D{0.0f, geometries[mapIndex]->cellSize.Y * 0.5f});
            axes[mapIndex] =
                FindNativeMapAxes(mapManager, mapComponent, mapType, totalMap, *geometries[mapIndex]);
            markerSizes[mapIndex] =
                FindNativeTreasureMarkerSize(map, mapComponent, mapType, *geometries[mapIndex]);
        }

        const auto renderPosition =
            GetStaticLocationMapPosition(mapComponent, mapType, totalMap, *axes[mapIndex], *location);
        if (!renderPosition) continue;
        const SDK::FVector2D markerCenter = MapRenderToCanvas(*geometries[mapIndex], *renderPosition);
        auto* image = static_cast<SDK::UImage*>(
            SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), totalMap->ImageParent_Canvas));
        if (!image) continue;
        if (chestTexture) SetImageTexture(image, chestTexture);
        SetImageColor(image, chestTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                          : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        auto* slot = totalMap->ImageParent_Canvas->AddChildToCanvas(image);
        if (!slot) continue;
        slot->SetAlignment({0.5f, 0.5f});
        slot->SetPosition(markerCenter);
        slot->SetSize(*markerSizes[mapIndex]);
        // Native treasure widgets have an additional transformed child layer. Match their final visual size with a
        // post-layout correction, independent of which valid native layout-size source was available this frame.
        image->SetRenderTransformPivot({0.5f, 0.5f});
        image->SetRenderScale({0.75f, 0.75f});
        if (auto* renderSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(totalMap->RenderTargetTotal_Image)) {
            slot->SetZOrder(renderSlot->GetZOrder() + 1);
        }
        SetVisible(image);
        spawnedMarkerWidgets.push_back(TrackWidget(image));
        ++rendered;
    }
    return rendered;
}

SDK::UImage* SpawnMapImage(SDK::UObject* owner,
                           SDK::UCanvasPanel* parent,
                           const SDK::FVector2D& position,
                           const SDK::FVector2D& size,
                           SDK::int32 zOrder) {
    auto* image =
        static_cast<SDK::UImage*>(SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), parent));
    if (!image) return nullptr;
    auto* slot = parent->AddChildToCanvas(image);
    if (!slot) return nullptr;
    slot->SetAlignment({0.5f, 0.5f});
    slot->SetPosition(position);
    slot->SetSize(size);
    slot->SetZOrder(zOrder);
    SetVisible(image);
    return image;
}

SDK::UTreasureLocationMinimapBlueprint_C* SpawnNativeMiniMapMarker(
    SDK::UMiniMapBlueprint_C* miniMap,
    const SDK::FVector2D& treasurePanelPosition,
    SDK::UTexture2D* texture,
    const SDK::FLinearColor& color,
    SDK::int32 zOrder) {
    if (!miniMap || !miniMap->Treasure_Panel) return nullptr;

    auto* marker = static_cast<SDK::UTreasureLocationMinimapBlueprint_C*>(SDK::UWidgetBlueprintLibrary::Create(
        miniMap,
        SDK::TSubclassOf<SDK::UUserWidget>(SDK::UTreasureLocationMinimapBlueprint_C::StaticClass()),
        miniMap->GetOwningPlayer()));
    if (!marker) return nullptr;

    SDK::FVector2D markerSize{16.0f, 16.0f};
    SDK::FVector2D markerAlignment{0.5f, 0.5f};
    for (auto* nativeMarker : miniMap->TreasureIconList) {
        auto* nativeSlot = nativeMarker ? SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(nativeMarker) : nullptr;
        if (!nativeSlot) continue;
        const SDK::FVector2D nativeSize = nativeSlot->GetSize();
        if (nativeSize.X > 0.0f && nativeSize.Y > 0.0f) markerSize = nativeSize;
        markerAlignment = nativeSlot->GetAlignment();
        break;
    }

    auto* slot = miniMap->Treasure_Panel->AddChildToCanvas(marker);
    if (!slot) return nullptr;
    slot->SetAlignment(markerAlignment);
    slot->SetPosition(treasurePanelPosition);
    slot->SetSize(markerSize);
    slot->SetAutoSize(false);
    slot->SetZOrder(zOrder);

    if (marker->Image_30) {
        if (texture) SetImageTexture(marker->Image_30, texture);
        SetImageColor(marker->Image_30, color);
        SetVisible(marker->Image_30);
    }
    SetVisible(marker);
    return marker;
}

SDK::UCanvasPanel* CreateMiniMapLayer(SDK::UCanvasPanel* nativeMarkerPanel, SDK::int32 zOrder) {
    if (!nativeMarkerPanel) return nullptr;
    auto* panel = static_cast<SDK::UCanvasPanel*>(
        SDK::UGameplayStatics::SpawnObject(SDK::UCanvasPanel::StaticClass(), nativeMarkerPanel));
    if (!panel) return nullptr;
    auto* slot = nativeMarkerPanel->AddChildToCanvas(panel);
    if (!slot) return nullptr;
    slot->SetAlignment({0.0f, 0.0f});
    slot->SetPosition({0.0f, 0.0f});
    slot->SetSize(SDK::USlateBlueprintLibrary::GetLocalSize(nativeMarkerPanel->GetCachedGeometry()));
    slot->SetZOrder(zOrder);
    panel->SetRenderTransformPivot({0.0f, 0.0f});
    SetVisible(panel);
    return panel;
}

std::optional<SDK::FVector2D> GetRoomMapCenter(SDK::UPBMapComponent* mapComponent,
                                               SDK::UTotalMapBlueprint_C* totalMap,
                                               SDK::EDivideMap mapType,
                                               const bloodstained::tracker::generated::RoomMapData& room) {
    SDK::FVector2D center{};
    std::uint32_t visibleCells = 0;
    const std::uint32_t cellCount = static_cast<std::uint32_t>(room.width) * room.height;
    for (std::uint32_t cell = 0; cell < cellCount; ++cell) {
        if (!Tracker::IsRoomCellVisible(room, cell + 1)) continue;
        const SDK::FVector2D nativeCenter = mapComponent->GetRoomCenterInMapPosition(
            mapType, NameFromString(room.name), static_cast<SDK::int32>(cell + 1), totalMap->IconPixelOffset,
            totalMap->canvasSize);
        center = center + nativeCenter;
        ++visibleCells;
    }
    if (visibleCells == 0) return std::nullopt;
    return center * (1.0f / static_cast<float>(visibleCells));
}

NativeMapAxes FindNativeMiniMapAxes(SDK::UPBMapManager* mapManager,
                                    SDK::UPBMapComponent* mapComponent,
                                    SDK::EDivideMap renderMapType,
                                    SDK::EDivideMap areaMapType,
                                    SDK::UTotalMapBlueprint_C* totalMap) {
    NativeMapAxes axes{{totalMap->RoomPixelSize.X, 0.0f}, {0.0f, totalMap->RoomPixelSize.Y}};
    bool foundX = false;
    bool foundZ = false;
    for (const auto& room : bloodstained::tracker::generated::ROOMS) {
        if (room.out_of_map || (room.width < 2 && room.height < 2)) continue;
        const SDK::FName roomId = NameFromString(room.name);
        if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) continue;
        const SDK::FVector2D first = mapComponent->GetRoomCenterInMapPosition(
            renderMapType, roomId, 1, totalMap->IconPixelOffset, totalMap->canvasSize);
        if (!foundX && room.width >= 2) {
            const SDK::FVector2D nextX = mapComponent->GetRoomCenterInMapPosition(
                renderMapType, roomId, 2, totalMap->IconPixelOffset, totalMap->canvasSize);
            axes.x = nextX - first;
            foundX = std::abs(axes.x.X) + std::abs(axes.x.Y) > 0.01f;
        }
        if (!foundZ && room.height >= 2) {
            const SDK::FVector2D nextZ = mapComponent->GetRoomCenterInMapPosition(
                renderMapType, roomId, room.width + 1, totalMap->IconPixelOffset, totalMap->canvasSize);
            axes.z = nextZ - first;
            foundZ = std::abs(axes.z.X) + std::abs(axes.z.Y) > 0.01f;
        }
        if (foundX && foundZ) break;
    }
    return axes;
}

std::optional<SDK::FVector2D> GetMiniMapLocationPosition(
    SDK::UPBMapManager* mapManager,
    SDK::UPBMapComponent* mapComponent,
    SDK::EDivideMap renderMapType,
    SDK::EDivideMap areaMapType,
    SDK::UTotalMapBlueprint_C* totalMap,
    const NativeMapAxes& axes,
    const bloodstained::tracker::generated::LocationData& location) {
    if (!location.has_map_position) return std::nullopt;
    const auto* room = Tracker::FindRoom(location.room);
    if (!room || room->out_of_map || room->width == 0 || room->height == 0) return std::nullopt;

    const SDK::FName roomId = NameFromString(location.room);
    if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) return std::nullopt;

    const float mapX = std::clamp(location.map_x, 0.0f, static_cast<float>(room->width));
    const float mapZ = GetLocationMarkerMapZ(location, room->height);
    const std::uint32_t cellX = std::min(static_cast<std::uint32_t>(mapX), room->width - 1u);
    const std::uint32_t cellZ = std::min(static_cast<std::uint32_t>(mapZ), room->height - 1u);
    const std::uint32_t assignment = cellZ * room->width + cellX + 1u;
    const SDK::FVector2D cellCenter = mapComponent->GetRoomCenterInMapPosition(
        renderMapType, roomId, static_cast<SDK::int32>(assignment), totalMap->IconPixelOffset,
        totalMap->canvasSize);
    return cellCenter + axes.x * (mapX - static_cast<float>(cellX) - 0.5f) +
           axes.z * (mapZ - static_cast<float>(cellZ) - 0.5f);
}

struct MiniMapAnchor {
    std::string treasureId;
    std::string roomId;
    SDK::FVector worldPosition;
    SDK::FVector2D storedMarkerMap;
    SDK::FVector2D map;
    SDK::FVector2D canvas;
    bool exactRoomPosition = false;
};

struct MiniMapLayerTransform {
    SDK::FVector2D scale;
    SDK::FVector2D translation;
    std::vector<MiniMapAnchor> anchors;
};

float FitMiniMapAxisScale(const std::vector<MiniMapAnchor>& anchors, bool xAxis, float fallback) {
    float sourceMean = 0.0f;
    float targetMean = 0.0f;
    for (const auto& anchor : anchors) {
        sourceMean += xAxis ? anchor.map.X : anchor.map.Y;
        targetMean += xAxis ? anchor.canvas.X : anchor.canvas.Y;
    }
    sourceMean /= static_cast<float>(anchors.size());
    targetMean /= static_cast<float>(anchors.size());

    float covariance = 0.0f;
    float variance = 0.0f;
    for (const auto& anchor : anchors) {
        const float source = (xAxis ? anchor.map.X : anchor.map.Y) - sourceMean;
        const float target = (xAxis ? anchor.canvas.X : anchor.canvas.Y) - targetMean;
        covariance += source * target;
        variance += source * source;
    }
    return variance > 0.01f ? covariance / variance : fallback;
}

float MedianMiniMapValue(std::vector<float> values) {
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values[middle];
}

std::optional<MiniMapLayerTransform> GetNativeMiniMapTransform(SDK::UMiniMapBlueprint_C* miniMap,
                                                               SDK::APBInterfaceHUD* hud,
                                                               const GhostMapGeometry& geometry,
                                                               SDK::UCanvasPanel* targetCanvas = nullptr) {
    auto* totalMap = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    auto* mapManager = miniMap ? miniMap->GetMapManager() : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!totalMap || !mapManager || !mapComponent) return std::nullopt;
    const SDK::FGeometry treasurePanelGeometry =
        miniMap->Treasure_Panel ? miniMap->Treasure_Panel->GetCachedGeometry() : SDK::FGeometry{};
    const SDK::FGeometry targetGeometry = targetCanvas ? targetCanvas->GetCachedGeometry() : SDK::FGeometry{};
    if (targetCanvas) {
        const SDK::FVector2D treasurePanelSize =
            SDK::USlateBlueprintLibrary::GetLocalSize(treasurePanelGeometry);
        const SDK::FVector2D targetSize = SDK::USlateBlueprintLibrary::GetLocalSize(targetGeometry);
        if (treasurePanelSize.X <= 0.0f || treasurePanelSize.Y <= 0.0f || targetSize.X <= 0.0f ||
            targetSize.Y <= 0.0f) {
            return std::nullopt;
        }
    }

    struct NativeTreasureLocation {
        std::string roomId;
        SDK::FVector worldPosition;
        SDK::FVector2D storedMarkerMap;
        SDK::FVector2D renderMap;
    };

    std::unordered_set<std::string> activeTreasureIds;
    activeTreasureIds.reserve(miniMap->TreasureIconList.Num());
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !marker->Image_30 || BORROWED_MINIMAP_MARKERS.contains(marker) ||
            marker->GetParent() != miniMap->Treasure_Panel) {
            continue;
        }
        const auto visibility = marker->GetVisibility();
        const bool active = visibility == SDK::ESlateVisibility::Visible ||
                            visibility == SDK::ESlateVisibility::HitTestInvisible ||
                            visibility == SDK::ESlateVisibility::SelfHitTestInvisible;
        if (active) activeTreasureIds.insert(NormalizeTreasureId(marker->treasureID.ToString()));
    }
    if (activeTreasureIds.empty()) return std::nullopt;

    // Do not call CalcArray here. Although the generated SDK exposes it as public, invoking its reflected wrapper
    // from this post-Tick hook reproduces Bloodstained's startup access violation. Keep the keyed native marker
    // positions as a diagnostic fallback while the m_KeyArray/m_DataArray relationship is decoded using reads only.
    std::unordered_map<std::string, NativeTreasureLocation> nativeTreasureLocations;
    auto* treasureComponent = hud->m_MapTreasureIconComponent;
    if (treasureComponent) {
        std::unordered_map<std::string, SDK::FVector2D> storedMarkerLocations;
        storedMarkerLocations.reserve(activeTreasureIds.size());
        for (const auto& entry : treasureComponent->TreasureMarkerMapLocation) {
            const std::string treasureId = NormalizeTreasureId(entry.Key().ToString());
            if (activeTreasureIds.contains(treasureId))
                storedMarkerLocations.insert_or_assign(treasureId, entry.Value());
        }
        // Retain the keyed marker-space value only for the disabled widget experiment's unmatched-anchor fallback.
        for (const auto& [treasureId, storedMarkerMap] : storedMarkerLocations) {
            nativeTreasureLocations.insert_or_assign(
                treasureId, NativeTreasureLocation{"<unmatched>", {}, storedMarkerMap, storedMarkerMap});
        }
    }

    std::vector<MiniMapAnchor> anchors;
    anchors.reserve(miniMap->TreasureIconList.Num());
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !marker->Image_30 || BORROWED_MINIMAP_MARKERS.contains(marker)) {
            continue;
        }
        const auto visibility = marker->GetVisibility();
        const bool active = visibility == SDK::ESlateVisibility::Visible ||
                            visibility == SDK::ESlateVisibility::HitTestInvisible ||
                            visibility == SDK::ESlateVisibility::SelfHitTestInvisible;
        if (!active) continue;
        const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
        const auto location = nativeTreasureLocations.find(treasureId);
        if (location == nativeTreasureLocations.end()) continue;

        auto* markerSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker);
        if (!markerSlot || marker->GetParent() != miniMap->Treasure_Panel) continue;
        SDK::FVector2D targetPosition{};
        if (targetCanvas) {
            targetPosition = markerSlot->GetPosition();
            const SDK::FVector2D absolutePosition =
                SDK::USlateBlueprintLibrary::LocalToAbsolute(treasurePanelGeometry, targetPosition);
            targetPosition = SDK::USlateBlueprintLibrary::AbsoluteToLocal(targetGeometry, absolutePosition);
        } else {
            // The DX11 overlay consumes desktop pixel coordinates. Learn that transform directly from the rendered
            // centers of native chest images instead of fitting canvas-slot coordinates and converting them through
            // Treasure_Panel a second time. The latter double-applies the minimap's pan/zoom render transform.
            const SDK::FGeometry markerGeometry = marker->Image_30->GetCachedGeometry();
            const SDK::FVector2D markerSize = SDK::USlateBlueprintLibrary::GetLocalSize(markerGeometry);
            if (markerSize.X <= 0.0f || markerSize.Y <= 0.0f) continue;
            SDK::FVector2D viewportPosition{};
            SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, markerGeometry, markerSize * 0.5f,
                                                          &targetPosition, &viewportPosition);
            if (!std::isfinite(targetPosition.X) || !std::isfinite(targetPosition.Y)) continue;
        }
        anchors.push_back(MiniMapAnchor{treasureId, location->second.roomId, location->second.worldPosition,
                                        location->second.storedMarkerMap, location->second.renderMap,
                                        targetPosition, location->second.roomId != "<unmatched>"});
    }
    SDK::FVector2D scale{};
    const bool allAnchorsHaveExactRoomPositions =
        std::all_of(anchors.begin(), anchors.end(), [](const MiniMapAnchor& anchor) {
            return anchor.exactRoomPosition;
        });
    if (!targetCanvas) {
        // GetMiniMapLocationPosition and RenderTargetMini_Image's cached origin both include the live minimap pan.
        // Applying the current origin every tick therefore counts player movement twice. Keep the origin captured
        // for this widget lifetime and let only the native room coordinate supply movement. Re-anchor when the
        // widget is reconstructed or the player changes the minimap zoom enough to alter its pixel basis.
        SDK::FVector2D originPixel{}, originViewport{}, xPixel{}, xViewport{}, yPixel{}, yViewport{};
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {}, &originPixel,
                                                      &originViewport);
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {1.0f, 0.0f}, &xPixel,
                                                      &xViewport);
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {0.0f, 1.0f}, &yPixel,
                                                      &yViewport);
        scale = {xPixel.X - originPixel.X, yPixel.Y - originPixel.Y};
        if (std::abs(scale.X) <= 0.0001f || std::abs(scale.Y) <= 0.0001f) return std::nullopt;
        // Native minimap room coordinates are center-relative. The frame center is the one screen-space origin that
        // follows user HUD repositioning but does not move with player/map pan. It is valid before any teleporter is
        // used and remains invariant under center-based minimap zoom, so no startup or map-state anchor is retained.
        auto* frameImage = miniMap->Frame_Image;
        if (!frameImage) return std::nullopt;
        const SDK::FGeometry frameGeometry = frameImage->GetCachedGeometry();
        const SDK::FVector2D frameSize = SDK::USlateBlueprintLibrary::GetLocalSize(frameGeometry);
        if (frameSize.X <= 0.0f || frameSize.Y <= 0.0f) return std::nullopt;
        SDK::FVector2D frameCenterPixel{}, frameCenterViewport{};
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, frameGeometry, frameSize * 0.5f,
                                                      &frameCenterPixel, &frameCenterViewport);
        return MiniMapLayerTransform{scale, frameCenterPixel, std::move(anchors)};
    }

    if (anchors.empty()) return std::nullopt;

    if (!allAnchorsHaveExactRoomPositions) {
        // Slot-space synchronization is retained only for the disabled widget experiment. A single native anchor
        // cannot establish scale in that space, where Treasure_Panel normally supplies the pan/zoom transform. The
        // same legacy fit is deliberately retained for unmatched diagnostic anchors because their keyed marker-map
        // coordinates are not in render-target space.
        scale = {FitMiniMapAxisScale(anchors, true, 1.0f), FitMiniMapAxisScale(anchors, false, 1.0f)};
    } else {
        // GetInMapPosition and GetRoomCenterInMapPosition return coordinates in the mini render target's local
        // space. Measure one local unit in desktop pixels every tick; this automatically incorporates Slate DPI,
        // the player's minimap-zoom setting and any live Blueprint render transform. It also works with only one
        // visible chest, whereas a regression fit would otherwise fall back to the incorrect scale 1.
        SDK::FVector2D originPixel{}, originViewport{}, xPixel{}, xViewport{}, yPixel{}, yViewport{};
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {}, &originPixel,
                                                      &originViewport);
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {1.0f, 0.0f}, &xPixel,
                                                      &xViewport);
        SDK::USlateBlueprintLibrary::LocalToViewport(miniMap, geometry.renderTarget, {0.0f, 1.0f}, &yPixel,
                                                      &yViewport);
        scale = {xPixel.X - originPixel.X, yPixel.Y - originPixel.Y};
    }
    if (std::abs(scale.X) <= 0.0001f || std::abs(scale.Y) <= 0.0001f) return std::nullopt;

    std::vector<float> xTranslations;
    std::vector<float> yTranslations;
    xTranslations.reserve(anchors.size());
    yTranslations.reserve(anchors.size());
    for (const auto& anchor : anchors) {
        xTranslations.push_back(anchor.canvas.X - anchor.map.X * scale.X);
        yTranslations.push_back(anchor.canvas.Y - anchor.map.Y * scale.Y);
    }
    // Opening a chest changes which native widgets are active. A median keeps a single mismatched registration or
    // transiently stale widget from shifting every generated marker when that active set changes.
    const SDK::FVector2D translation{MedianMiniMapValue(std::move(xTranslations)),
                                     MedianMiniMapValue(std::move(yTranslations))};
    return MiniMapLayerTransform{scale, translation, std::move(anchors)};
}

SDK::FVector2D TransformMiniMapPosition(const SDK::FVector2D& position,
                                        const MiniMapLayerTransform& transform) {
    return {position.X * transform.scale.X + transform.translation.X,
            position.Y * transform.scale.Y + transform.translation.Y};
}

SDK::FVector2D TransformMiniMapSize(const SDK::FVector2D& size,
                                    const MiniMapLayerTransform& transform) {
    return {std::abs(size.X * transform.scale.X), std::abs(size.Y * transform.scale.Y)};
}

struct ScreenRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

bool IsWidgetPaintVisible(SDK::UWidget* widget) {
    if (!widget) return false;
    const auto visibility = widget->GetVisibility();
    return visibility == SDK::ESlateVisibility::Visible ||
           visibility == SDK::ESlateVisibility::HitTestInvisible ||
           visibility == SDK::ESlateVisibility::SelfHitTestInvisible;
}

std::optional<ScreenRect> GetWidgetScreenRect(SDK::UObject* worldContext, SDK::UWidget* widget) {
    if (!worldContext || !widget) return std::nullopt;
    const SDK::FGeometry geometry = widget->GetCachedGeometry();
    const SDK::FVector2D localSize = SDK::USlateBlueprintLibrary::GetLocalSize(geometry);
    if (localSize.X <= 0.0f || localSize.Y <= 0.0f) return std::nullopt;

    SDK::FVector2D topLeftPixel{};
    SDK::FVector2D topLeftViewport{};
    SDK::FVector2D bottomRightPixel{};
    SDK::FVector2D bottomRightViewport{};
    SDK::USlateBlueprintLibrary::LocalToViewport(worldContext, geometry, {}, &topLeftPixel, &topLeftViewport);
    SDK::USlateBlueprintLibrary::LocalToViewport(worldContext, geometry, localSize, &bottomRightPixel,
                                                  &bottomRightViewport);
    ScreenRect result{std::min(topLeftPixel.X, bottomRightPixel.X),
                      std::min(topLeftPixel.Y, bottomRightPixel.Y),
                      std::max(topLeftPixel.X, bottomRightPixel.X),
                      std::max(topLeftPixel.Y, bottomRightPixel.Y)};
    if (result.Width() <= 0.0f || result.Height() <= 0.0f) return std::nullopt;
    return result;
}

std::optional<SDK::FVector2D> LocalToScreenPixel(SDK::UObject* worldContext,
                                                  const SDK::FGeometry& geometry,
                                                  const SDK::FVector2D& localPosition) {
    if (!worldContext) return std::nullopt;
    SDK::FVector2D pixelPosition{};
    SDK::FVector2D viewportPosition{};
    SDK::USlateBlueprintLibrary::LocalToViewport(worldContext, geometry, localPosition, &pixelPosition,
                                                  &viewportPosition);
    if (!std::isfinite(pixelPosition.X) || !std::isfinite(pixelPosition.Y)) return std::nullopt;
    return pixelPosition;
}

SDK::FVector2D GetNativeMiniMapMarkerSizeInCanvas(SDK::UMiniMapBlueprint_C* miniMap,
                                                  SDK::UCanvasPanel* targetCanvas) {
    if (!miniMap || !miniMap->Treasure_Panel) return {16.0f, 16.0f};
    const SDK::FGeometry treasureGeometry = miniMap->Treasure_Panel->GetCachedGeometry();
    const SDK::FGeometry targetGeometry = targetCanvas ? targetCanvas->GetCachedGeometry() : SDK::FGeometry{};
    for (auto* marker : miniMap->TreasureIconList) {
        auto* slot = marker ? SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker) : nullptr;
        if (!slot) continue;
        const SDK::FVector2D nativeSize = slot->GetSize();
        if (nativeSize.X <= 0.0f || nativeSize.Y <= 0.0f) continue;
        if (!targetCanvas) return nativeSize;
        const SDK::FVector2D absoluteSize =
            SDK::USlateBlueprintLibrary::TransformVectorLocalToAbsolute(treasureGeometry, nativeSize);
        const SDK::FVector2D canvasSize =
            SDK::USlateBlueprintLibrary::TransformVectorAbsoluteToLocal(targetGeometry, absoluteSize);
        if (canvasSize.X > 0.0f && canvasSize.Y > 0.0f) return canvasSize;
    }
    return {16.0f, 16.0f};
}

SDK::FVector2D GetMiniMapOverlayMarkerSize(const MiniMapLayerTransform& transform) {
    // The embedded 96x96 image has a visible glyph spanning roughly two thirds of its transparent canvas. A 40.5
    // render-target-unit canvas therefore produces the same approximately 27-unit visible marker as native icons.
    return {40.5f * std::abs(transform.scale.X), 40.5f * std::abs(transform.scale.Y)};
}

SDK::FVector2D GetMiniMapShardOverlayMarkerSize(const MiniMapLayerTransform& transform) {
    // Compare the solid glyphs rather than faint antialiasing/glow pixels: the shard fills 89x86 pixels of its
    // canvas while the chest fills 32x26. Scaling by those bounds gives both markers the same visible footprint.
    return {40.5f * (32.0f / 89.0f) * std::abs(transform.scale.X),
            40.5f * (26.0f / 86.0f) * std::abs(transform.scale.Y)};
}

SDK::FVector2D GetMiniMapWallOverlayMarkerSize(const MiniMapLayerTransform& transform) {
    // The solid wall glyph fills 58x59 pixels versus 32x26 for the chest glyph.
    return {40.5f * (32.0f / 58.0f) * std::abs(transform.scale.X),
            40.5f * (26.0f / 59.0f) * std::abs(transform.scale.Y)};
}

struct MiniMapRenderResult {
    bool anchored = false;
    std::size_t anchors = 0;
    std::size_t ghostCells = 0;
    std::size_t chests = 0;
    std::size_t walls = 0;
    std::size_t shards = 0;
};

enum class MiniMapMarkerKind {
    WALL,
    SHARD,
};

struct NativeMiniMapMarkerSnapshot {
    bool valid = false;
    SDK::FName treasureId;
    SDK::ESlateVisibility markerVisibility = SDK::ESlateVisibility::Collapsed;
    SDK::ESlateVisibility imageVisibility = SDK::ESlateVisibility::Collapsed;
    float markerOpacity = 1.0f;
    float imageOpacity = 1.0f;
    SDK::FWidgetTransform markerTransform{};
    SDK::FWidgetTransform imageTransform{};
    SDK::FVector2D markerPivot{};
    SDK::FVector2D imagePivot{};
    SDK::FSlateBrush brush{};
    SDK::FLinearColor color{};
    SDK::FAnchorData layout{};
    bool autoSize = false;
    SDK::int32 zOrder = 0;
};

struct MiniMapMarkerBinding {
    std::string key;
    MiniMapMarkerKind kind = MiniMapMarkerKind::WALL;
    const bloodstained::tracker::generated::LocationData* wallLocation = nullptr;
    std::string shardRoom;
    TrackedWidgetHandle marker;
    TrackedWidgetHandle overlayImage;
    bool borrowedNativeMarker = false;
    NativeMiniMapMarkerSnapshot nativeSnapshot;
    SDK::FSlateBrush desiredBrush{};
    SDK::FLinearColor desiredColor{};
    SDK::FVector2D desiredSize{16.0f, 16.0f};
    SDK::FVector2D desiredAlignment{0.5f, 0.5f};
    bool desiredAutoSize = false;
    SDK::int32 zeroGeometryTicks = 0;
};

std::vector<MiniMapMarkerBinding> MINI_MAP_MARKER_BINDINGS;
std::unordered_set<std::string> MINI_MAP_PROTECTED_TREASURE_IDS;

SDK::UTreasureLocationMinimapBlueprint_C* ResolveMiniMapMarker(const TrackedWidgetHandle& handle) {
    return static_cast<SDK::UTreasureLocationMinimapBlueprint_C*>(ResolveWidget(handle));
}

NativeMiniMapMarkerSnapshot SnapshotNativeMiniMapMarker(
    SDK::UTreasureLocationMinimapBlueprint_C* marker) {
    NativeMiniMapMarkerSnapshot snapshot;
    if (!marker || !marker->Image_30) return snapshot;
    auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker);
    if (!slot) return snapshot;

    snapshot.valid = true;
    snapshot.treasureId = marker->treasureID;
    snapshot.markerVisibility = marker->GetVisibility();
    snapshot.imageVisibility = marker->Image_30->GetVisibility();
    snapshot.markerOpacity = marker->GetRenderOpacity();
    snapshot.imageOpacity = marker->Image_30->GetRenderOpacity();
    snapshot.markerTransform = marker->RenderTransform;
    snapshot.imageTransform = marker->Image_30->RenderTransform;
    snapshot.markerPivot = marker->RenderTransformPivot;
    snapshot.imagePivot = marker->Image_30->RenderTransformPivot;
    snapshot.brush = marker->Image_30->Brush;
    snapshot.color = marker->Image_30->ColorAndOpacity;
    snapshot.layout = slot->GetLayout();
    snapshot.autoSize = slot->GetAutoSize();
    snapshot.zOrder = slot->GetZOrder();
    return snapshot;
}

void RestoreNativeMiniMapMarker(MiniMapMarkerBinding& binding, bool preserveNativeLayout = false) {
    if (!binding.borrowedNativeMarker || !binding.nativeSnapshot.valid) return;
    auto* marker = ResolveMiniMapMarker(binding.marker);
    if (!marker || !marker->Image_30) return;
    BORROWED_MINIMAP_MARKERS.erase(marker);

    const auto& snapshot = binding.nativeSnapshot;
    marker->treasureID = snapshot.treasureId;
    marker->SetRenderOpacity(snapshot.markerOpacity);
    marker->Image_30->SetRenderOpacity(snapshot.imageOpacity);
    marker->SetRenderTransform(snapshot.markerTransform);
    marker->Image_30->SetRenderTransform(snapshot.imageTransform);
    marker->SetRenderTransformPivot(snapshot.markerPivot);
    marker->Image_30->SetRenderTransformPivot(snapshot.imagePivot);
    SetImageBrush(marker->Image_30, snapshot.brush);
    SetImageColor(marker->Image_30, snapshot.color);
    if (!preserveNativeLayout) {
        SetWidgetVisibility(marker, snapshot.markerVisibility);
        SetWidgetVisibility(marker->Image_30, snapshot.imageVisibility);
        if (auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker)) {
            slot->SetLayout(snapshot.layout);
            slot->SetAutoSize(snapshot.autoSize);
            slot->SetZOrder(snapshot.zOrder);
        }
    }
}

void ClearMiniMapMarkerBindings() {
    for (auto& binding : MINI_MAP_MARKER_BINDINGS) RestoreNativeMiniMapMarker(binding);
    BORROWED_MINIMAP_MARKERS.clear();
    MINI_MAP_MARKER_BINDINGS.clear();
    MINI_MAP_PROTECTED_TREASURE_IDS.clear();
}

bool IsMarkerAlreadyBorrowed(SDK::UTreasureLocationMinimapBlueprint_C* marker) {
    for (const auto& binding : MINI_MAP_MARKER_BINDINGS) {
        if (binding.borrowedNativeMarker && ResolveMiniMapMarker(binding.marker) == marker) return true;
    }
    return false;
}

bool BorrowInactiveNativeMarker(SDK::UMiniMapBlueprint_C* miniMap, MiniMapMarkerBinding& binding) {
    if (!ENABLE_NATIVE_MINIMAP_MARKER_POOL) return false;
    if (!miniMap || !miniMap->Treasure_Panel) return false;
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !marker->Image_30 || marker->GetParent() != miniMap->Treasure_Panel ||
            IsMarkerAlreadyBorrowed(marker) ||
            MINI_MAP_PROTECTED_TREASURE_IDS.contains(NormalizeTreasureId(marker->treasureID.ToString())) ||
            marker->IsVisible()) {
            continue;
        }
        auto snapshot = SnapshotNativeMiniMapMarker(marker);
        if (!snapshot.valid) continue;

        if (auto* detached = ResolveMiniMapMarker(binding.marker)) {
            if (!binding.borrowedNativeMarker) detached->RemoveFromParent();
        }
        binding.marker = TrackWidget(marker);
        binding.borrowedNativeMarker = true;
        BORROWED_MINIMAP_MARKERS.insert(marker);
        binding.nativeSnapshot = snapshot;
        binding.zeroGeometryTicks = 0;
        return true;
    }

    return false;
}

MiniMapRenderResult RenderMiniMapTracker(
    SDK::UMiniMapBlueprint_C* miniMap,
    const std::unordered_set<std::string>& reachableRooms,
    const std::vector<const bloodstained::tracker::generated::LocationData*>& reachableLocations,
    const std::unordered_set<std::string>& reachableShardRooms,
    std::vector<TrackedWidgetHandle>& activatedMarkerWidgets,
    std::vector<TrackedWidgetHandle>& visibilityWidgets,
    std::vector<TrackedWidgetHandle>& spawnedWidgets,
    TrackedWidgetHandle* ghostPanelOut,
    TrackedWidgetHandle* iconPanelOut) {
    MiniMapRenderResult result;
    auto* totalMap = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    auto* mapManager = miniMap ? miniMap->GetMapManager() : nullptr;
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!totalMap || !totalMap->ImageParent_Canvas || !miniMap->Custom_Marker_Panel || !mapManager ||
        !mapComponent) return result;

    const SDK::EDivideMap renderMapType = totalMap->mapType;
    const SDK::EDivideMap areaMapType = mapComponent->CheckMapType(mapManager->GetCurrentAreaId());

    auto geometry = GetMiniMapGeometry(totalMap);
    if (!geometry) return result;
    geometry->markerAnchorCorrection = {};
    const auto layerTransform =
        GetNativeMiniMapTransform(miniMap, hud, *geometry, miniMap->Custom_Marker_Panel);
    if (!layerTransform) return result;
    result.anchored = true;
    result.anchors = layerTransform->anchors.size();

    SDK::int32 markerZOrder = 1;
    for (auto* marker : miniMap->CustomMarkerIconList) {
        if (auto* markerSlot = marker ? SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker) : nullptr) {
            markerZOrder = std::max(markerZOrder, markerSlot->GetZOrder() + 1);
        }
    }

    auto* ghostPanel = CreateMiniMapLayer(miniMap->Treasure_Panel, -1);
    if (!ghostPanel) {
        if (ghostPanel) ghostPanel->RemoveFromParent();
        return result;
    }
    *ghostPanelOut = TrackWidget(ghostPanel);
    *iconPanelOut = {};

    // The minimap reachability grid remains disabled. Generated collectibles use independent images on the
    // Blueprint's Custom_Marker_Panel without modifying either native-owned marker array.

    std::unordered_set<std::string> treasureIds;
    std::unordered_set<std::uint64_t> wallLocationIds;
    for (const auto* location : reachableLocations) {
        if (location->type == LocationType::CHEST) {
            const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
            if (nativeLocationName) treasureIds.insert(NormalizeTreasureId(std::string(*nativeLocationName)));
        } else if (location->type == LocationType::WALL) {
            wallLocationIds.insert(location->id);
        }
    }
    MINI_MAP_PROTECTED_TREASURE_IDS = treasureIds;
    auto* chestTexture =
        treasureIds.empty() ? nullptr : CreateEmbeddedTexture(miniMap, IDR_CHEST_MARKER_PNG, "chest marker");
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !treasureIds.contains(NormalizeTreasureId(marker->treasureID.ToString()))) continue;
        SetVisible(marker);
        SetVisible(marker->Image_30);
        activatedMarkerWidgets.push_back(TrackWidget(marker->Image_30));
        visibilityWidgets.push_back(TrackWidget(marker));
        visibilityWidgets.push_back(TrackWidget(marker->Image_30));
        if (chestTexture) SetImageTexture(marker->Image_30, chestTexture);
        SetImageColor(marker->Image_30, chestTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                    : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        ++result.chests;
    }

    const NativeMapAxes axes =
        FindNativeMiniMapAxes(mapManager, mapComponent, renderMapType, areaMapType, totalMap);
    const SDK::FVector2D generatedMarkerSize =
        GetNativeMiniMapMarkerSizeInCanvas(miniMap, miniMap->Custom_Marker_Panel);
    auto* wallTexture = wallLocationIds.empty()
                            ? nullptr
                            : CreateEmbeddedTexture(miniMap, IDR_WALL_MARKER_PNG, "wall marker");
    for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
        if (!wallLocationIds.contains(location.id)) continue;
        const SDK::FName roomId = NameFromString(location.room);
        if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) continue;
        const auto markerCenter = GetMiniMapLocationPosition(mapManager, mapComponent, renderMapType,
                                                             areaMapType, totalMap, axes, location);
        if (!markerCenter) continue;

        auto* image = SpawnMapImage(miniMap, miniMap->Custom_Marker_Panel,
                                    TransformMiniMapPosition(*markerCenter, *layerTransform),
                                    generatedMarkerSize, markerZOrder);
        if (!image) continue;
        if (wallTexture) SetImageTexture(image, wallTexture);
        const SDK::FLinearColor markerColor =
            wallTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                        : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f};
        SetImageColor(image, markerColor);
        image->Brush.DrawAs = SDK::ESlateBrushDrawType::Image;
        image->Brush.ImageSize = generatedMarkerSize;
        SetImageBrush(image, image->Brush);
        SynchronizeDynamicImage(image);
        const auto imageHandle = TrackWidget(image);
        spawnedWidgets.push_back(imageHandle);
        MiniMapMarkerBinding binding;
        binding.key = std::string("wall:") + std::string(location.name);
        binding.kind = MiniMapMarkerKind::WALL;
        binding.wallLocation = &location;
        binding.overlayImage = imageHandle;
        binding.desiredColor = markerColor;
        binding.desiredBrush = image->Brush;
        if (auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(image)) {
            binding.desiredSize = slot->GetSize();
            binding.desiredAlignment = slot->GetAlignment();
            binding.desiredAutoSize = slot->GetAutoSize();
        }
        MINI_MAP_MARKER_BINDINGS.push_back(std::move(binding));
        ++result.walls;
    }

    auto* shardMarkerTexture =
        reachableShardRooms.empty() ? nullptr : CreateEmbeddedTexture(miniMap, IDR_SHARD_MARKER_PNG, "shard marker");
    for (const std::string& roomName : reachableShardRooms) {
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map) continue;
        const SDK::FName roomId = NameFromString(roomName);
        if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) continue;
        const auto markerCenter = GetRoomMapCenter(mapComponent, totalMap, renderMapType, *room);
        if (!markerCenter) continue;
        auto* image = SpawnMapImage(miniMap, miniMap->Custom_Marker_Panel,
                                    TransformMiniMapPosition(*markerCenter, *layerTransform),
                                    generatedMarkerSize, markerZOrder);
        if (!image) continue;
        if (shardMarkerTexture) SetImageTexture(image, shardMarkerTexture);
        const SDK::FLinearColor markerColor = shardMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                                : SDK::FLinearColor{0.2f, 0.9f, 0.35f, 1.0f};
        SetImageColor(image, markerColor);
        image->Brush.DrawAs = SDK::ESlateBrushDrawType::Image;
        image->Brush.ImageSize = generatedMarkerSize;
        SetImageBrush(image, image->Brush);
        SynchronizeDynamicImage(image);
        const auto imageHandle = TrackWidget(image);
        spawnedWidgets.push_back(imageHandle);
        MiniMapMarkerBinding binding;
        binding.key = std::string("shard:") + roomName;
        binding.kind = MiniMapMarkerKind::SHARD;
        binding.shardRoom = roomName;
        binding.overlayImage = imageHandle;
        binding.desiredColor = markerColor;
        binding.desiredBrush = image->Brush;
        if (auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(image)) {
            binding.desiredSize = slot->GetSize();
            binding.desiredAlignment = slot->GetAlignment();
            binding.desiredAutoSize = slot->GetAutoSize();
        }
        MINI_MAP_MARKER_BINDINGS.push_back(std::move(binding));
        ++result.shards;
    }

    return result;
}

bool SynchronizeMiniMapMarkers(SDK::UMiniMapBlueprint_C* miniMap) {
    auto* totalMap = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    auto* mapManager = miniMap ? miniMap->GetMapManager() : nullptr;
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!miniMap || !miniMap->Treasure_Panel || !miniMap->Custom_Marker_Panel || !totalMap ||
        !totalMap->ImageParent_Canvas || !mapManager || !mapComponent) {
        return false;
    }

    auto geometry = GetMiniMapGeometry(totalMap);
    if (!geometry) return false;
    geometry->markerAnchorCorrection = {};
    const auto layerTransform =
        GetNativeMiniMapTransform(miniMap, hud, *geometry, miniMap->Custom_Marker_Panel);
    if (!layerTransform) return false;

    const SDK::EDivideMap renderMapType = totalMap->mapType;
    const SDK::EDivideMap areaMapType = mapComponent->CheckMapType(mapManager->GetCurrentAreaId());
    const NativeMapAxes axes =
        FindNativeMiniMapAxes(mapManager, mapComponent, renderMapType, areaMapType, totalMap);

    SDK::int32 markerZOrder = 1;
    for (auto* marker : miniMap->CustomMarkerIconList) {
        if (auto* markerSlot = marker ? SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker) : nullptr) {
            markerZOrder = std::max(markerZOrder, markerSlot->GetZOrder() + 1);
        }
    }
    const SDK::FVector2D markerSize =
        GetNativeMiniMapMarkerSizeInCanvas(miniMap, miniMap->Custom_Marker_Panel);

    for (auto& binding : MINI_MAP_MARKER_BINDINGS) {
        auto* image = static_cast<SDK::UImage*>(ResolveWidget(binding.overlayImage));
        if (!image || image->GetParent() != miniMap->Custom_Marker_Panel) return false;

        std::optional<SDK::FVector2D> mapPosition;
        if (binding.kind == MiniMapMarkerKind::WALL && binding.wallLocation) {
            mapPosition = GetMiniMapLocationPosition(mapManager, mapComponent, renderMapType, areaMapType,
                                                     totalMap, axes, *binding.wallLocation);
        } else if (binding.kind == MiniMapMarkerKind::SHARD) {
            const auto* room = Tracker::FindRoom(binding.shardRoom);
            if (room && !room->out_of_map &&
                mapComponent->CheckMapType(mapManager->RoomIdToAreaId(NameFromString(room->name))) == areaMapType) {
                mapPosition = GetRoomMapCenter(mapComponent, totalMap, renderMapType, *room);
            }
        }
        if (!mapPosition) {
            SetCollapsed(image);
            continue;
        }

        auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(image);
        if (!slot) return false;
        slot->SetAlignment({0.5f, 0.5f});
        slot->SetPosition(TransformMiniMapPosition(*mapPosition, *layerTransform));
        slot->SetSize(markerSize.X > 0.0f && markerSize.Y > 0.0f ? markerSize : binding.desiredSize);
        slot->SetAutoSize(false);
        slot->SetZOrder(markerZOrder);
        image->SetRenderOpacity(1.0f);
        SetImageBrush(image, binding.desiredBrush);
        SetImageColor(image, binding.desiredColor);
        SetVisible(image);
        if (binding.zeroGeometryTicks < 3) {
            SynchronizeDynamicImage(image);
            miniMap->ForceLayoutPrepass();
            const SDK::FVector2D laidOutSize =
                SDK::USlateBlueprintLibrary::GetLocalSize(image->GetCachedGeometry());
            if (laidOutSize.X <= 0.0f || laidOutSize.Y <= 0.0f) {
                ++binding.zeroGeometryTicks;
            } else {
                binding.zeroGeometryTicks = 3;
            }
        }
    }

    return true;
}

}  // namespace

InGameTracker& InGameTracker::Instance() {
    static InGameTracker instance;
    return instance;
}

MiniMapOverlaySnapshot InGameTracker::GetMiniMapOverlaySnapshot() const {
    const std::lock_guard lock(miniMapOverlayMutex_);
    return miniMapOverlaySnapshot_;
}

void InGameTracker::InvalidateReachability(std::string_view reason) {
    if (!reachabilityDirty_) Logger::Log("[Tracker] Invalidated logical traversal:", std::string(reason));
    reachabilityDirty_ = true;
    miniMapDirty_ = true;
}

void InGameTracker::MarkInventorySynchronized() {
    if (inventorySynchronized_) return;
    inventorySynchronized_ = true;
    InvalidateReachability("AP inventory synchronized");
}

void InGameTracker::ObserveNativeItem(std::string_view itemName) {
    if (Tracker::IsTraversalItem(itemName)) InvalidateReachability("native progression item acquired");
}

void InGameTracker::ObserveLocationCleared(std::string_view locationName) {
    miniMapDirty_ = true;
    ReconcileFinishedTreasureMarkers(Archipelago::ConnectedInstance());
}

void InGameTracker::ResetConnection() {
    ClearMainMapMarkers();
    ClearMiniMapMarkers();
    inventorySynchronized_ = false;
    reachabilityDirty_ = true;
    pendingGhostMap_ = nullptr;
    pendingWallLocationIds_.clear();
    reachableRooms_.clear();
    RECONCILED_TREASURE_COMPONENT_INDEX = -1;
    RECONCILED_FINISHED_TREASURE_IDS.clear();
    miniMapDirty_ = true;
}

bool InGameTracker::IsMainMapEnabled() const {
    const auto mode = displayMode_.load();
    return mode == TrackerDisplayMode::MAIN_MAP || mode == TrackerDisplayMode::FULL;
}

bool InGameTracker::IsMiniMapEnabled() const {
    const auto mode = displayMode_.load();
    return mode == TrackerDisplayMode::MINI_MAP || mode == TrackerDisplayMode::FULL;
}

void InGameTracker::LoadDisplayMode() {
    SDK::int32 savedMode = static_cast<SDK::int32>(TrackerDisplayMode::FULL);
    bool hasSavedMode = false;
    SDK::UPBGameInstance::GetSavedValue(NameFromString(TRACKER_DISPLAY_MODE_SAVE_KEY), &savedMode, &hasSavedMode);
    if (!hasSavedMode || savedMode < static_cast<SDK::int32>(TrackerDisplayMode::NONE) ||
        savedMode > static_cast<SDK::int32>(TrackerDisplayMode::FULL)) {
        savedMode = static_cast<SDK::int32>(TrackerDisplayMode::FULL);
    }
    displayMode_.store(static_cast<TrackerDisplayMode>(savedMode));
    miniMapDirty_ = true;
    Logger::Log("[Tracker] Loaded in-game tracking display mode:", savedMode);
}

void InGameTracker::SetDisplayMode(TrackerDisplayMode mode) {
    const auto modeValue = static_cast<SDK::int32>(mode);
    if (modeValue < static_cast<SDK::int32>(TrackerDisplayMode::NONE) ||
        modeValue > static_cast<SDK::int32>(TrackerDisplayMode::FULL)) {
        return;
    }
    displayMode_.store(mode);
    SDK::UPBGameInstance::SetSavedValue(NameFromString(TRACKER_DISPLAY_MODE_SAVE_KEY), modeValue);
    if (!IsMainMapEnabled()) ClearMainMapMarkers();
    if (!IsMiniMapEnabled()) ClearMiniMapMarkers();
    miniMapDirty_ = true;
    Logger::Log("[Tracker] Set in-game tracking display mode:", modeValue);
}

void InGameTracker::ClearMainMapMarkers() {
    for (const auto& ghostWidget : spawnedMainMapGhostWidgets_) {
        if (auto* widget = ResolveWidget(ghostWidget)) widget->RemoveFromParent();
    }
    spawnedMainMapGhostWidgets_.clear();

    // Event_MapStart has already refreshed the native marker state before ApplyMapMarkers runs. Restore only the
    // image resource that the tracker replaced; hiding the widget here would overwrite the game's newly refreshed
    // opened/closed state and can leave a cleared chest carrying the tracker's green brush.
    RestoreNativeImageBrushes(MAIN_MAP_NATIVE_BRUSH_OVERRIDES);
    activatedNativeMarkerWidgets_.clear();

    for (const auto& markerWidget : spawnedWallMarkerWidgets_) {
        if (auto* widget = ResolveWidget(markerWidget)) widget->RemoveFromParent();
    }
    spawnedWallMarkerWidgets_.clear();
    pendingSyntheticTreasureIds_.clear();
}

void InGameTracker::ClearMiniMapMarkers() {
    {
        const std::lock_guard lock(miniMapOverlayMutex_);
        miniMapOverlaySnapshot_ = {};
    }
    miniMapWallLocationIds_.clear();
    miniMapShardRooms_.clear();
    miniMapTreasureIds_.clear();
    ClearMiniMapMarkerBindings();
    miniMapVisibilityWidgets_.clear();

    RestoreNativeImageBrushes(MINI_MAP_NATIVE_BRUSH_OVERRIDES);
    activatedMiniMapMarkerWidgets_.clear();

    for (const auto& markerWidget : spawnedMiniMapWidgets_) {
        if (auto* widget = ResolveWidget(markerWidget)) widget->RemoveFromParent();
    }
    spawnedMiniMapWidgets_.clear();

    if (auto* ghostPanel = ResolveWidget(miniMapGhostPanel_)) ghostPanel->RemoveFromParent();
    miniMapGhostPanel_ = {};
    if (auto* iconPanel = ResolveWidget(miniMapIconPanel_)) iconPanel->RemoveFromParent();
    miniMapIconPanel_ = {};
}

void InGameTracker::ApplyDeferredGhostMap(void* mapWidget) {
    if (pendingGhostMap_ != mapWidget) return;

    auto* map = static_cast<SDK::UMapManageBlueprint_C*>(mapWidget);
    if (!IsMainMapEnabled() || !Archipelago::ConnectedInstance() || !inventorySynchronized_) {
        pendingGhostMap_ = nullptr;
        pendingWallLocationIds_.clear();
        pendingSyntheticTreasureIds_.clear();
        return;
    }
    if (!HasLaidOutMap(map)) return;

    RenderReachableRoomGhosts(map, reachableRooms_, spawnedMainMapGhostWidgets_);
    RenderWallLocationMarkers(map, pendingWallLocationIds_, spawnedWallMarkerWidgets_);
    RenderSyntheticTreasureMarkers(map, pendingSyntheticTreasureIds_, spawnedWallMarkerWidgets_);
    pendingGhostMap_ = nullptr;
    pendingWallLocationIds_.clear();
    pendingSyntheticTreasureIds_.clear();
}

void InGameTracker::ApplyMapMarkers(void* mapWidget) {
    auto* archipelago = Archipelago::ConnectedInstance();
    auto* map = static_cast<SDK::UMapManageBlueprint_C*>(mapWidget);
    ReconcileFinishedTreasureMarkers(archipelago);
    ClearMainMapMarkers();
    if (!IsMainMapEnabled() || !archipelago || !map) return;
    pendingWallLocationIds_.clear();
    pendingSyntheticTreasureIds_.clear();

    Tracker tracker;
    auto traversalInventory = archipelago->GetTrackerInventory();
    for (const auto& nativeItem : bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS) {
        if (GameManager::Instance().CheckAllInventories(std::string(nativeItem.native_id))) {
            traversalInventory[std::string(nativeItem.item_name)] =
                std::max(traversalInventory[std::string(nativeItem.item_name)], 1u);
        }
    }
    tracker.SetInventory(traversalInventory);
    const Difficulty difficulty = GetDifficulty();
    if (inventorySynchronized_) {
        if (reachabilityDirty_) {
            reachableRooms_.clear();
            for (const auto* room : tracker.GetReachableRooms(difficulty)) {
                if (!room->out_of_map) reachableRooms_.insert(std::string(room->name));
            }
            reachabilityDirty_ = false;
            Logger::Log("[Tracker] Recomputed logical traversal; reachable map rooms:", reachableRooms_.size());
        }
        pendingGhostMap_ = map;
    }
    const auto reachableLocations =
        tracker.GetReachableMissingLocations(difficulty, archipelago->GetMissingLocationIds());

    std::unordered_set<std::string> treasureIds;
    std::string reachableTreasureLocations;
    std::unordered_set<std::string> markedRooms;
    std::unordered_map<std::string, std::string> enemyByRoom;
    for (const auto* location : reachableLocations) {
        const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
        if (!nativeLocationName ||
            archipelago->WasLocationClearedLocally(std::string(*nativeLocationName)) ||
            !archipelago->IsMissingLocation(std::string(*nativeLocationName), location->id)) {
            continue;
        }
        if (location->type == LocationType::CHEST) {
            treasureIds.insert(NormalizeTreasureId(std::string(*nativeLocationName)));
            if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
                if (!reachableTreasureLocations.empty()) reachableTreasureLocations += ", ";
                reachableTreasureLocations += location->name;
            }
        } else if (location->type == LocationType::WALL) {
            pendingWallLocationIds_.insert(location->id);
        } else if (location->type == LocationType::ENEMY) {
            std::string enemyId(nativeLocationName->substr(
                0, nativeLocationName->size() - std::string_view("_Shard").size()));
            for (const std::string_view room : tracker.GetReachableEnemyRooms(*location, difficulty)) {
                markedRooms.insert(std::string(room));
                enemyByRoom.insert_or_assign(std::string(room), enemyId);
            }
        }
    }

    std::unordered_set<std::string> materializedTreasureIds;
    std::string markedTreasureIds;
    auto* chestMarkerTexture =
        treasureIds.empty() ? nullptr : CreateEmbeddedTexture(map, IDR_CHEST_MARKER_PNG, "chest marker");
    for (auto* marker : map->TreasureMarkerList) {
        if (!marker || !marker->Image_30) continue;
        const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
        if (!treasureIds.contains(treasureId)) continue;
        // Some large rooms snap several native treasure widgets onto coarse shared map cells. When the cooked actor
        // position is known, use the generated marker at that exact position and suppress the misleading native one.
        if (HasAuthoritativeChestPosition(treasureId)) {
            SetCollapsed(marker);
            if (marker->Image_30) SetCollapsed(marker->Image_30);
            continue;
        }
        RememberNativeImageBrush(MAIN_MAP_NATIVE_BRUSH_OVERRIDES, marker->Image_30);
        SetVisible(marker);
        SetVisible(marker->Image_30);
        activatedNativeMarkerWidgets_.push_back(TrackWidget(marker->Image_30));
        if (chestMarkerTexture) SetImageTexture(marker->Image_30, chestMarkerTexture);
        SetImageColor(marker->Image_30, chestMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                         : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
            if (!markedTreasureIds.empty()) markedTreasureIds += ", ";
            markedTreasureIds += marker->treasureID.ToString();
        }
        materializedTreasureIds.insert(NormalizeTreasureId(marker->treasureID.ToString()));
    }
    SuppressFinishedTreasureMarkers(map->TreasureMarkerList);

    auto* shardMarkerTexture =
        enemyByRoom.empty() ? nullptr : CreateEmbeddedTexture(map, IDR_SHARD_MARKER_PNG, "shard marker");
    for (const auto& markerEntry : map->RoomMarkerMap) {
        std::string roomId = markerEntry.Key().ToString();
        auto* marker = markerEntry.Value();
        if (!marker || !markedRooms.contains(roomId)) continue;

        SetVisible(marker);
        SetVisible(marker->Image_71);
        activatedNativeMarkerWidgets_.push_back(TrackWidget(marker->Image_71));
        auto enemy = enemyByRoom.find(roomId);
        if (enemy != enemyByRoom.end()) {
            marker->EnemyType = NameFromString(enemy->second);
            RememberNativeImageBrush(MAIN_MAP_NATIVE_BRUSH_OVERRIDES, marker->Image_71);
            if (shardMarkerTexture) SetImageTexture(marker->Image_71, shardMarkerTexture);
            SetImageColor(marker->Image_71, shardMarkerTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                              : SDK::FLinearColor{0.2f, 0.9f, 0.35f, 1.0f});
        }
    }

    if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
        if (!markedTreasureIds.empty()) {
            Logger::Log(LogLevel::File, "[Tracker] Materialized in-logic treasure IDs:", markedTreasureIds);
        }
        if (!reachableTreasureLocations.empty()) {
            Logger::Log(LogLevel::File, "[Tracker] Reachable missing chest checks:", reachableTreasureLocations);
        }
    }
    std::string missingTreasureMarkers;
    for (const auto& treasureId : treasureIds) {
        if (materializedTreasureIds.contains(treasureId)) continue;
        pendingSyntheticTreasureIds_.insert(treasureId);
        if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
            if (!missingTreasureMarkers.empty()) missingTreasureMarkers += ", ";
            missingTreasureMarkers += treasureId;
        }
    }
    if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
        if (!missingTreasureMarkers.empty()) {
            Logger::Log(LogLevel::File, "[Tracker] In-logic chest IDs without native marker widgets:",
                        missingTreasureMarkers);
        }
    }
}

void InGameTracker::ApplyMiniMap(void* miniMapWidget) {
    if (!ENABLE_SCREEN_SPACE_MINIMAP_COLLECTIBLES) return;

    const auto clearOverlay = [this] {
        const std::lock_guard lock(miniMapOverlayMutex_);
        miniMapOverlaySnapshot_ = {};
    };

    auto* miniMap = static_cast<SDK::UMiniMapBlueprint_C*>(miniMapWidget);
    auto* currentCanvas = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    if (activeMiniMap_ != miniMap || activeMiniMapCanvas_ != currentCanvas) {
        ClearMiniMapMarkers();
        activeMiniMap_ = miniMap;
        activeMiniMapCanvas_ = currentCanvas;
        activeMiniMapType_ = -1;
        miniMapDirty_ = true;
    }

    auto* archipelago = Archipelago::ConnectedInstance();
    ReconcileFinishedTreasureMarkers(archipelago);
    if (!IsMiniMapEnabled() || !archipelago || !inventorySynchronized_ || !miniMap || !currentCanvas ||
        !miniMap->Treasure_Panel || !miniMap->Frame_Image || !IsWidgetPaintVisible(miniMap) ||
        !IsWidgetPaintVisible(miniMap->Frame_Image)) {
        clearOverlay();
        return;
    }

    // The DX11 overlay is composed after Unreal's scene and UI, so it would otherwise remain visible above the
    // teleporter's intentional full-screen flash. Native warp flags cover the outgoing effect but can clear before
    // the arrival animation finishes; retain suppression for one native warp-animation duration after the last
    // active frame as well.
    auto* player = static_cast<SDK::APBBaseCharacter*>(GameManager::Instance().Player());
    static ULONGLONG lastActiveWarpEffect = 0;
    static DWORD arrivalSuppressionMilliseconds = 1500;
    const ULONGLONG warpTime = GetTickCount64();
    const bool nativeWarpEffectActive =
        player && (player->CurrentryWarpingByWarpRoom || player->WarpState != SDK::EWarpEffectState::WRP_Idle);
    if (nativeWarpEffectActive) {
        lastActiveWarpEffect = warpTime;
        const float nativeDuration = std::clamp(player->WarpAnimationDuration, 0.25f, 5.0f);
        arrivalSuppressionMilliseconds = static_cast<DWORD>(nativeDuration * 1000.0f);
    }
    if (nativeWarpEffectActive ||
        (lastActiveWarpEffect != 0 && warpTime - lastActiveWarpEffect < arrivalSuppressionMilliseconds)) {
        clearOverlay();
        return;
    }

    const auto miniMapGeometry = GetMiniMapGeometry(currentCanvas);
    auto* mapManager = miniMap->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!miniMapGeometry || !mapManager || !mapComponent) {
        clearOverlay();
        return;
    }

    const auto currentMapType =
        static_cast<SDK::int32>(mapComponent->CheckMapType(mapManager->GetCurrentAreaId()));
    if (activeMiniMapType_ != currentMapType) {
        ClearMiniMapMarkers();
        activeMiniMapType_ = currentMapType;
        miniMapDirty_ = true;
    }

    if (miniMapDirty_ || reachabilityDirty_) {
        Tracker tracker;
        auto traversalInventory = archipelago->GetTrackerInventory();
        for (const auto& nativeItem : bloodstained::tracker::generated::TRAVERSAL_NATIVE_ITEMS) {
            if (GameManager::Instance().CheckAllInventories(std::string(nativeItem.native_id))) {
                traversalInventory[std::string(nativeItem.item_name)] =
                    std::max(traversalInventory[std::string(nativeItem.item_name)], 1u);
            }
        }
        tracker.SetInventory(traversalInventory);
        const Difficulty difficulty = GetDifficulty();
        if (reachabilityDirty_) {
            reachableRooms_.clear();
            for (const auto* room : tracker.GetReachableRooms(difficulty)) {
                if (!room->out_of_map) reachableRooms_.insert(std::string(room->name));
            }
            reachabilityDirty_ = false;
        }

        // MiniMapBlueprint.UpdateIcons runs before this post-Tick hook. Preserve its current visibility/color and
        // restore the native image resource before rebuilding the AP target set.
        RestoreNativeImageBrushes(MINI_MAP_NATIVE_BRUSH_OVERRIDES);
        activatedMiniMapMarkerWidgets_.clear();
        miniMapVisibilityWidgets_.clear();
        miniMapTreasureIds_.clear();
        miniMapWallLocationIds_.clear();
        miniMapShardRooms_.clear();

        const auto candidateLocations =
            tracker.GetReachableMissingLocations(difficulty, archipelago->GetMissingLocationIds());
        for (const auto* location : candidateLocations) {
            const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
            if (!nativeLocationName ||
                archipelago->WasLocationClearedLocally(std::string(*nativeLocationName)) ||
                !archipelago->IsMissingLocation(std::string(*nativeLocationName), location->id)) {
                continue;
            }
            if (location->type == LocationType::CHEST) {
                miniMapTreasureIds_.insert(NormalizeTreasureId(std::string(*nativeLocationName)));
            } else if (location->type == LocationType::WALL) {
                miniMapWallLocationIds_.insert(location->id);
            } else if (location->type == LocationType::ENEMY) {
                for (const std::string_view room : tracker.GetReachableEnemyRooms(*location, difficulty)) {
                    miniMapShardRooms_.insert(std::string(room));
                }
            }
        }

        auto* chestTexture = miniMapTreasureIds_.empty()
                                 ? nullptr
                                 : CreateEmbeddedTexture(miniMap, IDR_CHEST_MARKER_PNG, "chest marker");
        for (auto* marker : miniMap->TreasureIconList) {
            if (!marker || !marker->Image_30) continue;
            const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
            if (!miniMapTreasureIds_.contains(treasureId)) continue;
            if (HasAuthoritativeChestPosition(treasureId)) {
                SetCollapsed(marker);
                SetCollapsed(marker->Image_30);
                continue;
            }
            RememberNativeImageBrush(MINI_MAP_NATIVE_BRUSH_OVERRIDES, marker->Image_30);
            SetVisible(marker);
            SetVisible(marker->Image_30);
            if (chestTexture) SetImageTexture(marker->Image_30, chestTexture);
            SetImageColor(marker->Image_30, chestTexture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                         : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
            activatedMiniMapMarkerWidgets_.push_back(TrackWidget(marker->Image_30));
            miniMapVisibilityWidgets_.push_back(TrackWidget(marker));
            miniMapVisibilityWidgets_.push_back(TrackWidget(marker->Image_30));
        }

        if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
            auto* roomManager = SDK::UPBRoomManager::GetRoomManager();
            const std::string currentRoom = roomManager ? roomManager->GetCurrentRoomId().ToString() : "<unknown>";
            const SDK::FVector playerLocation = player ? player->K2_GetActorLocation() : SDK::FVector{};
            Logger::Log(LogLevel::File, "[Tracker][Diagnostics] Current room:", currentRoom,
                        "player world position:", playerLocation.X, playerLocation.Y, playerLocation.Z);
            for (auto* marker : miniMap->TreasureIconList) {
                if (!marker) continue;
                const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
                const auto* location = FindChestLocationByNativeId(treasureId);
                if (!location || location->room != currentRoom) continue;
                const auto bounds = marker->Image_30 ? GetWidgetScreenRect(miniMap, marker->Image_30) : std::nullopt;
                Logger::Log(LogLevel::File, "[Tracker][Diagnostics] Native chest marker:",
                            marker->treasureID.ToString(), "target:", miniMapTreasureIds_.contains(treasureId),
                            "cleared:", archipelago->WasLocationClearedLocally(marker->treasureID.ToString()),
                            "visibility:", static_cast<int>(marker->GetVisibility()),
                            marker->Image_30 ? static_cast<int>(marker->Image_30->GetVisibility()) : -1,
                            "bounds:", bounds ? bounds->left : -1.0f, bounds ? bounds->top : -1.0f,
                            bounds ? bounds->right : -1.0f, bounds ? bounds->bottom : -1.0f);
            }
            if (auto* treasureComponent = hud->m_MapTreasureIconComponent) {
                for (const auto& entry : treasureComponent->TreasureMarkerMapLocation) {
                    const std::string treasureId = NormalizeTreasureId(entry.Key().ToString());
                    const auto* location = FindChestLocationByNativeId(treasureId);
                    if (!location || location->room != currentRoom) continue;
                    Logger::Log(LogLevel::File, "[Tracker][Diagnostics] Stored chest marker:",
                                entry.Key().ToString(), "position:", entry.Value().X, entry.Value().Y);
                }
            }
        }

        miniMapDirty_ = false;
        Logger::Log(LogLevel::File, "[Tracker] Rebuilt safe minimap overlay targets:",
                    miniMapTreasureIds_.size(), "native chests,", miniMapWallLocationIds_.size(),
                    "walls, and", miniMapShardRooms_.size(), "shard rooms");
    }

    // Native Tick is authoritative for chest layout. Reassert only the already-supported AP chest visibility, then
    // derive the generated marker transform from those live native slot positions.
    for (const auto& markerWidget : miniMapVisibilityWidgets_) {
        if (auto* widget = ResolveWidget(markerWidget)) SetVisible(widget);
    }
    // UpdateIcons runs before this hook on every minimap tick. Re-suppress exact-position AP targets after that
    // native refresh so their coarse native widgets cannot reappear over a different physical chest.
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker) continue;
        const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
        if (!miniMapTreasureIds_.contains(treasureId) || !HasAuthoritativeChestPosition(treasureId)) continue;
        SetCollapsed(marker);
        if (marker->Image_30) SetCollapsed(marker->Image_30);
    }
    // The native Blueprint can recreate a green icon for an AP-renamed chest even after the physical chest and all
    // of its AP slots are cleared. The local AP journal is authoritative, so suppress those stale native widgets
    // after UpdateIcons has run rather than allowing them to masquerade as unchecked locations.
    SuppressFinishedTreasureMarkers(miniMap->TreasureIconList);

    const auto clipRect = GetWidgetScreenRect(miniMap, miniMap->Frame_Image);
    auto layerTransform = GetNativeMiniMapTransform(miniMap, hud, *miniMapGeometry);
    if (!clipRect) {
        if constexpr (ENABLE_TRACKER_DIAGNOSTICS) {
            static ULONGLONG lastMissingGeometryLog = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastMissingGeometryLog >= 5000) {
                lastMissingGeometryLog = now;
                Logger::Log(LogLevel::File, "[Tracker][Diagnostics] Screen overlay unavailable; native transform:",
                            layerTransform.has_value(), "frame clip:", clipRect.has_value(), "treasure markers:",
                            miniMap->TreasureIconList.Num(), "wall targets:", miniMapWallLocationIds_.size(),
                            "shard targets:", miniMapShardRooms_.size());
            }
        }
        clearOverlay();
        return;
    }

    if (!layerTransform) {
        // The frame can paint before any native chest has valid cached geometry. Publish an empty fresh snapshot so
        // the Present overlay follows minimap visibility immediately while waiting for a safe positional anchor.
        MiniMapOverlaySnapshot waitingSnapshot;
        waitingSnapshot.visible = true;
        waitingSnapshot.clipLeft = clipRect->left;
        waitingSnapshot.clipTop = clipRect->top;
        waitingSnapshot.clipRight = clipRect->right;
        waitingSnapshot.clipBottom = clipRect->bottom;
        waitingSnapshot.updatedAtMilliseconds = GetTickCount64();
        const std::lock_guard lock(miniMapOverlayMutex_);
        miniMapOverlaySnapshot_ = std::move(waitingSnapshot);
        return;
    }

    const SDK::EDivideMap renderMapType = currentCanvas->mapType;
    const SDK::EDivideMap areaMapType = static_cast<SDK::EDivideMap>(currentMapType);
    const NativeMapAxes axes =
        FindNativeMiniMapAxes(mapManager, mapComponent, renderMapType, areaMapType, currentCanvas);
    const SDK::FVector2D markerSize = GetMiniMapOverlayMarkerSize(*layerTransform);
    const float markerWidth = markerSize.X;
    const float markerHeight = markerSize.Y;
    const SDK::FVector2D shardMarkerSize = GetMiniMapShardOverlayMarkerSize(*layerTransform);
    const SDK::FVector2D wallMarkerSize = GetMiniMapWallOverlayMarkerSize(*layerTransform);

    // GetRoomCenterInMapPosition returns coordinates in the render target's center-relative grid, while the map
    // texture is painted from IconPixelOffset inside that target. Applying that native offset through the measured
    // pixel basis accounts for the observed residual without a resolution-, zoom-, or HUD-position-specific value.
    layerTransform->translation.X += currentCanvas->IconPixelOffset.X * layerTransform->scale.X;
    layerTransform->translation.Y +=
        currentCanvas->IconPixelOffset.Y * layerTransform->scale.Y + markerHeight * 0.5f;

    MiniMapOverlaySnapshot snapshot;
    snapshot.visible = true;
    snapshot.clipLeft = clipRect->left;
    snapshot.clipTop = clipRect->top;
    snapshot.clipRight = clipRect->right;
    snapshot.clipBottom = clipRect->bottom;
    snapshot.updatedAtMilliseconds = GetTickCount64();
    snapshot.markers.reserve(miniMapTreasureIds_.size() + miniMapWallLocationIds_.size() +
                             miniMapShardRooms_.size());

    std::unordered_set<std::string> nativeWidgetTreasureIds;
    nativeWidgetTreasureIds.reserve(miniMap->TreasureIconList.Num());
    for (auto* marker : miniMap->TreasureIconList) {
        if (marker) nativeWidgetTreasureIds.insert(NormalizeTreasureId(marker->treasureID.ToString()));
    }
    std::unordered_set<std::string> syntheticTreasureIds;
    for (const auto& treasureId : miniMapTreasureIds_) {
        if (HasAuthoritativeChestPosition(treasureId) || !nativeWidgetTreasureIds.contains(treasureId)) {
            syntheticTreasureIds.insert(treasureId);
        }
    }
    for (const auto& treasureId : syntheticTreasureIds) {
        const auto* location = FindChestLocationByNativeId(treasureId);
        if (!location || !location->has_map_position) continue;
        const auto mapPosition = GetMiniMapLocationPosition(
            mapManager, mapComponent, renderMapType, areaMapType, currentCanvas, axes, *location);
        if (!mapPosition) continue;
        const SDK::FVector2D pixelPosition = TransformMiniMapPosition(*mapPosition, *layerTransform);
        snapshot.markers.push_back(MiniMapOverlayMarker{MiniMapOverlayMarkerKind::CHEST, pixelPosition.X,
                                                        pixelPosition.Y, markerWidth, markerHeight});
    }

    for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
        if (!miniMapWallLocationIds_.contains(location.id)) continue;
        const auto mapPosition = GetMiniMapLocationPosition(mapManager, mapComponent, renderMapType, areaMapType,
                                                            currentCanvas, axes, location);
        if (!mapPosition) continue;
        const SDK::FVector2D pixelPosition = TransformMiniMapPosition(*mapPosition, *layerTransform);
        snapshot.markers.push_back(MiniMapOverlayMarker{MiniMapOverlayMarkerKind::WALL, pixelPosition.X,
                                                        pixelPosition.Y, wallMarkerSize.X, wallMarkerSize.Y});
    }

    for (const std::string& roomName : miniMapShardRooms_) {
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map ||
            mapComponent->CheckMapType(mapManager->RoomIdToAreaId(NameFromString(roomName))) != areaMapType) {
            continue;
        }
        const auto mapPosition = GetRoomMapCenter(mapComponent, currentCanvas, renderMapType, *room);
        if (!mapPosition) continue;
        const SDK::FVector2D pixelPosition = TransformMiniMapPosition(*mapPosition, *layerTransform);
        snapshot.markers.push_back(MiniMapOverlayMarker{MiniMapOverlayMarkerKind::SHARD, pixelPosition.X,
                                                        pixelPosition.Y, shardMarkerSize.X, shardMarkerSize.Y});
    }

    {
        const std::lock_guard lock(miniMapOverlayMutex_);
        miniMapOverlaySnapshot_ = std::move(snapshot);
    }
}
