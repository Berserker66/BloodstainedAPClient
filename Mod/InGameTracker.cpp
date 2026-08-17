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
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Archipelago.h"
#include "GameManager.h"
#include "Logger.h"
#include "Tracker.h"
#include "Utils.h"

namespace {

using bloodstained::tracker::Difficulty;
using bloodstained::tracker::Tracker;
using bloodstained::tracker::generated::LocationType;

constexpr std::string_view TRACKER_DISPLAY_MODE_SAVE_KEY = "AP_TrackerDisplayMode";
constexpr std::string_view HIDDEN_CHEST_NATIVE_ID = "treasurebox_sip025_2";
constexpr std::string_view CHEST_MARKER_TEXTURE =
    "/Game/Archipelago/UI/AP_ChestMarker.AP_ChestMarker";
constexpr std::string_view WALL_MARKER_TEXTURE =
    "/Game/Archipelago/UI/AP_WallMarker.AP_WallMarker";
constexpr std::string_view SHARD_MARKER_TEXTURE =
    "/Game/Archipelago/UI/AP_ShardMarker.AP_ShardMarker";
// Compile-time gate for all main-map/minimap diagnostics. Keep disabled in normal test and release builds.
constexpr bool ENABLE_MAP_DIAGNOSTICS = false;
constexpr bool ENABLE_MINIMAP_CAPTURE_DIAGNOSTICS = false;
#define LOG_MAP_DIAGNOSTIC(...)                         \
    do {                                                \
        if constexpr (ENABLE_MAP_DIAGNOSTICS) {         \
            Logger::Log(__VA_ARGS__);                   \
        }                                               \
    } while (false)
// AP-created minimap icons live beside the continuously laid-out native treasure widgets, while reachability cells
// sit behind the native exploration render target. Both stay inside Unreal's paint tree and inherit its clipping,
// fades and HUD layering.

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

const bloodstained::tracker::generated::LocationData* FindLocationByNativeId(
    std::string_view normalizedNativeId) {
    // Paint and post-Tick paths ask this for every native marker. The old nested binding/location scan multiplied
    // the complete generated tables by the marker count every frame.
    static const std::unordered_map<std::string,
                                    const bloodstained::tracker::generated::LocationData*> locations = [] {
        std::unordered_map<std::uint64_t,
                           const bloodstained::tracker::generated::LocationData*> byId;
        byId.reserve(bloodstained::tracker::generated::LOCATIONS.size());
        for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
            byId.emplace(location.id, &location);
        }
        std::unordered_map<std::string,
                           const bloodstained::tracker::generated::LocationData*> byNativeId;
        byNativeId.reserve(bloodstained::tracker::generated::LOCATION_BINDINGS.size());
        for (const auto& binding : bloodstained::tracker::generated::LOCATION_BINDINGS) {
            const auto location = byId.find(binding.id);
            if (location == byId.end()) continue;
            byNativeId.emplace(NormalizeTreasureId(std::string(binding.native_name)), location->second);
        }
        return byNativeId;
    }();
    const auto location = locations.find(std::string(normalizedNativeId));
    return location == locations.end() ? nullptr : location->second;
}

const bloodstained::tracker::generated::LocationData* FindChestLocationByNativeId(
    std::string_view normalizedNativeId) {
    const auto* location = FindLocationByNativeId(normalizedNativeId);
    return location && location->type == LocationType::CHEST ? location : nullptr;
}

bool HasStaticChestPosition(std::string_view normalizedNativeId) {
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

void SetHidden(SDK::UWidget* widget) {
    SetWidgetVisibility(widget, SDK::ESlateVisibility::Hidden);
}

TrackedWidgetHandle TrackWidget(SDK::UWidget* widget) {
    return widget ? TrackedWidgetHandle{widget, widget->Index} : TrackedWidgetHandle{};
}

void ReleaseRootedObject(void*& pointer, SDK::int32& objectIndex) {
    if (pointer && objectIndex >= 0 && SDK::UObject::GObjects->GetByIndex(objectIndex) == pointer) {
        auto* object = static_cast<SDK::UObject*>(pointer);
        object->Flags = static_cast<SDK::EObjectFlags>(
            static_cast<SDK::int32>(object->Flags) &
            ~static_cast<SDK::int32>(SDK::EObjectFlags::MarkAsRootSet));
    }
    pointer = nullptr;
    objectIndex = -1;
}

void ForgetObject(void*& pointer, SDK::int32& objectIndex) {
    pointer = nullptr;
    objectIndex = -1;
}

bool IsLiveObject(const SDK::UObject* object, SDK::int32 expectedIndex) {
    if (!object || expectedIndex < 0 ||
        SDK::UObject::GObjects->GetByIndex(expectedIndex) != object || !object->Class) {
        return false;
    }
    constexpr SDK::int32 DESTROYED_FLAGS =
        static_cast<SDK::int32>(SDK::EObjectFlags::BeginDestroyed) |
        static_cast<SDK::int32>(SDK::EObjectFlags::FinishDestroyed);
    return (static_cast<SDK::int32>(object->Flags) & DESTROYED_FLAGS) == 0;
}

bool IsLiveBrush(const SDK::USlateBrushAsset* brush, SDK::int32 expectedIndex,
                 const SDK::UObject* resource, SDK::int32 resourceIndex) {
    if (!IsLiveObject(brush, expectedIndex)) return false;
    return brush->Brush.ResourceObject == resource && IsLiveObject(resource, resourceIndex);
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
SDK::int32 REGISTERED_EXTRA_TREASURE_COMPONENT_INDEX = -1;
TrackedWidgetHandle BORROWED_CUSTOM_MINIMAP_IMAGE;
TrackedWidgetHandle BORROWED_CUSTOM_MINIMAP_PANEL;
SDK::ESlateVisibility BORROWED_CUSTOM_MINIMAP_PANEL_VISIBILITY = SDK::ESlateVisibility::Collapsed;
SDK::FVector2D BORROWED_CUSTOM_MINIMAP_POSITION{};
SDK::FVector2D BORROWED_CUSTOM_MINIMAP_SIZE{};
SDK::FSlateBrush BORROWED_CUSTOM_MINIMAP_BRUSH{};
bool BORROWED_CUSTOM_MINIMAP_ACTIVE = false;
void* MINI_MAP_GHOST_PAINT_BRUSH = nullptr;
SDK::int32 MINI_MAP_GHOST_PAINT_BRUSH_INDEX = -1;
void* MINI_MAP_GHOST_TEXTURE = nullptr;
SDK::int32 MINI_MAP_GHOST_TEXTURE_INDEX = -1;
SDK::int32 MINI_MAP_GHOST_TEXTURE_WIDTH = 0;
SDK::int32 MINI_MAP_GHOST_TEXTURE_HEIGHT = 0;
SDK::FVector2D MINI_MAP_GHOST_RENDER_MINIMUM{};
SDK::FVector2D MINI_MAP_GHOST_RENDER_MAXIMUM{};
SDK::FVector2D MINI_MAP_GHOST_LAST_PAINT_MINIMUM{};
SDK::FVector2D MINI_MAP_GHOST_LAST_PAINT_MAXIMUM{};
SDK::FVector2D MINI_MAP_GHOST_LAST_CANVAS_MINIMUM{};
SDK::FVector2D MINI_MAP_GHOST_LAST_CANVAS_MAXIMUM{};
SDK::FVector2D MINI_MAP_GHOST_LAST_AUTO_OFFSET{};
SDK::FVector2D MINI_MAP_GHOST_LAST_PLAYER_RENDER{};
SDK::FVector2D MINI_MAP_GHOST_LAST_PLAYER_NATIVE_PAINT{};
SDK::FVector2D MINI_MAP_GHOST_LAST_PLAYER_ATLAS_PAINT{};
bool MINI_MAP_GHOST_LAST_AUTO_OFFSET_VALID = false;
double MINI_MAP_PANEL_MAP_X_TO_X = 0.0;
double MINI_MAP_PANEL_MAP_Y_TO_X = 0.0;
double MINI_MAP_PANEL_MAP_X_TO_Y = 0.0;
double MINI_MAP_PANEL_MAP_Y_TO_Y = 0.0;
double MINI_MAP_PANEL_OFFSET_X = 0.0;
double MINI_MAP_PANEL_OFFSET_Y = 0.0;
bool MINI_MAP_PANEL_TRANSFORM_VALID = false;
double MINI_MAP_RENDER_TO_PANEL_X_SCALE = 0.0;
double MINI_MAP_RENDER_TO_PANEL_Y_SCALE = 0.0;
double MINI_MAP_RENDER_TO_PANEL_OFFSET_X = 0.0;
double MINI_MAP_RENDER_TO_PANEL_OFFSET_Y = 0.0;
bool MINI_MAP_RENDER_TO_PANEL_VALID = false;
std::vector<SDK::uint8> MINI_MAP_GHOST_PNG_BYTES;
std::vector<SDK::uint8> MINI_MAP_GHOST_PIXELS;
std::chrono::steady_clock::time_point MINI_MAP_LAST_DIAGNOSTIC_CAPTURE{};

struct MiniMapAtlasCell {
    std::string room;
    std::uint32_t assignment = 0;
    SDK::int32 traverseIndex = -1;
    SDK::FVector2D center{};
    SDK::int32 left = 0;
    SDK::int32 top = 0;
    SDK::int32 right = 0;
    SDK::int32 bottom = 0;
    bool reachable = false;
    bool explored = false;
    bool painted = false;
};

std::vector<MiniMapAtlasCell> MINI_MAP_ATLAS_CELLS;
SDK::int32 MINI_MAP_ATLAS_MAP_TYPE = -1;
SDK::int32 MINI_MAP_ATLAS_LAST_TRAVERSE_INDEX = -2;

constexpr SDK::int32 TRAVERSE_WIDTH = 200;
constexpr SDK::int32 TRAVERSE_HEIGHT = 100;
constexpr SDK::int32 ROOM_MAP_TO_TRAVERSE_Z = 50;
// The most zoomed-out minimap exposes at most seven by five room cells. Five cells per axis is therefore a
// deliberately conservative margin which also covers large markers at the edge of the clipped frame.
constexpr SDK::int32 MINI_MAP_CELL_CULL_RADIUS = 5;

SDK::int32 GetTraverseIndex(const bloodstained::tracker::generated::RoomMapData& room,
                            std::uint32_t assignment) {
    if (assignment == 0 || room.width == 0) return -1;
    const std::uint32_t zeroBasedAssignment = assignment - 1u;
    const SDK::int32 cellX = static_cast<SDK::int32>(zeroBasedAssignment % room.width);
    const SDK::int32 cellZ = static_cast<SDK::int32>(zeroBasedAssignment / room.width);
    const SDK::int32 gridX = room.x + cellX;
    const SDK::int32 gridY = room.z + cellZ + ROOM_MAP_TO_TRAVERSE_Z;
    if (gridX < 0 || gridX >= TRAVERSE_WIDTH || gridY < 0 || gridY >= TRAVERSE_HEIGHT) return -1;
    return gridY * TRAVERSE_WIDTH + gridX;
}

struct GhostCellColor {
    SDK::uint8 red;
    SDK::uint8 green;
    SDK::uint8 blue;
};

std::string_view GetRoomBiomePrefix(std::string_view roomName) {
    const std::size_t prefixBegin = roomName.find_first_not_of("m0123456789");
    const std::size_t prefixEnd = roomName.find('_', prefixBegin);
    if (prefixBegin == std::string_view::npos || prefixEnd == std::string_view::npos) return {};
    return roomName.substr(prefixBegin, prefixEnd - prefixBegin);
}

GhostCellColor GetGhostCellColor(std::string_view roomName) {
    // Muted versions of the biome fills on the official castle-map artwork. These are deliberately less saturated
    // than the explored map so hue communicates the destination without making a reachable ghost look discovered.
    const std::string_view prefix = GetRoomBiomePrefix(roomName);
    if (prefix == "SIP") return {188, 134, 143};  // Galleon Minerva
    if (prefix == "VIL") return {139, 164, 126};  // Arvantville
    if (prefix == "ENT") return {190, 170, 115};  // Entrance
    if (prefix == "GDN") return {145, 169, 118};  // Garden of Silence
    if (prefix == "SAN") return {190, 147, 119};  // Dian Cecht Cathedral
    if (prefix == "KNG") return {137, 165, 132};  // Hall of Termination
    if (prefix == "LIB") return {184, 169, 113};  // Livre Ex Machina
    if (prefix == "TWR") return {124, 155, 175};  // Towers of Twin Dragons
    if (prefix == "TRN") return {157, 150, 132};  // Bridge of Evil / train
    if (prefix == "BIG") return {174, 126, 139};  // Den of Behemoths
    if (prefix == "UGD") return {125, 157, 136};  // Forbidden Underground Waterway
    if (prefix == "SND") return {190, 164, 116};  // Hidden Desert
    if (prefix == "ARC") return {167, 157, 112};  // Underground Sorcery Lab
    if (prefix == "TAR") return {184, 119, 142};  // Secret Sorcery Lab
    if (prefix == "JPN") return {190, 127, 123};  // Oriental Sorcery Lab
    if (prefix == "RVA") return {190, 105, 84};   // Inferno Cave
    if (prefix == "ICE") return {125, 158, 181};  // Glacial Tomb
    // Bonus/free-content maps have no color assignment in the official artwork.
    return {135, 143, 155};
}

SDK::FLinearColor ToLinearGhostColor(const GhostCellColor& color, float alpha) {
    return {color.red / 255.0f, color.green / 255.0f, color.blue / 255.0f, alpha};
}

std::vector<ReachableRoomCell> BuildReachableRoomCells(
    const std::unordered_set<std::string>& reachableRooms) {
    std::vector<ReachableRoomCell> cells;
    for (const std::string& roomName : reachableRooms) {
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map) continue;
        const std::uint32_t cellCount = static_cast<std::uint32_t>(room->width) * room->height;
        for (std::uint32_t assignment = 1; assignment <= cellCount; ++assignment) {
            if (Tracker::IsRoomCellVisible(*room, assignment)) {
                cells.push_back({roomName, assignment});
            }
        }
    }
    return cells;
}

bool IsWidgetPaintVisible(SDK::UWidget* widget);

std::filesystem::path BloodstainedSavedDirectory() {
    std::array<wchar_t, MAX_PATH> localAppData{};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
    if (length == 0 || length >= localAppData.size()) return {};
    return std::filesystem::path(localAppData.data()) / L"BloodstainedRotN" / L"Saved";
}

std::filesystem::path MiniMapDiagnosticDirectory() {
    const auto saved = BloodstainedSavedDirectory();
    return saved.empty() ? std::filesystem::path{} : saved / L"Logs" / L"MinimapDiagnostics";
}

std::string MiniMapDiagnosticTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) %
                              1000;
    std::ostringstream stream;
    stream << std::put_time(&local, "%Y%m%d-%H%M%S") << '-' << std::setw(3)
           << std::setfill('0') << milliseconds.count();
    return stream.str();
}

std::string SanitizeDiagnosticName(std::string value) {
    for (char& character : value) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '-' &&
            character != '_') {
            character = '_';
        }
    }
    return value.empty() ? "unknown-room" : value;
}

bool WriteDiagnosticBytes(const std::filesystem::path& path,
                          const std::vector<SDK::uint8>& bytes) {
    if (bytes.empty()) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool CaptureScreenRegionBmp(const std::filesystem::path& path, int left, int top,
                            int width, int height) {
    if (width <= 0 || height <= 0) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (!memory || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    const HGDIOBJ previous = SelectObject(memory, bitmap);
    const bool copied = BitBlt(memory, 0, 0, width, height, screen, left, top,
                               SRCCOPY | CAPTUREBLT) != FALSE;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    const std::size_t pixelBytes = static_cast<std::size_t>(width) * height * 4;
    std::vector<SDK::uint8> pixels(pixelBytes);
    const bool read = copied && GetDIBits(memory, bitmap, 0, static_cast<UINT>(height),
                                          pixels.data(), &info, DIB_RGB_COLORS) != 0;

    bool written = false;
    if (read) {
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4d42;
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(pixelBytes);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output) {
            output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
            output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
            output.write(reinterpret_cast<const char*>(pixels.data()),
                         static_cast<std::streamsize>(pixels.size()));
            written = output.good();
        }
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return written;
}

void CaptureMiniMapDiagnostics(SDK::UMiniMapBlueprint_C* miniMap,
                               SDK::UPBMapComponent* mapComponent,
                               SDK::EDivideMap mapType) {
    if constexpr (!(ENABLE_MAP_DIAGNOSTICS && ENABLE_MINIMAP_CAPTURE_DIAGNOSTICS)) return;
    if (!miniMap || !mapComponent || !miniMap->Frame_Image) return;
    const auto now = std::chrono::steady_clock::now();
    if (MINI_MAP_LAST_DIAGNOSTIC_CAPTURE.time_since_epoch().count() != 0 &&
        now - MINI_MAP_LAST_DIAGNOSTIC_CAPTURE < std::chrono::seconds(10)) {
        return;
    }
    MINI_MAP_LAST_DIAGNOSTIC_CAPTURE = now;

    const std::filesystem::path directory = MiniMapDiagnosticDirectory();
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Could not create minimap diagnostic directory:",
                    error.message());
        return;
    }

    auto* roomManager = GameManager::Instance().RoomManager();
    const std::string room = roomManager ? roomManager->GetCurrentRoomId().ToString() : "unknown-room";
    const std::string stem = MiniMapDiagnosticTimestamp() + "_" + SanitizeDiagnosticName(room);
    const bool atlasWritten = WriteDiagnosticBytes(
        directory / ("atlas_" + stem + ".png"), MINI_MAP_GHOST_PNG_BYTES);

    const SDK::FGeometry frameGeometry = miniMap->Frame_Image->GetCachedGeometry();
    const SDK::FVector2D frameSize = SDK::USlateBlueprintLibrary::GetLocalSize(frameGeometry);
    const SDK::FVector2D frameTopLeft =
        SDK::USlateBlueprintLibrary::LocalToAbsolute(frameGeometry, {});
    const SDK::FVector2D frameBottomRight =
        SDK::USlateBlueprintLibrary::LocalToAbsolute(frameGeometry, frameSize);
    const int left = static_cast<int>(std::floor(std::min(frameTopLeft.X, frameBottomRight.X)));
    const int top = static_cast<int>(std::floor(std::min(frameTopLeft.Y, frameBottomRight.Y)));
    const int width = static_cast<int>(std::ceil(std::abs(frameBottomRight.X - frameTopLeft.X)));
    const int height = static_cast<int>(std::ceil(std::abs(frameBottomRight.Y - frameTopLeft.Y)));
    const bool compositeWritten = CaptureScreenRegionBmp(
        directory / ("minimap_" + stem + ".bmp"), left, top, width, height);

    auto* totalMap = miniMap->TotalMapBlueprint;
    SDK::FVector2D totalMapCanvasSize{};
    SDK::FVector2D miniRenderSize{};
    if (totalMap && totalMap->ImageParent_Canvas) {
        totalMapCanvasSize = SDK::USlateBlueprintLibrary::GetLocalSize(
            totalMap->ImageParent_Canvas->GetCachedGeometry());
    }
    if (totalMap && totalMap->RenderTargetMini_Image) {
        miniRenderSize = SDK::USlateBlueprintLibrary::GetLocalSize(
            totalMap->RenderTargetMini_Image->GetCachedGeometry());
    }
    std::size_t visibleTreasureWidgets = 0;
    std::size_t laidOutTreasureWidgets = 0;
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker) continue;
        if (IsWidgetPaintVisible(marker)) ++visibleTreasureWidgets;
        const SDK::FVector2D size = SDK::USlateBlueprintLibrary::GetLocalSize(
            marker->GetCachedGeometry());
        if (size.X > 0.0f && size.Y > 0.0f) ++laidOutTreasureWidgets;
    }

    std::ofstream metadata(directory / ("state_" + stem + ".txt"),
                           std::ios::trunc);
    if (metadata) {
        metadata << "room=" << room << '\n'
                 << "map_type=" << static_cast<int>(mapType) << '\n'
                 << "mini_left_top=" << mapComponent->m_MiniLeftTop.X << ','
                 << mapComponent->m_MiniLeftTop.Y << '\n'
                 << "mini_write=" << mapComponent->m_MiniWrite.X << ','
                 << mapComponent->m_MiniWrite.Y << '\n'
                 << "map_offset_sip=" << mapComponent->GetMapOffset(SDK::EDivideMap::Sip).X << ','
                 << mapComponent->GetMapOffset(SDK::EDivideMap::Sip).Y << '\n'
                 << "map_offset_alchemy=" << mapComponent->GetMapOffset(SDK::EDivideMap::Alchemy).X << ','
                 << mapComponent->GetMapOffset(SDK::EDivideMap::Alchemy).Y << '\n'
                 << "map_offset_main=" << mapComponent->GetMapOffset(SDK::EDivideMap::Main).X << ','
                 << mapComponent->GetMapOffset(SDK::EDivideMap::Main).Y << '\n'
                 << "minimap_initial_top_left=" << miniMap->InitialTopLeft.X << ','
                 << miniMap->InitialTopLeft.Y << '\n'
                 << "minimap_new_top_left=" << miniMap->NewTopLeft.X << ','
                 << miniMap->NewTopLeft.Y << '\n'
                 << "minimap_top_left_offset=" << miniMap->TopLeftOffset.X << ','
                 << miniMap->TopLeftOffset.Y << '\n'
                 << "minimap_marker_panel_offset=" << miniMap->MarkerPanelOffset.X << ','
                 << miniMap->MarkerPanelOffset.Y << '\n'
                 << "minimap_scale_rate=" << miniMap->ScaleRate << '\n'
                 << "minimap_zoom_rate=" << miniMap->ZoomRate << '\n'
                 << "total_map_canvas_size=" << totalMapCanvasSize.X << ','
                 << totalMapCanvasSize.Y << '\n'
                 << "mini_render_size=" << miniRenderSize.X << ',' << miniRenderSize.Y << '\n'
                 << "atlas_render_bounds=" << MINI_MAP_GHOST_RENDER_MINIMUM.X << ','
                 << MINI_MAP_GHOST_RENDER_MINIMUM.Y << ',' << MINI_MAP_GHOST_RENDER_MAXIMUM.X << ','
                 << MINI_MAP_GHOST_RENDER_MAXIMUM.Y << '\n'
                 << "atlas_canvas_bounds=" << MINI_MAP_GHOST_LAST_CANVAS_MINIMUM.X << ','
                 << MINI_MAP_GHOST_LAST_CANVAS_MINIMUM.Y << ',' << MINI_MAP_GHOST_LAST_CANVAS_MAXIMUM.X << ','
                 << MINI_MAP_GHOST_LAST_CANVAS_MAXIMUM.Y << '\n'
                 << "atlas_paint_bounds=" << MINI_MAP_GHOST_LAST_PAINT_MINIMUM.X << ','
                 << MINI_MAP_GHOST_LAST_PAINT_MINIMUM.Y << ',' << MINI_MAP_GHOST_LAST_PAINT_MAXIMUM.X << ','
                 << MINI_MAP_GHOST_LAST_PAINT_MAXIMUM.Y << '\n'
                 << "atlas_tuning=baked\n"
                 << "atlas_auto_offset_valid=" << MINI_MAP_GHOST_LAST_AUTO_OFFSET_VALID << '\n'
                 << "atlas_auto_offset=" << MINI_MAP_GHOST_LAST_AUTO_OFFSET.X << ','
                 << MINI_MAP_GHOST_LAST_AUTO_OFFSET.Y << '\n'
                 << "player_render_position=" << MINI_MAP_GHOST_LAST_PLAYER_RENDER.X << ','
                 << MINI_MAP_GHOST_LAST_PLAYER_RENDER.Y << '\n'
                 << "player_native_paint=" << MINI_MAP_GHOST_LAST_PLAYER_NATIVE_PAINT.X << ','
                 << MINI_MAP_GHOST_LAST_PLAYER_NATIVE_PAINT.Y << '\n'
                 << "player_atlas_paint=" << MINI_MAP_GHOST_LAST_PLAYER_ATLAS_PAINT.X << ','
                 << MINI_MAP_GHOST_LAST_PLAYER_ATLAS_PAINT.Y << '\n'
                 << "treasure_widgets_visible=" << visibleTreasureWidgets << '\n'
                 << "treasure_widgets_laid_out=" << laidOutTreasureWidgets << '\n'
                 << "frame_bounds=" << left << ',' << top << ',' << width << ',' << height << '\n';
    }
    LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Captured minimap diagnostics:", stem,
                "composite:", compositeWritten, "atlas:", atlasWritten);
}

void RegisterExtraNativeTreasureLocations(SDK::UPBMapTreasureIconComponent* treasureComponent) {
    if (!treasureComponent || REGISTERED_EXTRA_TREASURE_COMPONENT_INDEX == treasureComponent->Index) return;
    REGISTERED_EXTRA_TREASURE_COMPONENT_INDEX = treasureComponent->Index;

    struct NativeTreasureRegistration {
        std::string_view treasureId;
        SDK::EGameTreasureFlag flag;
        std::string_view roomId;
        SDK::FVector worldPosition;
    };
    const std::array<NativeTreasureRegistration, 2> REGISTRATIONS = {{
        {"Treasurebox_SIP025_2", SDK::EGameTreasureFlag::Treasure_StatusUp_Test1,
         "m01SIP_025", {1003.0f, -120.0f, 60.0f}},
        {"Wall_SIP004_1", SDK::EGameTreasureFlag::Dummy1,
         "m01SIP_004", {120.0f, -120.0f, 240.0f}},
    }};

    auto findStoredPosition = [&](std::string_view normalizedId) -> std::optional<SDK::FVector2D> {
        for (const auto& entry : treasureComponent->TreasureMarkerMapLocation) {
            if (NormalizeTreasureId(entry.Key().ToString()) == normalizedId) return entry.Value();
        }
        return std::nullopt;
    };

    for (const auto& registration : REGISTRATIONS) {
        const std::string normalizedId = NormalizeTreasureId(std::string(registration.treasureId));
        auto position = findStoredPosition(normalizedId);
        const bool hadValidPosition = position &&
                                      (std::abs(position->X) > 0.001f || std::abs(position->Y) > 0.001f);
        if (!hadValidPosition) {
            if (position) {
                // Cooked AP-only actors can run before the treasure component has enough room context and leave a
                // permanent default (0, 0) entry. Remove that sentinel so the normal native registration path can
                // derive the room-relative minimap position from the real actor coordinates below.
                treasureComponent->UnregistTreasureIcon(NameFromString(registration.treasureId),
                                                         registration.flag);
            }
            treasureComponent->RegistTreasureIcon(NameFromString(registration.treasureId), registration.flag,
                                                   NameFromString(registration.roomId),
                                                   registration.worldPosition);
            position = findStoredPosition(normalizedId);
        }
        if (position && (std::abs(position->X) > 0.001f || std::abs(position->Y) > 0.001f)) {
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Native minimap position",
                        hadValidPosition ? "already valid:" : "registered:",
                        std::string(registration.treasureId), "map:", position->X, position->Y);
        } else {
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Native minimap position registration failed:",
                        std::string(registration.treasureId), "room:", std::string(registration.roomId),
                        "stored map:", position ? position->X : 0.0f, position ? position->Y : 0.0f);
        }
    }

    // The hidden right chest shares m01SIP_025 with the fully native left chest. PB's stored treasure coordinates
    // use 100 units per room-map cell (the minimap Blueprint later scales them by 1.7075/1.6285). When actor-based
    // registration cannot resolve the streamed room transform and leaves (0, 0), derive the missing entry from that
    // same-room native anchor. Once committed to the component, the ordinary Blueprint owns all subsequent layout.
    constexpr std::string_view HIDDEN_CHEST_ID = "Treasurebox_SIP025_2";
    constexpr std::string_view HIDDEN_CHEST_ANCHOR_ID = "Treasurebox_SIP025_1";
    constexpr float HIDDEN_CHEST_MAP_X = 0.796032f;
    constexpr float HIDDEN_CHEST_ANCHOR_MAP_X = 0.428571f;
    const std::string hiddenId = NormalizeTreasureId(std::string(HIDDEN_CHEST_ID));
    const auto hiddenPosition = findStoredPosition(hiddenId);
    const auto anchorPosition = findStoredPosition(
        NormalizeTreasureId(std::string(HIDDEN_CHEST_ANCHOR_ID)));
    const bool hiddenPositionInvalid = !hiddenPosition ||
                                       (std::abs(hiddenPosition->X) <= 0.001f &&
                                        std::abs(hiddenPosition->Y) <= 0.001f);
    if (hiddenPositionInvalid && anchorPosition) {
        const SDK::FVector2D repairedPosition{
            anchorPosition->X + (HIDDEN_CHEST_MAP_X - HIDDEN_CHEST_ANCHOR_MAP_X) * 100.0f,
            anchorPosition->Y,
        };
        treasureComponent->AddTreasureLocation(NameFromString(HIDDEN_CHEST_ID), repairedPosition, false);
        auto storedPosition = findStoredPosition(hiddenId);
        if (!storedPosition || (std::abs(storedPosition->X) <= 0.001f &&
                                std::abs(storedPosition->Y) <= 0.001f)) {
            // AddTreasureLocation is insert-only for this component and will not replace the malformed cooked key.
            // Update the existing native map entry in place; MiniMapBlueprint reads this exact TMap before its stock
            // SetPosition call.
            for (auto& entry : treasureComponent->TreasureMarkerMapLocation) {
                if (NormalizeTreasureId(entry.Key().ToString()) != hiddenId) continue;
                entry.Value() = repairedPosition;
                break;
            }
            storedPosition = findStoredPosition(hiddenId);
        }
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Repaired hidden chest from same-room native anchor; anchor:",
                    anchorPosition->X, anchorPosition->Y, "requested:", repairedPosition.X,
                    repairedPosition.Y, "stored:", storedPosition ? storedPosition->X : 0.0f,
                    storedPosition ? storedPosition->Y : 0.0f);
    }
}

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
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Reconciled native finished state for",
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

SDK::UTexture2D* LoadCookedMarkerTexture(std::string_view assetPath, std::string_view description) {
    SDK::TSoftObjectPtr<SDK::UObject> reference{};
    const SDK::FSoftObjectPath path =
        SDK::UKismetSystemLibrary::MakeSoftObjectPath(FStringFromString(std::string(assetPath)));
    reference.ObjectID.AssetPathName = path.AssetPathName;
    reference.ObjectID.SubPathString = path.SubPathString;
    auto* object = SDK::UKismetSystemLibrary::LoadAsset_Blocking(reference);
    if (!object || !object->IsA(SDK::UTexture2D::StaticClass())) {
        LOG_MAP_DIAGNOSTIC("[Tracker] Could not load cooked ", description, " texture: ", assetPath);
        return nullptr;
    }
    return static_cast<SDK::UTexture2D*>(object);
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
    if (!texture) LOG_MAP_DIAGNOSTIC("[Tracker] Unreal could not import the ghost-map tile texture");
    return texture;
}

void AppendBigEndian(std::vector<SDK::uint8>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<SDK::uint8>((value >> 24) & 0xff));
    bytes.push_back(static_cast<SDK::uint8>((value >> 16) & 0xff));
    bytes.push_back(static_cast<SDK::uint8>((value >> 8) & 0xff));
    bytes.push_back(static_cast<SDK::uint8>(value & 0xff));
}

std::uint32_t PngCrc32(const SDK::uint8* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void AppendPngChunk(std::vector<SDK::uint8>& png, const char type[4],
                    const std::vector<SDK::uint8>& data) {
    AppendBigEndian(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t crcStart = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    AppendBigEndian(png, PngCrc32(png.data() + crcStart, png.size() - crcStart));
}

std::vector<SDK::uint8> EncodeUncompressedRgbaPng(const std::vector<SDK::uint8>& pixels,
                                                  std::uint32_t width,
                                                  std::uint32_t height) {
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    std::vector<SDK::uint8> filtered;
    filtered.reserve((rowBytes + 1) * height);
    for (std::uint32_t row = 0; row < height; ++row) {
        filtered.push_back(0);
        const auto begin = pixels.begin() + static_cast<std::ptrdiff_t>(row * rowBytes);
        filtered.insert(filtered.end(), begin, begin + static_cast<std::ptrdiff_t>(rowBytes));
    }

    std::vector<SDK::uint8> zlib{0x78, 0x01};
    std::size_t offset = 0;
    while (offset < filtered.size()) {
        const std::uint16_t blockSize = static_cast<std::uint16_t>(
            std::min<std::size_t>(65535, filtered.size() - offset));
        zlib.push_back(offset + blockSize == filtered.size() ? 1 : 0);
        zlib.push_back(static_cast<SDK::uint8>(blockSize & 0xff));
        zlib.push_back(static_cast<SDK::uint8>(blockSize >> 8));
        const std::uint16_t inverseSize = static_cast<std::uint16_t>(~blockSize);
        zlib.push_back(static_cast<SDK::uint8>(inverseSize & 0xff));
        zlib.push_back(static_cast<SDK::uint8>(inverseSize >> 8));
        zlib.insert(zlib.end(), filtered.begin() + static_cast<std::ptrdiff_t>(offset),
                    filtered.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
        offset += blockSize;
    }
    std::uint32_t adlerA = 1;
    std::uint32_t adlerB = 0;
    for (const SDK::uint8 byte : filtered) {
        adlerA = (adlerA + byte) % 65521;
        adlerB = (adlerB + adlerA) % 65521;
    }
    AppendBigEndian(zlib, (adlerB << 16) | adlerA);

    std::vector<SDK::uint8> png{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<SDK::uint8> header;
    AppendBigEndian(header, width);
    AppendBigEndian(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    AppendPngChunk(png, "IHDR", header);
    AppendPngChunk(png, "IDAT", zlib);
    AppendPngChunk(png, "IEND", {});
    return png;
}

struct ReachabilityAtlasTexture {
    SDK::UTextureRenderTarget2D* renderTarget = nullptr;
    SDK::UTexture2D* stagingTexture = nullptr;
};

ReachabilityAtlasTexture CreateReachabilityAtlasTexture(
    SDK::UObject* worldContext,
    SDK::int32 width,
    SDK::int32 height,
    const std::vector<MiniMapAtlasCell>& cells) {
    if (!worldContext || width <= 0 || height <= 0) return {};
    std::vector<SDK::uint8> pixels(static_cast<std::size_t>(width) * height * 4, 0);
    // Keep biome hues faint because the minimap is composited over the live game world.
    constexpr SDK::uint8 MINI_MAP_GHOST_ALPHA = 52;
    for (const auto& cell : cells) {
        if (!cell.painted) continue;
        const GhostCellColor color = GetGhostCellColor(cell.room);
        const std::array<SDK::uint8, 4> rgba{color.red, color.green, color.blue, MINI_MAP_GHOST_ALPHA};
        for (SDK::int32 y = cell.top; y < cell.bottom; ++y) {
            for (SDK::int32 x = cell.left; x < cell.right; ++x) {
                const std::size_t pixel = (static_cast<std::size_t>(y) * width + x) * 4;
                std::copy(rgba.begin(), rgba.end(), pixels.begin() + pixel);
            }
        }
    }
    MINI_MAP_GHOST_PIXELS = pixels;
    auto png = EncodeUncompressedRgbaPng(pixels, static_cast<std::uint32_t>(width),
                                         static_cast<std::uint32_t>(height));
    MINI_MAP_GHOST_PNG_BYTES = png;
    SDK::TArray<SDK::uint8> textureBytes(png.data(), static_cast<SDK::int32>(png.size()),
                                         static_cast<SDK::int32>(png.size()));
    auto* stagingTexture = SDK::UKismetRenderingLibrary::ImportBufferAsTexture2D(worldContext, textureBytes);
    if (!stagingTexture) return {};
    auto* renderTarget = SDK::UKismetRenderingLibrary::CreateRenderTarget2D(
        worldContext, width, height, SDK::ETextureRenderTargetFormat::RTF_RGBA8);
    if (!renderTarget) return {nullptr, stagingTexture};
    SDK::UKismetRenderingLibrary::ClearRenderTarget2D(
        worldContext, renderTarget, {0.0f, 0.0f, 0.0f, 0.0f});
    SDK::UCanvas* canvas = nullptr;
    SDK::FVector2D canvasSize{};
    SDK::FDrawToRenderTargetContext context{};
    SDK::UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(
        worldContext, renderTarget, &canvas, &canvasSize, &context);
    if (canvas) {
        canvas->K2_DrawTexture(stagingTexture, {0.0f, 0.0f},
                               {static_cast<float>(width), static_cast<float>(height)},
                               {0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
                               SDK::EBlendMode::BLEND_Opaque, 0.0f, {0.0f, 0.0f});
    }
    SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(worldContext, context);
    return {renderTarget, stagingTexture};
}

std::size_t UpdateReachabilityAtlasCells(
    SDK::UMiniMapBlueprint_C* miniMap,
    SDK::UPBMapManager* mapManager,
    SDK::UPBMapComponent* mapComponent,
    SDK::UTotalMapBlueprint_C* totalMap,
    SDK::EDivideMap mapType,
    const std::vector<ReachableRoomCell>& reachableCells,
    bool refreshReachability = false) {
    if (!miniMap || !mapManager || !mapComponent || !totalMap ||
        MINI_MAP_ATLAS_MAP_TYPE != static_cast<SDK::int32>(mapType) ||
        !MINI_MAP_GHOST_TEXTURE || MINI_MAP_GHOST_TEXTURE_INDEX < 0 ||
        SDK::UObject::GObjects->GetByIndex(MINI_MAP_GHOST_TEXTURE_INDEX) != MINI_MAP_GHOST_TEXTURE) {
        return 0;
    }
    auto* resource = static_cast<SDK::UObject*>(MINI_MAP_GHOST_TEXTURE);
    if (!resource->IsA(SDK::UTextureRenderTarget2D::StaticClass())) return 0;
    auto* renderTarget = static_cast<SDK::UTextureRenderTarget2D*>(resource);

    // PBMapManager owns the authoritative 200x100 traversal grid. Following its current grid coordinate avoids
    // re-deriving the active cell from player/widget coordinates, whose vertical strides deliberately differ.
    SDK::int32 traverseX = -1;
    SDK::int32 traverseY = -1;
    mapManager->GetTraverseCurrent(&traverseX, &traverseY);
    const SDK::int32 currentTraverseIndex =
        traverseX >= 0 && traverseX < 200 && traverseY >= 0 && traverseY < 100
            ? traverseY * 200 + traverseX
            : -1;
    if (!refreshReachability && currentTraverseIndex == MINI_MAP_ATLAS_LAST_TRAVERSE_INDEX) return 0;
    MINI_MAP_ATLAS_LAST_TRAVERSE_INDEX = currentTraverseIndex;

    std::set<std::pair<std::string, std::uint32_t>> reachableKeys;
    if (refreshReachability) {
        for (const auto& cell : reachableCells) reachableKeys.emplace(cell.room, cell.assignment);
    }
    for (auto& cell : MINI_MAP_ATLAS_CELLS) {
        if (refreshReachability) {
            cell.reachable = reachableKeys.contains({cell.room, cell.assignment});
        }
        if (cell.traverseIndex == currentTraverseIndex) cell.explored = true;
    }

    std::vector<std::pair<MiniMapAtlasCell*, bool>> changes;
    for (auto& cell : MINI_MAP_ATLAS_CELLS) {
        const bool shouldPaint = cell.reachable && !cell.explored;
        if (cell.painted != shouldPaint) changes.emplace_back(&cell, shouldPaint);
    }
    if (changes.empty()) return 0;

    SDK::UCanvas* canvas = nullptr;
    SDK::FVector2D canvasSize{};
    SDK::FDrawToRenderTargetContext context{};
    SDK::UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(
        miniMap, renderTarget, &canvas, &canvasSize, &context);
    if (!canvas || !canvas->DefaultTexture) {
        SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(miniMap, context);
        return 0;
    }
    constexpr SDK::uint8 MINI_MAP_GHOST_ALPHA = 52;
    for (auto& [cell, shouldPaint] : changes) {
        const SDK::FVector2D position{static_cast<float>(cell->left), static_cast<float>(cell->top)};
        const SDK::FVector2D size{static_cast<float>(cell->right - cell->left),
                                  static_cast<float>(cell->bottom - cell->top)};
        const GhostCellColor color = GetGhostCellColor(cell->room);
        const SDK::FLinearColor drawColor = ToLinearGhostColor(
            color, shouldPaint ? MINI_MAP_GHOST_ALPHA / 255.0f : 0.0f);
        canvas->K2_DrawTexture(canvas->DefaultTexture, position, size, {0.0f, 0.0f}, {1.0f, 1.0f},
                               drawColor,
                               SDK::EBlendMode::BLEND_Opaque, 0.0f, {0.0f, 0.0f});
        cell->painted = shouldPaint;
        const std::array<SDK::uint8, 4> rgba = shouldPaint
                                                   ? std::array<SDK::uint8, 4>{color.red, color.green, color.blue,
                                                                              MINI_MAP_GHOST_ALPHA}
                                                   : std::array<SDK::uint8, 4>{0, 0, 0, 0};
        for (SDK::int32 y = cell->top; y < cell->bottom; ++y) {
            for (SDK::int32 x = cell->left; x < cell->right; ++x) {
                const std::size_t pixel =
                    (static_cast<std::size_t>(y) * MINI_MAP_GHOST_TEXTURE_WIDTH + x) * 4;
                if (pixel + 4 <= MINI_MAP_GHOST_PIXELS.size()) {
                    std::copy(rgba.begin(), rgba.end(), MINI_MAP_GHOST_PIXELS.begin() + pixel);
                }
            }
        }
    }
    SDK::UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(miniMap, context);
    if constexpr (ENABLE_MAP_DIAGNOSTICS && ENABLE_MINIMAP_CAPTURE_DIAGNOSTICS) {
        MINI_MAP_GHOST_PNG_BYTES = EncodeUncompressedRgbaPng(
            MINI_MAP_GHOST_PIXELS, static_cast<std::uint32_t>(MINI_MAP_GHOST_TEXTURE_WIDTH),
            static_cast<std::uint32_t>(MINI_MAP_GHOST_TEXTURE_HEIGHT));
    }
    return changes.size();
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

bool IsRoomInsideMiniMapWindow(const bloodstained::tracker::generated::RoomMapData* room,
                               SDK::int32 traverseX, SDK::int32 traverseY) {
    if (!room || room->out_of_map || room->width == 0 || room->height == 0 ||
        traverseX < 0 || traverseY < 0) {
        return false;
    }
    const SDK::int32 roomLeft = room->x;
    const SDK::int32 roomRight = roomLeft + static_cast<SDK::int32>(room->width) - 1;
    const SDK::int32 roomBottom = room->z + ROOM_MAP_TO_TRAVERSE_Z;
    const SDK::int32 roomTop = roomBottom + static_cast<SDK::int32>(room->height) - 1;
    return roomRight >= traverseX - MINI_MAP_CELL_CULL_RADIUS &&
           roomLeft <= traverseX + MINI_MAP_CELL_CULL_RADIUS &&
           roomTop >= traverseY - MINI_MAP_CELL_CULL_RADIUS &&
           roomBottom <= traverseY + MINI_MAP_CELL_CULL_RADIUS;
}

bool IsLocationInsideMiniMapWindow(
    const bloodstained::tracker::generated::LocationData& location,
    SDK::int32 traverseX, SDK::int32 traverseY) {
    const auto* room = Tracker::FindRoom(location.room);
    if (!IsRoomInsideMiniMapWindow(room, traverseX, traverseY)) return false;
    if (!location.has_map_position) return true;

    const float mapX = std::clamp(location.map_x, 0.0f, static_cast<float>(room->width));
    const float mapZ = GetLocationMarkerMapZ(location, room->height);
    const SDK::int32 cellX = std::min(static_cast<SDK::int32>(mapX),
                                      static_cast<SDK::int32>(room->width) - 1);
    const SDK::int32 cellZ = std::min(static_cast<SDK::int32>(mapZ),
                                      static_cast<SDK::int32>(room->height) - 1);
    const SDK::int32 locationX = room->x + cellX;
    const SDK::int32 locationY = room->z + ROOM_MAP_TO_TRAVERSE_Z + cellZ;
    return std::abs(locationX - traverseX) <= MINI_MAP_CELL_CULL_RADIUS &&
           std::abs(locationY - traverseY) <= MINI_MAP_CELL_CULL_RADIUS;
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

NativeMapAxes FindNativeRenderAxes(SDK::UPBMapManager* mapManager,
                                   SDK::UPBMapComponent* mapComponent,
                                   SDK::EDivideMap mapType,
                                   SDK::UTotalMapBlueprint_C* totalMap,
                                   std::optional<SDK::EDivideMap> areaMapType = std::nullopt) {
    NativeMapAxes axes{{totalMap->RoomPixelSize.X, 0.0f}, {0.0f, totalMap->RoomPixelSize.Y}};
    bool foundX = false;
    bool foundZ = false;
    for (const auto& room : bloodstained::tracker::generated::ROOMS) {
        if (room.out_of_map || (room.width < 2 && room.height < 2)) continue;
        const SDK::FName roomId = NameFromString(room.name);
        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        if (mapComponent->CheckMapType(area) != areaMapType.value_or(mapType)) continue;

        const SDK::FVector2D first = mapComponent->GetRoomCenterInMapPosition(
            mapType, roomId, 1, totalMap->IconPixelOffset, totalMap->canvasSize);
        if (!foundX && room.width >= 2) {
            const SDK::FVector2D nextX = mapComponent->GetRoomCenterInMapPosition(
                mapType, roomId, 2, totalMap->IconPixelOffset, totalMap->canvasSize);
            axes.x = nextX - first;
            foundX = std::abs(axes.x.X) + std::abs(axes.x.Y) > 0.01f;
        }
        if (!foundZ && room.height >= 2) {
            const SDK::FVector2D nextZ = mapComponent->GetRoomCenterInMapPosition(
                mapType, roomId, room.width + 1, totalMap->IconPixelOffset, totalMap->canvasSize);
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
    SDK::FVector2D position = cellCenter + axes.x * (mapX - static_cast<float>(cellX) - 0.5f) +
                              axes.z * (mapZ - static_cast<float>(cellZ) - 0.5f);
    const auto nativeName = Tracker::FindNativeLocationName(location.id);
    if (nativeName && NormalizeTreasureId(std::string(*nativeName)) == HIDDEN_CHEST_NATIVE_ID) {
        // The actor's room-local Z maps this synthetic marker near the cell's top. Move it toward the chest's
        // visible floor position using the native room axis, so map zoom and aspect ratio remain authoritative.
        position -= axes.z * 0.35f;
    }
    return position;
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
                                      const std::vector<ReachableRoomCell>& reachableCells,
                                      std::vector<TrackedWidgetHandle>& spawnedGhostWidgets) {
    auto* mapManager = map->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return 0;

    auto* texture = CreateGhostMapTileTexture(map);
    if (!texture) return 0;

    auto* traversalSnapshot = static_cast<SDK::UPBSaveGameData*>(
        SDK::UGameplayStatics::SpawnObject(SDK::UPBSaveGameData::StaticClass(), map));
    if (traversalSnapshot) mapManager->OnSerializeGame(traversalSnapshot);
    const bool hasTraversalLedger =
        traversalSnapshot && traversalSnapshot->m_Traverse.Num() == TRAVERSE_WIDTH * TRAVERSE_HEIGHT;

    std::size_t renderedCells = 0;
    std::set<std::tuple<SDK::uint8, SDK::int32, SDK::int32>> renderedPositions;
    std::array<std::optional<GhostMapGeometry>, 4> mapGeometry;
    std::unordered_map<std::string, std::pair<bool, bool>> roomKnowledge;
    for (const ReachableRoomCell& reachableCell : reachableCells) {
        const std::string& roomName = reachableCell.room;
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map) continue;

        const SDK::FName roomId = NameFromString(roomName);
        auto [knowledgeIt, inserted] = roomKnowledge.try_emplace(roomName, false, false);
        if (inserted) {
            knowledgeIt->second.first =
                mapManager->IsKnowledgeRoom(roomId, &knowledgeIt->second.second);
        }
        if (knowledgeIt->second.first && knowledgeIt->second.second) continue;

        const SDK::int32 traverseIndex = GetTraverseIndex(*room, reachableCell.assignment);
        if (hasTraversalLedger && traverseIndex >= 0 && traversalSnapshot->m_Traverse[traverseIndex] != 0) {
            continue;
        }

        const SDK::EAreaID area = mapManager->RoomIdToAreaId(roomId);
        const SDK::EDivideMap mapType = mapComponent->CheckMapType(area);
        auto* totalMap = GetMapForType(map, mapType);
        if (!totalMap || !totalMap->ImageParent_Canvas || totalMap->RoomPixelSize.X <= 0.0f ||
            totalMap->RoomPixelSize.Y <= 0.0f) {
            LOG_MAP_DIAGNOSTIC(LogLevel::Warning, "[Tracker] No active map canvas for reachable room:", roomName);
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

        {
            const SDK::FVector2D nativeCellCenter = mapComponent->GetRoomCenterInMapPosition(
                mapType, roomId, static_cast<SDK::int32>(reachableCell.assignment),
                totalMap->IconPixelOffset, totalMap->canvasSize);
            const SDK::FVector2D cellCenter = MapRenderToCanvas(*mapGeometry[mapIndex], nativeCellCenter);
            const auto positionKey = std::make_tuple(
                static_cast<SDK::uint8>(mapType), static_cast<SDK::int32>(std::lround(cellCenter.X * 100.0f)),
                static_cast<SDK::int32>(std::lround(cellCenter.Y * 100.0f)));
            if (!renderedPositions.insert(positionKey).second) continue;

            auto* image = static_cast<SDK::UImage*>(
                SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), totalMap->ImageParent_Canvas));
            if (!image) continue;
            SetImageTexture(image, texture);
            SetImageColor(image, ToLinearGhostColor(GetGhostCellColor(roomName), 1.0f));

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
    auto* wallMarkerTexture = LoadCookedMarkerTexture(WALL_MARKER_TEXTURE, "wall marker");
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

std::size_t RenderShardRoomMarkers(SDK::UMapManageBlueprint_C* map,
                                   const std::unordered_set<std::string>& shardRooms,
                                   std::vector<TrackedWidgetHandle>& spawnedMarkerWidgets) {
    if (!map || shardRooms.empty()) return 0;

    auto* mapManager = map->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return 0;

    auto* texture = LoadCookedMarkerTexture(SHARD_MARKER_TEXTURE, "shard marker");
    std::array<std::optional<GhostMapGeometry>, 4> geometries;
    std::array<std::optional<SDK::FVector2D>, 4> markerSizes;
    std::size_t rendered = 0;
    for (const std::string& roomName : shardRooms) {
        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map || room->width == 0 || room->height == 0) continue;
        const SDK::FName roomId = NameFromString(roomName);
        const SDK::EDivideMap mapType =
            mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId));
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
            markerSizes[mapIndex] =
                FindNativeRoomMarkerSize(map, mapComponent, mapType, *geometries[mapIndex]);
        }

        const SDK::FVector2D firstCenter = mapComponent->GetRoomCenterInMapPosition(
            mapType, roomId, 1, totalMap->IconPixelOffset, totalMap->canvasSize);
        const SDK::FVector2D lastCenter = mapComponent->GetRoomCenterInMapPosition(
            mapType, roomId, static_cast<SDK::int32>(room->width * room->height),
            totalMap->IconPixelOffset, totalMap->canvasSize);
        const SDK::FVector2D markerCenter =
            MapRenderToCanvas(*geometries[mapIndex], (firstCenter + lastCenter) * 0.5f);

        auto* image = static_cast<SDK::UImage*>(
            SDK::UGameplayStatics::SpawnObject(SDK::UImage::StaticClass(), totalMap->ImageParent_Canvas));
        if (!image) continue;
        if (texture) SetImageTexture(image, texture);
        SetImageColor(image, texture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                     : SDK::FLinearColor{0.2f, 0.9f, 0.35f, 1.0f});
        auto* slot = totalMap->ImageParent_Canvas->AddChildToCanvas(image);
        if (!slot) continue;
        slot->SetAlignment({0.5f, 0.5f});
        slot->SetPosition(markerCenter);
        slot->SetSize(*markerSizes[mapIndex]);
        if (auto* renderSlot =
                SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(totalMap->RenderTargetTotal_Image)) {
            slot->SetZOrder(renderSlot->GetZOrder() + 1);
        }
        SetVisible(image);
        spawnedMarkerWidgets.push_back(TrackWidget(image));
        ++rendered;
    }
    return rendered;
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

    auto* chestTexture = LoadCookedMarkerTexture(CHEST_MARKER_TEXTURE, "chest marker");
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
            axes[mapIndex] = FindNativeRenderAxes(mapManager, mapComponent, mapType, totalMap);
            markerSizes[mapIndex] =
                FindNativeTreasureMarkerSize(map, mapComponent, mapType, *geometries[mapIndex]);
        }

        const auto renderPosition =
            GetStaticLocationMapPosition(mapComponent, mapType, totalMap, *axes[mapIndex], *location);
        if (!renderPosition) continue;
        // The actor point lies at floor height, while the visible native chest glyph is centered slightly above it.
        // The image is rendered at 0.75 scale below, so raise its center by one quarter of that visible height. A
        // full native-child anchor offset overcompensates and puts capacity pickups at the ceiling.
        SDK::FVector2D markerCenter = MapRenderToCanvas(*geometries[mapIndex], *renderPosition);
        markerCenter.Y -= markerSizes[mapIndex]->Y * 0.1875f;
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

SDK::UTreasureLocationMinimapBlueprint_C* SpawnMiniMapMarker(
    SDK::UMiniMapBlueprint_C* miniMap,
    const SDK::FVector2D& position,
    const SDK::FVector2D& size,
    SDK::int32 zOrder) {
    if (!miniMap || !miniMap->Treasure_Panel) return nullptr;
    auto* marker = static_cast<SDK::UTreasureLocationMinimapBlueprint_C*>(
        SDK::UWidgetBlueprintLibrary::Create(
            miniMap,
            SDK::TSubclassOf<SDK::UUserWidget>(
                SDK::UTreasureLocationMinimapBlueprint_C::StaticClass()),
            miniMap->GetOwningPlayer()));
    if (!marker || !marker->Image_30) return nullptr;

    auto* slot = miniMap->Treasure_Panel->AddChildToCanvas(marker);
    if (!slot) return nullptr;
    slot->SetAlignment({0.5f, 0.5f});
    slot->SetPosition(position);
    slot->SetSize(size);
    slot->SetAutoSize(false);
    slot->SetZOrder(zOrder);
    SetVisible(marker);
    SetVisible(marker->Image_30);
    return marker;
}

bool IsWidgetPaintVisible(SDK::UWidget* widget) {
    if (!widget) return false;
    const auto visibility = widget->GetVisibility();
    return visibility == SDK::ESlateVisibility::Visible ||
           visibility == SDK::ESlateVisibility::HitTestInvisible ||
           visibility == SDK::ESlateVisibility::SelfHitTestInvisible;
}

struct MiniMapRenderResult {
    bool anchored = false;
    std::size_t ghostCells = 0;
    std::size_t chests = 0;
    std::size_t walls = 0;
    std::size_t shards = 0;
};

struct MiniMapPanelTransform {
    double mapXToPanelX = 0.0;
    double mapYToPanelX = 0.0;
    double mapXToPanelY = 0.0;
    double mapYToPanelY = 0.0;
    double panelOffsetX = 0.0;
    double panelOffsetY = 0.0;
    double rmsError = 0.0;
    double maximumError = 0.0;
    std::size_t sampleCount = 0;

    SDK::FVector2D Apply(const SDK::FVector2D& mapPosition) const {
        return {
            static_cast<float>(mapXToPanelX * mapPosition.X + mapYToPanelX * mapPosition.Y + panelOffsetX),
            static_cast<float>(mapXToPanelY * mapPosition.X + mapYToPanelY * mapPosition.Y + panelOffsetY),
        };
    }
};

std::optional<MiniMapPanelTransform> FitMiniMapPanelTransform(
    const std::vector<std::pair<SDK::FVector2D, SDK::FVector2D>>& samples) {
    if (samples.size() < 3) return std::nullopt;

    double meanMapX = 0.0;
    double meanMapY = 0.0;
    double meanPanelX = 0.0;
    double meanPanelY = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        meanMapX += mapPosition.X;
        meanMapY += mapPosition.Y;
        meanPanelX += panelPosition.X;
        meanPanelY += panelPosition.Y;
    }
    const double inverseCount = 1.0 / static_cast<double>(samples.size());
    meanMapX *= inverseCount;
    meanMapY *= inverseCount;
    meanPanelX *= inverseCount;
    meanPanelY *= inverseCount;

    double covarianceXX = 0.0;
    double covarianceXY = 0.0;
    double covarianceYY = 0.0;
    double mapXPanelX = 0.0;
    double mapYPanelX = 0.0;
    double mapXPanelY = 0.0;
    double mapYPanelY = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        const double mapX = mapPosition.X - meanMapX;
        const double mapY = mapPosition.Y - meanMapY;
        const double panelX = panelPosition.X - meanPanelX;
        const double panelY = panelPosition.Y - meanPanelY;
        covarianceXX += mapX * mapX;
        covarianceXY += mapX * mapY;
        covarianceYY += mapY * mapY;
        mapXPanelX += mapX * panelX;
        mapYPanelX += mapY * panelX;
        mapXPanelY += mapX * panelY;
        mapYPanelY += mapY * panelY;
    }
    const double determinant = covarianceXX * covarianceYY - covarianceXY * covarianceXY;
    const double covarianceScale = std::max(1.0, covarianceXX * covarianceYY);
    if (std::abs(determinant) <= covarianceScale * 1.0e-10) return std::nullopt;

    MiniMapPanelTransform transform;
    transform.mapXToPanelX =
        (covarianceYY * mapXPanelX - covarianceXY * mapYPanelX) / determinant;
    transform.mapYToPanelX =
        (covarianceXX * mapYPanelX - covarianceXY * mapXPanelX) / determinant;
    transform.mapXToPanelY =
        (covarianceYY * mapXPanelY - covarianceXY * mapYPanelY) / determinant;
    transform.mapYToPanelY =
        (covarianceXX * mapYPanelY - covarianceXY * mapXPanelY) / determinant;
    transform.panelOffsetX = meanPanelX - transform.mapXToPanelX * meanMapX -
                             transform.mapYToPanelX * meanMapY;
    transform.panelOffsetY = meanPanelY - transform.mapXToPanelY * meanMapX -
                             transform.mapYToPanelY * meanMapY;
    transform.sampleCount = samples.size();

    double squaredError = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        const SDK::FVector2D predicted = transform.Apply(mapPosition);
        const double errorX = predicted.X - panelPosition.X;
        const double errorY = predicted.Y - panelPosition.Y;
        const double error = std::sqrt(errorX * errorX + errorY * errorY);
        squaredError += error * error;
        transform.maximumError = std::max(transform.maximumError, error);
    }
    transform.rmsError = std::sqrt(squaredError * inverseCount);
    return transform;
}

std::optional<MiniMapPanelTransform> FitAxisAlignedMiniMapPanelTransform(
    const std::vector<std::pair<SDK::FVector2D, SDK::FVector2D>>& samples) {
    if (samples.size() < 3) return std::nullopt;

    double meanMapX = 0.0;
    double meanMapY = 0.0;
    double meanPanelX = 0.0;
    double meanPanelY = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        meanMapX += mapPosition.X;
        meanMapY += mapPosition.Y;
        meanPanelX += panelPosition.X;
        meanPanelY += panelPosition.Y;
    }
    const double inverseCount = 1.0 / static_cast<double>(samples.size());
    meanMapX *= inverseCount;
    meanMapY *= inverseCount;
    meanPanelX *= inverseCount;
    meanPanelY *= inverseCount;

    double varianceX = 0.0;
    double varianceY = 0.0;
    double covarianceX = 0.0;
    double covarianceY = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        const double mapX = mapPosition.X - meanMapX;
        const double mapY = mapPosition.Y - meanMapY;
        varianceX += mapX * mapX;
        varianceY += mapY * mapY;
        covarianceX += mapX * (panelPosition.X - meanPanelX);
        covarianceY += mapY * (panelPosition.Y - meanPanelY);
    }
    if (varianceX <= 1.0e-6 || varianceY <= 1.0e-6) return std::nullopt;

    MiniMapPanelTransform transform;
    transform.mapXToPanelX = covarianceX / varianceX;
    transform.mapYToPanelY = covarianceY / varianceY;
    transform.panelOffsetX = meanPanelX - transform.mapXToPanelX * meanMapX;
    transform.panelOffsetY = meanPanelY - transform.mapYToPanelY * meanMapY;
    transform.sampleCount = samples.size();
    double squaredError = 0.0;
    for (const auto& [mapPosition, panelPosition] : samples) {
        const SDK::FVector2D predicted = transform.Apply(mapPosition);
        const double errorX = predicted.X - panelPosition.X;
        const double errorY = predicted.Y - panelPosition.Y;
        const double error = std::sqrt(errorX * errorX + errorY * errorY);
        squaredError += error * error;
        transform.maximumError = std::max(transform.maximumError, error);
    }
    transform.rmsError = std::sqrt(squaredError * inverseCount);
    return transform;
}

std::string GetNativeWallMarkerId(std::uint64_t locationId) {
    if (const auto nativeName = Tracker::FindNativeLocationName(locationId)) {
        return NormalizeTreasureId(std::string(*nativeName));
    }
    return std::string("ap_wall_") + std::to_string(locationId);
}

bool IsAuthoritativeWallMarkerId(std::string_view normalizedId) {
    static const std::unordered_set<std::string> markerIds = [] {
        std::unordered_set<std::string> ids;
        for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
            if (location.type == LocationType::WALL && location.has_map_position) {
                ids.insert(GetNativeWallMarkerId(location.id));
            }
        }
        return ids;
    }();
    return markerIds.contains(std::string(normalizedId));
}

MiniMapRenderResult RenderMiniMapTracker(
    SDK::UMiniMapBlueprint_C* miniMap,
    const std::vector<ReachableRoomCell>& reachableCells,
    const std::vector<const bloodstained::tracker::generated::LocationData*>& reachableLocations,
    const std::unordered_set<std::string>& reachableShardRooms,
    std::vector<TrackedWidgetHandle>& activatedMarkerWidgets,
    std::vector<TrackedWidgetHandle>& visibilityWidgets,
    std::vector<TrackedWidgetHandle>& spawnedWidgets,
    std::vector<TrackedWidgetHandle>& spawnedGhostWidgets,
    bool renderGhostCells) {
    MiniMapRenderResult result;
    auto* totalMap = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    auto* mapManager = miniMap ? miniMap->GetMapManager() : nullptr;
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!totalMap || !totalMap->ImageParent_Canvas || !miniMap->Treasure_Panel || !mapManager ||
        !mapComponent) return result;

    const SDK::EDivideMap areaMapType = mapComponent->CheckMapType(mapManager->GetCurrentAreaId());
    // TotalMapBlueprint retains its full-map mapType when embedded in MiniMapBlueprint. Native minimap marker and
    // room coordinates are partitioned by the player's current area instead, so all minimap geometry queries must
    // use that active division or they mix unrelated coordinate spaces.
    const SDK::EDivideMap renderMapType = areaMapType;

    auto geometry = GetMiniMapGeometry(totalMap);
    if (!geometry) return result;
    geometry->markerAnchorCorrection = {};
    NativeMapAxes renderAxes{{totalMap->RoomPixelSize.X, 0.0f}, {0.0f, totalMap->RoomPixelSize.Y}};
    bool foundRenderX = false;
    bool foundRenderZ = false;
    for (const auto& room : bloodstained::tracker::generated::ROOMS) {
        if (room.out_of_map || (foundRenderX && foundRenderZ)) continue;
        const SDK::FName roomId = NameFromString(room.name);
        if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) continue;
        const SDK::FVector2D first = mapComponent->GetRoomCenterInMapPosition(
            renderMapType, roomId, 1, totalMap->IconPixelOffset, totalMap->canvasSize);
        if (!foundRenderX && room.width >= 2) {
            const SDK::FVector2D next = mapComponent->GetRoomCenterInMapPosition(
                renderMapType, roomId, 2, totalMap->IconPixelOffset, totalMap->canvasSize);
            renderAxes.x = next - first;
            foundRenderX = std::abs(renderAxes.x.X) + std::abs(renderAxes.x.Y) > 0.01f;
        }
        if (!foundRenderZ && room.height >= 2) {
            const SDK::FVector2D next = mapComponent->GetRoomCenterInMapPosition(
                renderMapType, roomId, room.width + 1, totalMap->IconPixelOffset,
                totalMap->canvasSize);
            renderAxes.z = next - first;
            foundRenderZ = std::abs(renderAxes.z.X) + std::abs(renderAxes.z.Y) > 0.01f;
        }
    }

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
    std::unordered_set<std::string> wallMarkerIds;
    std::unordered_set<std::string> shardMarkerIds;
    // Missing native marker actors are baked into BloodstainedAP.pak. Runtime code only selects and recolors the
    // widgets that the game created during ordinary level construction.
    for (const auto* location : reachableLocations) {
        if (!location || !location->has_map_position) continue;
        if (location->type == LocationType::WALL) wallMarkerIds.insert(GetNativeWallMarkerId(location->id));
    }
    result.anchored = true;

    auto* chestTexture =
        treasureIds.empty() ? nullptr : LoadCookedMarkerTexture(CHEST_MARKER_TEXTURE, "chest marker");
    auto* wallTexture = wallMarkerIds.empty()
                            ? nullptr
                            : LoadCookedMarkerTexture(WALL_MARKER_TEXTURE, "wall marker");
    auto* shardTexture = shardMarkerIds.empty()
                             ? nullptr
                             : LoadCookedMarkerTexture(SHARD_MARKER_TEXTURE, "shard marker");
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !marker->Image_30) continue;
        const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
        SDK::UTexture2D* texture = nullptr;
        SDK::FLinearColor fallbackColor{};
        if (treasureIds.contains(markerId) && markerId != HIDDEN_CHEST_NATIVE_ID) {
            texture = chestTexture;
            fallbackColor = {0.15f, 0.85f, 1.0f, 1.0f};
            ++result.chests;
        } else if (shardMarkerIds.contains(markerId)) {
            texture = shardTexture;
            fallbackColor = {0.2f, 0.9f, 0.35f, 1.0f};
            ++result.shards;
        } else continue;
        SetVisible(marker);
        SetVisible(marker->Image_30);
        activatedMarkerWidgets.push_back(TrackWidget(marker->Image_30));
        visibilityWidgets.push_back(TrackWidget(marker));
        visibilityWidgets.push_back(TrackWidget(marker->Image_30));
        if (texture) SetImageTexture(marker->Image_30, texture);
        SetImageColor(marker->Image_30, texture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                : fallbackColor);
    }

    // Synthetic collectibles are real children of the native treasure canvas. MiniMapBlueprint owns this panel's
    // render scale and advances its canvas slot by the exact m_MiniLeftTop delta every Tick, so these markers inherit
    // native movement, zoom, clipping, layering and menu reconstruction without a separate paint/calibration path.
    auto findStoredTreasurePosition = [&](std::string_view normalizedId) -> std::optional<SDK::FVector2D> {
        if (!hud->m_MapTreasureIconComponent) return std::nullopt;
        for (const auto& entry : hud->m_MapTreasureIconComponent->TreasureMarkerMapLocation) {
            if (NormalizeTreasureId(entry.Key().ToString()) == normalizedId) return entry.Value();
        }
        return std::nullopt;
    };
    std::vector<std::pair<SDK::FVector2D, SDK::FVector2D>> panelTransformSamples;
    panelTransformSamples.reserve(miniMap->TreasureIconList.Num());
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || marker->GetParent() != miniMap->Treasure_Panel) continue;
        const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
        if (markerId == HIDDEN_CHEST_NATIVE_ID) continue;
        const auto* markerLocation = FindChestLocationByNativeId(markerId);
        if (!markerLocation ||
            mapComponent->CheckMapType(
                mapManager->RoomIdToAreaId(NameFromString(markerLocation->room))) != areaMapType) {
            continue;
        }
        auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker);
        const auto markerMapPosition = findStoredTreasurePosition(markerId);
        if (!slot || !markerMapPosition) continue;
        panelTransformSamples.emplace_back(*markerMapPosition, slot->GetPosition());
    }
    const auto panelTransform = FitMiniMapPanelTransform(panelTransformSamples);
    if (panelTransform) {
        MINI_MAP_PANEL_MAP_X_TO_X = panelTransform->mapXToPanelX;
        MINI_MAP_PANEL_MAP_Y_TO_X = panelTransform->mapYToPanelX;
        MINI_MAP_PANEL_MAP_X_TO_Y = panelTransform->mapXToPanelY;
        MINI_MAP_PANEL_MAP_Y_TO_Y = panelTransform->mapYToPanelY;
        MINI_MAP_PANEL_OFFSET_X = panelTransform->panelOffsetX;
        MINI_MAP_PANEL_OFFSET_Y = panelTransform->panelOffsetY;
        MINI_MAP_PANEL_TRANSFORM_VALID = true;
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Measured minimap panel transform; samples:",
                    panelTransform->sampleCount, "map-to-panel:", panelTransform->mapXToPanelX,
                    panelTransform->mapYToPanelX, panelTransform->mapXToPanelY,
                    panelTransform->mapYToPanelY, "offset:", panelTransform->panelOffsetX,
                    panelTransform->panelOffsetY, "RMS/max error:", panelTransform->rmsError,
                    panelTransform->maximumError);
    } else {
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Could not measure minimap panel transform; usable samples:",
                    panelTransformSamples.size());
    }
    // Fit the game's room-render coordinates directly to the native Treasure_Panel slots. Most stock chests do not
    // have exported room-local coordinates, so use their room center as a bounded-error sample. Across the complete
    // marker set those errors cancel, while the scale and translation remain well constrained. Deliberately forbid
    // shear/rotation: both native coordinate systems use the same map axes, and allowing a sparse or nearly
    // collinear subset to invent a cross-axis term previously projected the atlas completely offscreen.
    std::vector<std::pair<SDK::FVector2D, SDK::FVector2D>> renderToPanelSamples;
    renderToPanelSamples.reserve(panelTransformSamples.size());
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || marker->GetParent() != miniMap->Treasure_Panel) continue;
        const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
        const auto* markerLocation = FindChestLocationByNativeId(markerId);
        auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker);
        if (!markerLocation || !slot ||
            mapComponent->CheckMapType(
                mapManager->RoomIdToAreaId(NameFromString(markerLocation->room))) != areaMapType) {
            continue;
        }
        std::optional<SDK::FVector2D> renderPosition;
        if (markerLocation->has_map_position) {
            renderPosition = GetStaticLocationMapPosition(
                mapComponent, renderMapType, totalMap, renderAxes, *markerLocation);
        } else if (const auto* room = Tracker::FindRoom(markerLocation->room);
                   room && !room->out_of_map && room->width > 0 && room->height > 0) {
            const SDK::FVector2D firstCell = mapComponent->GetRoomCenterInMapPosition(
                renderMapType, NameFromString(markerLocation->room), 1,
                totalMap->IconPixelOffset, totalMap->canvasSize);
            renderPosition = firstCell + renderAxes.x * (static_cast<float>(room->width - 1u) * 0.5f) +
                             renderAxes.z * (static_cast<float>(room->height - 1u) * 0.5f);
        }
        if (renderPosition) renderToPanelSamples.emplace_back(*renderPosition, slot->GetPosition());
    }
    const auto renderToPanelTransform = FitAxisAlignedMiniMapPanelTransform(renderToPanelSamples);
    if (renderToPanelTransform) {
        MINI_MAP_RENDER_TO_PANEL_X_SCALE = renderToPanelTransform->mapXToPanelX;
        MINI_MAP_RENDER_TO_PANEL_Y_SCALE = renderToPanelTransform->mapYToPanelY;
        MINI_MAP_RENDER_TO_PANEL_OFFSET_X = renderToPanelTransform->panelOffsetX;
        MINI_MAP_RENDER_TO_PANEL_OFFSET_Y = renderToPanelTransform->panelOffsetY;
        MINI_MAP_RENDER_TO_PANEL_VALID = true;
        LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Measured room-render directly to minimap panel; samples:",
                    renderToPanelTransform->sampleCount, "scale:",
                    renderToPanelTransform->mapXToPanelX,
                    renderToPanelTransform->mapYToPanelY, "offset:",
                    renderToPanelTransform->panelOffsetX,
                    renderToPanelTransform->panelOffsetY, "RMS/max error:",
                    renderToPanelTransform->rmsError,
                    renderToPanelTransform->maximumError);
    } else {
        MINI_MAP_RENDER_TO_PANEL_VALID = false;
        LOG_MAP_DIAGNOSTIC(LogLevel::File,
                    "[Tracker] Could not measure room-render directly to minimap panel; usable samples:",
                    renderToPanelSamples.size());
    }
    std::string selectedPanelAnchorId;
    SDK::FVector2D selectedPanelAnchorSlot{};
    SDK::FVector2D selectedPanelAnchorMap{};
    SDK::FVector2D selectedPanelMapDelta{};
    SDK::UWidget* selectedPanelAnchorWidget = nullptr;
    auto toTreasurePanelPosition = [&](const SDK::FVector2D& targetMapPosition) -> std::optional<SDK::FVector2D> {
        float nearestDistanceSquared = std::numeric_limits<float>::max();
        std::optional<SDK::FVector2D> resultPosition;
        for (auto* marker : miniMap->TreasureIconList) {
            if (!marker || marker->GetParent() != miniMap->Treasure_Panel) continue;
            const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
            if (markerId == HIDDEN_CHEST_NATIVE_ID) continue;
            const auto* markerLocation = FindChestLocationByNativeId(markerId);
            if (!markerLocation ||
                mapComponent->CheckMapType(
                    mapManager->RoomIdToAreaId(NameFromString(markerLocation->room))) != areaMapType) {
                continue;
            }
            auto* slot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(marker);
            if (!slot) continue;
            const auto markerMapPosition = findStoredTreasurePosition(markerId);
            if (!markerMapPosition) continue;
            const SDK::FVector2D delta = *markerMapPosition - targetMapPosition;
            const float distanceSquared = delta.X * delta.X + delta.Y * delta.Y;
            if (distanceSquared >= nearestDistanceSquared) continue;
            selectedPanelAnchorId = markerId;
            selectedPanelAnchorWidget = marker;
            selectedPanelAnchorSlot = slot->GetPosition();
            selectedPanelAnchorMap = *markerMapPosition;
            if (panelTransform) {
                resultPosition = panelTransform->Apply(targetMapPosition);
                selectedPanelMapDelta = *resultPosition - selectedPanelAnchorSlot;
            } else {
                selectedPanelMapDelta = MapRenderToCanvas(*geometry, targetMapPosition) -
                                        MapRenderToCanvas(*geometry, *markerMapPosition);
                resultPosition = selectedPanelAnchorSlot + selectedPanelMapDelta;
            }
            nearestDistanceSquared = distanceSquared;
        }
        return resultPosition;
    };

    std::unordered_set<std::string> spawnedSyntheticIds;
    for (const auto* location : reachableLocations) {
        if (!location || !location->has_map_position) continue;
        std::string syntheticId;
        SDK::UTexture2D* texture = nullptr;
        SDK::FVector2D size{};
        if (location->type == LocationType::CHEST) {
            const auto nativeName = Tracker::FindNativeLocationName(location->id);
            if (!nativeName) continue;
            syntheticId = NormalizeTreasureId(std::string(*nativeName));
            if (syntheticId != HIDDEN_CHEST_NATIVE_ID) continue;
            texture = chestTexture;
            size = {40.0f, 40.0f};
        } else if (location->type == LocationType::WALL) {
            // Synthetic wall widgets are temporarily suppressed while the native minimap transform is calibrated.
            // Their previous coordinate basis could scale them to room-sized fly-by artifacts. Main-map wall
            // markers and genuinely native minimap wall markers remain available.
            continue;
        } else {
            continue;
        }
        if (!spawnedSyntheticIds.insert(syntheticId).second) continue;
        if (mapComponent->CheckMapType(
                mapManager->RoomIdToAreaId(NameFromString(location->room))) != areaMapType) {
            continue;
        }
        auto nativePosition = findStoredTreasurePosition(syntheticId);
        if (!nativePosition) {
            nativePosition =
                GetStaticLocationMapPosition(mapComponent, renderMapType, totalMap, renderAxes, *location);
        }
        if (!nativePosition) continue;
        const auto panelPosition = toTreasurePanelPosition(*nativePosition);
        if (!panelPosition) continue;
        if (syntheticId == HIDDEN_CHEST_NATIVE_ID && miniMap->Custom_Marker_Panel) {
            SDK::UCanvasPanel* markerPanel = nullptr;
            for (SDK::int32 index = 0; index < miniMap->Treasure_Panel->GetChildrenCount(); ++index) {
                auto* child = miniMap->Treasure_Panel->GetChildAt(index);
                if (!child || !child->IsA(SDK::UCanvasPanel::StaticClass()) ||
                    !child->Name.ToString().starts_with("AP_Marker_Panel_")) {
                    continue;
                }
                markerPanel = static_cast<SDK::UCanvasPanel*>(child);
                break;
            }
            if (!markerPanel) markerPanel = miniMap->Custom_Marker_Panel;
            SDK::UImage* customImage = nullptr;
            SDK::int32 selectedCustomIndex = -1;
            for (SDK::int32 index = 0; index < markerPanel->GetChildrenCount(); ++index) {
                auto* candidate = markerPanel->GetChildAt(index);
                if (!candidate || !candidate->IsA(SDK::UImage::StaticClass()) || IsWidgetPaintVisible(candidate)) {
                    continue;
                }
                customImage = static_cast<SDK::UImage*>(candidate);
                selectedCustomIndex = index;
                break;
            }
            auto* customSlot = customImage ? SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(customImage) : nullptr;
            if (customImage && customSlot) {
                // This is the exact transform used by MiniMapBlueprint.Setup for its baked custom-marker images.
                // Unlike dynamically added Treasure_Panel children, these images already own live Slate widgets.
                const SDK::FVector2D customPosition{
                    nativePosition->X * 1.7075f + 1956.0f,
                    nativePosition->Y * 1.6285f - 287.0f,
                };
                RememberNativeImageBrush(MINI_MAP_NATIVE_BRUSH_OVERRIDES, customImage);
                BORROWED_CUSTOM_MINIMAP_PANEL = TrackWidget(markerPanel);
                BORROWED_CUSTOM_MINIMAP_PANEL_VISIBILITY = markerPanel->GetVisibility();
                if (markerPanel != miniMap->Custom_Marker_Panel) {
                    auto* apPanelSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(markerPanel);
                    auto* stockPanelSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(
                        miniMap->Custom_Marker_Panel);
                    if (apPanelSlot && stockPanelSlot) {
                        apPanelSlot->SetLayout(stockPanelSlot->GetLayout());
                        apPanelSlot->SetAutoSize(stockPanelSlot->GetAutoSize());
                        apPanelSlot->SetZOrder(stockPanelSlot->GetZOrder());
                    }
                }
                SetVisible(markerPanel);
                customSlot->SetPosition(customPosition);
                customSlot->SetSize({42.0f, 42.0f});
                if (texture) SetImageTexture(customImage, texture);
                SetImageColor(customImage, texture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                                   : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
                customImage->Brush.DrawAs = SDK::ESlateBrushDrawType::Image;
                customImage->Brush.ImageSize = size;
                SetImageBrush(customImage, customImage->Brush);
                SynchronizeDynamicImage(customImage);
                SetVisible(customImage);
                BORROWED_CUSTOM_MINIMAP_POSITION = customPosition;
                BORROWED_CUSTOM_MINIMAP_SIZE = {42.0f, 42.0f};
                BORROWED_CUSTOM_MINIMAP_BRUSH = customImage->Brush;
                BORROWED_CUSTOM_MINIMAP_ACTIVE = true;
                activatedMarkerWidgets.push_back(TrackWidget(customImage));
                visibilityWidgets.push_back(TrackWidget(customImage));
                visibilityWidgets.push_back(TrackWidget(markerPanel));
                BORROWED_CUSTOM_MINIMAP_IMAGE = TrackWidget(customImage);
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Borrowed live custom minimap image for hidden chest; index:",
                            selectedCustomIndex, "map:", nativePosition->X, nativePosition->Y, "slot:",
                            customPosition.X, customPosition.Y, "top-left offset:", miniMap->TopLeftOffset.X,
                            miniMap->TopLeftOffset.Y);
                ++result.chests;
                continue;
            }
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] No unused live custom minimap image available; used:",
                        miniMap->CustomMarkerIndex, "total:", miniMap->CustomMarkerIconList.Num());
        }
        auto* marker = SpawnMiniMapMarker(miniMap, *panelPosition, size, 1);
        if (!marker) continue;
        auto* image = marker->Image_30;
        if (texture) SetImageTexture(image, texture);
        SetImageColor(image, texture ? SDK::FLinearColor{1.0f, 1.0f, 1.0f, 1.0f}
                                     : SDK::FLinearColor{0.15f, 0.85f, 1.0f, 1.0f});
        image->Brush.DrawAs = SDK::ESlateBrushDrawType::Image;
        image->Brush.ImageSize = size;
        SetImageBrush(image, image->Brush);
        SynchronizeDynamicImage(image);
        spawnedWidgets.push_back(TrackWidget(marker));
        if (syntheticId == HIDDEN_CHEST_NATIVE_ID) {
            SDK::FVector2D treasurePanelSlotPosition{};
            if (auto* panelSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(miniMap->Treasure_Panel)) {
                treasurePanelSlotPosition = panelSlot->GetPosition();
            }
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Spawned hidden chest as native panel child; anchor:",
                        selectedPanelAnchorId, "anchor slot:", selectedPanelAnchorSlot.X,
                        selectedPanelAnchorSlot.Y, "anchor map:", selectedPanelAnchorMap.X,
                        selectedPanelAnchorMap.Y, "target map:", nativePosition->X, nativePosition->Y,
                        "map delta:", selectedPanelMapDelta.X, selectedPanelMapDelta.Y,
                        "child slot:", panelPosition->X, panelPosition->Y, "panel slot:",
                        treasurePanelSlotPosition.X, treasurePanelSlotPosition.Y, "scale:", miniMap->ScaleRate);
        }
        if (location->type == LocationType::CHEST) ++result.chests;
        else ++result.walls;
    }

    // GetRoomCenterInMapPosition's vertical assignment stride includes the minimap's row-layout spacing and is not
    // the visible room-tile height. RoomPixelSize is the authoritative native tile rectangle (roughly 26.25 x 15
    // here); using the center stride produced the square cells seen in the previous paint-atlas build.
    const SDK::FVector2D renderCellSize = totalMap->RoomPixelSize;
    // GetRoomCenterInMapPosition separates logical rows by the assignment-layout stride (about 28 native units),
    // while the minimap actually draws a room tile at RoomPixelSize.Y (about 15). If raw centers are rasterized
    // unchanged, every pair of adjacent room rows acquires an almost-room-height transparent gap. Compress center
    // positions only; keep halfCell unchanged so the already-correct room rectangle height is preserved.
    const float nativeVerticalStride =
        std::abs(renderAxes.z.Y) > 0.01f ? std::abs(renderAxes.z.Y) : renderCellSize.Y;
    const float atlasVerticalCenterScale =
        nativeVerticalStride > 0.01f ? renderCellSize.Y / nativeVerticalStride : 1.0f;
    auto toAtlasCenter = [atlasVerticalCenterScale](SDK::FVector2D nativeCenter) {
        nativeCenter.Y *= atlasVerticalCenterScale;
        return nativeCenter;
    };

    // Fit raw room-render coordinates into Treasure_Panel space using the game's own chest widgets. The panel owns
    // both continuous player scrolling and discrete room-transition recentering, so the single atlas inherits the
    // complete native minimap motion without per-frame coordinate updates.
    const bool buildGhostAtlas = renderGhostCells && !reachableCells.empty();
    if (buildGhostAtlas) {
        std::set<std::pair<std::string, std::uint32_t>> reachableCellKeys;
        for (const auto& cell : reachableCells) {
            reachableCellKeys.emplace(cell.room, cell.assignment);
        }
        std::set<std::pair<SDK::int32, SDK::int32>> renderedGhostPositions;
        std::vector<MiniMapAtlasCell> atlasCells;

        // The save ledger is a bottom-left-origin 200x100 byte grid. PB_DT_RoomMaster's visible map coordinates
        // already use the same X axis, while its Z origin is 50 rows below the ledger origin. Derive ledger indices
        // directly from static room geometry: anchoring the whole atlas to GetTraverseCurrent during construction
        // made the mapping transition-sensitive because that function can briefly report the departing row.
        auto* traversalSnapshot = static_cast<SDK::UPBSaveGameData*>(
            SDK::UGameplayStatics::SpawnObject(SDK::UPBSaveGameData::StaticClass(), miniMap));
        if (traversalSnapshot) mapManager->OnSerializeGame(traversalSnapshot);
        const bool hasTraversalLedger = traversalSnapshot && traversalSnapshot->m_Traverse.Num() == 20000;
        for (const auto& room : bloodstained::tracker::generated::ROOMS) {
            if (room.out_of_map) continue;
            const SDK::FName roomId = NameFromString(room.name);
            if (mapComponent->CheckMapType(mapManager->RoomIdToAreaId(roomId)) != areaMapType) continue;
            const std::uint32_t cellCount = static_cast<std::uint32_t>(room.width) * room.height;
            for (std::uint32_t assignment = 1; assignment <= cellCount; ++assignment) {
                if (!Tracker::IsRoomCellVisible(room, assignment)) continue;
                const SDK::FVector2D rawCenter = mapComponent->GetRoomCenterInMapPosition(
                    renderMapType, roomId, static_cast<SDK::int32>(assignment),
                    totalMap->IconPixelOffset, totalMap->canvasSize);
                const SDK::FVector2D atlasCenter = toAtlasCenter(rawCenter);
                const auto positionKey = std::make_pair(
                    static_cast<SDK::int32>(std::lround(atlasCenter.X * 100.0f)),
                    static_cast<SDK::int32>(std::lround(atlasCenter.Y * 100.0f)));
                if (!renderedGhostPositions.insert(positionKey).second) continue;
                const bool reachable = reachableCellKeys.contains({std::string(room.name), assignment});
                const SDK::int32 traverseIndex = GetTraverseIndex(room, assignment);
                const bool explored = hasTraversalLedger && traverseIndex >= 0 &&
                                      traversalSnapshot->m_Traverse[traverseIndex] != 0;
                atlasCells.push_back({std::string(room.name), assignment, traverseIndex, atlasCenter,
                                      0, 0, 0, 0, reachable, explored, reachable && !explored});
            }
        }
        if (atlasCells.empty()) return result;

        const SDK::FVector2D halfCell = renderCellSize * 0.5f;
        SDK::FVector2D minimum = atlasCells.front().center - halfCell;
        SDK::FVector2D maximum = atlasCells.front().center + halfCell;
        for (const auto& cell : atlasCells) {
            minimum.X = std::min(minimum.X, cell.center.X - halfCell.X);
            minimum.Y = std::min(minimum.Y, cell.center.Y - halfCell.Y);
            maximum.X = std::max(maximum.X, cell.center.X + halfCell.X);
            maximum.Y = std::max(maximum.Y, cell.center.Y + halfCell.Y);
        }
        const SDK::FVector2D atlasSize = maximum - minimum;
        constexpr float MAX_ATLAS_TEXTURE_DIMENSION = 1024.0f;
        const float textureScale = std::min(
            1.0f, MAX_ATLAS_TEXTURE_DIMENSION / std::max(atlasSize.X, atlasSize.Y));
        const SDK::int32 columnCount = std::max<SDK::int32>(
            1, static_cast<SDK::int32>(std::lround(atlasSize.X / renderCellSize.X)));
        const SDK::int32 rowCount = std::max<SDK::int32>(
            1, static_cast<SDK::int32>(std::lround(atlasSize.Y / renderCellSize.Y)));
        const SDK::int32 pixelPitchX = std::max<SDK::int32>(
            1, static_cast<SDK::int32>(std::lround(renderCellSize.X * textureScale)));
        const SDK::int32 pixelPitchY = std::max<SDK::int32>(
            1, static_cast<SDK::int32>(std::lround(renderCellSize.Y * textureScale)));
        const SDK::int32 textureWidth = columnCount * pixelPitchX;
        const SDK::int32 textureHeight = rowCount * pixelPitchY;
        const SDK::FVector2D firstCenter = minimum + renderCellSize * 0.5f;
        for (auto& cell : atlasCells) {
            const SDK::int32 column = static_cast<SDK::int32>(
                std::lround((cell.center.X - firstCenter.X) / renderCellSize.X));
            const SDK::int32 row = static_cast<SDK::int32>(
                std::lround((cell.center.Y - firstCenter.Y) / renderCellSize.Y));
            cell.left = std::clamp(column * pixelPitchX, 0, textureWidth);
            cell.top = std::clamp(row * pixelPitchY, 0, textureHeight);
            cell.right = std::clamp(cell.left + std::max<SDK::int32>(1, pixelPitchX - 1), 0, textureWidth);
            cell.bottom = std::clamp(cell.top + std::max<SDK::int32>(1, pixelPitchY - 1), 0, textureHeight);
        }
        const ReachabilityAtlasTexture atlas = CreateReachabilityAtlasTexture(
            miniMap, textureWidth, textureHeight, atlasCells);
        auto* atlasTexture = atlas.renderTarget
                                 ? static_cast<SDK::UObject*>(atlas.renderTarget)
                                 : static_cast<SDK::UObject*>(atlas.stagingTexture);
        if (atlasTexture && atlas.stagingTexture) {
            auto* atlasBrush = static_cast<SDK::USlateBrushAsset*>(
                SDK::UGameplayStatics::SpawnObject(SDK::USlateBrushAsset::StaticClass(), miniMap));
            if (atlasBrush) {
                atlasBrush->Brush = SDK::UWidgetBlueprintLibrary::MakeBrushFromTexture(
                    atlas.stagingTexture, textureWidth, textureHeight);
                atlasBrush->Brush.ResourceObject = atlasTexture;
                atlasBrush->Brush.bHasUObject = 1;
                // FPaintContext takes a brush asset rather than a value brush. Neither SpawnObject's Outer nor a
                // collapsed UImage is a GC reference to that transient asset, so it was repeatedly collected between
                // Tick and NativePaint. Root both resources for exactly the lifetime of this atlas; cleanup removes
                // the root flags before dropping the handles.
                atlasBrush->Flags |= SDK::EObjectFlags::MarkAsRootSet;
                atlasTexture->Flags |= SDK::EObjectFlags::MarkAsRootSet;
                MINI_MAP_GHOST_PAINT_BRUSH = atlasBrush;
                MINI_MAP_GHOST_PAINT_BRUSH_INDEX = atlasBrush->Index;
                MINI_MAP_GHOST_TEXTURE = atlasTexture;
                MINI_MAP_GHOST_TEXTURE_INDEX = atlasTexture->Index;
                MINI_MAP_GHOST_TEXTURE_WIDTH = textureWidth;
                MINI_MAP_GHOST_TEXTURE_HEIGHT = textureHeight;
                MINI_MAP_GHOST_RENDER_MINIMUM = minimum;
                MINI_MAP_GHOST_RENDER_MAXIMUM = maximum;
                MINI_MAP_ATLAS_CELLS = std::move(atlasCells);
                MINI_MAP_ATLAS_MAP_TYPE = static_cast<SDK::int32>(areaMapType);
                result.ghostCells = static_cast<std::size_t>(std::count_if(
                    MINI_MAP_ATLAS_CELLS.begin(), MINI_MAP_ATLAS_CELLS.end(),
                    [](const MiniMapAtlasCell& cell) { return cell.painted; }));
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Prepared native-paint minimap reachability atlas:",
                            result.ghostCells, "reachable unexplored cells of",
                            MINI_MAP_ATLAS_CELLS.size(), "valid cells into", textureWidth, "x", textureHeight,
                            "texture; render bounds:", minimum.X, minimum.Y, maximum.X, maximum.Y,
                            "render cell size:", renderCellSize.X, renderCellSize.Y,
                            "integer texture pitch:", pixelPitchX, pixelPitchY,
                            "vertical center stride/scale:", nativeVerticalStride,
                            atlasVerticalCenterScale);
            }
        }
    }

    return result;
}

}  // namespace

InGameTracker& InGameTracker::Instance() {
    static InGameTracker instance;
    return instance;
}

void InGameTracker::InvalidateReachability(std::string_view reason) {
    if (!reachabilityDirty_) LOG_MAP_DIAGNOSTIC("[Tracker] Invalidated logical traversal:", std::string(reason));
    reachabilityDirty_ = true;
    mainMapDirty_ = true;
    miniMapDirty_ = true;
    miniMapGhostsDirty_ = true;
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
    mainMapDirty_ = true;
    miniMapDirty_ = true;
    ReconcileFinishedTreasureMarkers(Archipelago::ConnectedInstance());
}

void InGameTracker::ResetConnection() {
    ClearMainMapMarkers();
    ClearMiniMapMarkers();
    inventorySynchronized_ = false;
    reachabilityDirty_ = true;
    miniMapGhostsDirty_ = true;
    pendingGhostMap_ = nullptr;
    activeMainMap_ = nullptr;
    activeMiniMap_ = nullptr;
    activeMiniMapIndex_ = -1;
    activeMiniMapCanvas_ = nullptr;
    activeMiniMapCanvasIndex_ = -1;
    miniMapCanvasCalibrationValid_ = false;
    miniMapCalibrationStableFrames_ = 0;
    miniMapPaintRefreshRequested_ = false;
    pendingWallLocationIds_.clear();
    reachableRooms_.clear();
    reachableCells_.clear();
    RECONCILED_TREASURE_COMPONENT_INDEX = -1;
    RECONCILED_FINISHED_TREASURE_IDS.clear();
    REGISTERED_EXTRA_TREASURE_COMPONENT_INDEX = -1;
    mainMapDirty_ = true;
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
    mainMapDirty_ = true;
    miniMapDirty_ = true;
    miniMapGhostsDirty_ = true;
    LOG_MAP_DIAGNOSTIC("[Tracker] Loaded in-game tracking display mode:", savedMode);
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
    mainMapDirty_ = true;
    miniMapDirty_ = true;
    miniMapGhostsDirty_ = true;
    LOG_MAP_DIAGNOSTIC("[Tracker] Set in-game tracking display mode:", modeValue);
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
    pendingShardRooms_.clear();
}

void InGameTracker::ClearMiniMapMarkers(bool clearGhosts) {
    miniMapWallLocationIds_.clear();
    miniMapShardRooms_.clear();
    miniMapTreasureIds_.clear();
    miniMapVisibilityWidgets_.clear();

    RestoreNativeImageBrushes(MINI_MAP_NATIVE_BRUSH_OVERRIDES);
    activatedMiniMapMarkerWidgets_.clear();

    for (const auto& markerWidget : spawnedMiniMapWidgets_) {
        if (auto* widget = ResolveWidget(markerWidget)) widget->RemoveFromParent();
    }
    spawnedMiniMapWidgets_.clear();
    if (clearGhosts) {
        for (const auto& ghostWidget : spawnedMiniMapGhostWidgets_) {
            if (auto* widget = ResolveWidget(ghostWidget)) widget->RemoveFromParent();
        }
        spawnedMiniMapGhostWidgets_.clear();
        if (MINI_MAP_GHOST_PAINT_BRUSH && MINI_MAP_GHOST_PAINT_BRUSH_INDEX >= 0 &&
            SDK::UObject::GObjects->GetByIndex(MINI_MAP_GHOST_PAINT_BRUSH_INDEX) ==
                MINI_MAP_GHOST_PAINT_BRUSH) {
            auto* brushObject = static_cast<SDK::UObject*>(MINI_MAP_GHOST_PAINT_BRUSH);
            brushObject->Flags = static_cast<SDK::EObjectFlags>(
                static_cast<SDK::int32>(brushObject->Flags) &
                ~static_cast<SDK::int32>(SDK::EObjectFlags::MarkAsRootSet));
        }
        if (MINI_MAP_GHOST_TEXTURE && MINI_MAP_GHOST_TEXTURE_INDEX >= 0 &&
            SDK::UObject::GObjects->GetByIndex(MINI_MAP_GHOST_TEXTURE_INDEX) == MINI_MAP_GHOST_TEXTURE) {
            auto* textureObject = static_cast<SDK::UObject*>(MINI_MAP_GHOST_TEXTURE);
            textureObject->Flags = static_cast<SDK::EObjectFlags>(
                static_cast<SDK::int32>(textureObject->Flags) &
                ~static_cast<SDK::int32>(SDK::EObjectFlags::MarkAsRootSet));
        }
        MINI_MAP_GHOST_PAINT_BRUSH = nullptr;
        MINI_MAP_GHOST_PAINT_BRUSH_INDEX = -1;
        MINI_MAP_GHOST_TEXTURE = nullptr;
        MINI_MAP_GHOST_TEXTURE_INDEX = -1;
        MINI_MAP_GHOST_TEXTURE_WIDTH = 0;
        MINI_MAP_GHOST_TEXTURE_HEIGHT = 0;
        MINI_MAP_GHOST_RENDER_MINIMUM = {};
        MINI_MAP_GHOST_RENDER_MAXIMUM = {};
        MINI_MAP_GHOST_LAST_PAINT_MINIMUM = {};
        MINI_MAP_GHOST_LAST_PAINT_MAXIMUM = {};
        MINI_MAP_GHOST_LAST_CANVAS_MINIMUM = {};
        MINI_MAP_GHOST_LAST_CANVAS_MAXIMUM = {};
        MINI_MAP_PANEL_TRANSFORM_VALID = false;
        MINI_MAP_RENDER_TO_PANEL_VALID = false;
        MINI_MAP_GHOST_PNG_BYTES.clear();
        MINI_MAP_GHOST_PIXELS.clear();
        MINI_MAP_ATLAS_CELLS.clear();
        MINI_MAP_ATLAS_MAP_TYPE = -1;
        MINI_MAP_ATLAS_LAST_TRAVERSE_INDEX = -2;
    }
    if (auto* borrowed = ResolveWidget(BORROWED_CUSTOM_MINIMAP_IMAGE)) SetHidden(borrowed);
    BORROWED_CUSTOM_MINIMAP_IMAGE = {};
    if (auto* panel = ResolveWidget(BORROWED_CUSTOM_MINIMAP_PANEL)) {
        panel->SetVisibility(BORROWED_CUSTOM_MINIMAP_PANEL_VISIBILITY);
    }
    BORROWED_CUSTOM_MINIMAP_PANEL = {};
    BORROWED_CUSTOM_MINIMAP_ACTIVE = false;
}

void InGameTracker::ApplyDeferredGhostMap(void* mapWidget) {
    auto* map = static_cast<SDK::UMapManageBlueprint_C*>(mapWidget);
    if (activeMainMap_ != map) {
        ClearMainMapMarkers();
        activeMainMap_ = map;
        mainMapDirty_ = true;
    }
    if (mainMapDirty_ && IsMainMapEnabled() && Archipelago::ConnectedInstance() && inventorySynchronized_) {
        ApplyMapMarkers(map);
    }
    if (pendingGhostMap_ != mapWidget) return;

    if (!IsMainMapEnabled() || !Archipelago::ConnectedInstance() || !inventorySynchronized_) {
        pendingGhostMap_ = nullptr;
        pendingWallLocationIds_.clear();
        pendingSyntheticTreasureIds_.clear();
        pendingShardRooms_.clear();
        return;
    }
    if (!HasLaidOutMap(map)) return;

    const std::size_t ghostCells =
        RenderReachableRoomGhosts(map, reachableCells_, spawnedMainMapGhostWidgets_);
    const std::size_t wallMarkers =
        RenderWallLocationMarkers(map, pendingWallLocationIds_, spawnedWallMarkerWidgets_);
    const std::size_t syntheticChests =
        RenderSyntheticTreasureMarkers(map, pendingSyntheticTreasureIds_, spawnedWallMarkerWidgets_);
    const std::size_t shardRooms =
        RenderShardRoomMarkers(map, pendingShardRooms_, spawnedWallMarkerWidgets_);
    LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Rendered native main-map tracking:", ghostCells,
                "reachable cells,", wallMarkers, "walls,", syntheticChests,
                "synthetic chests, and", shardRooms, "shard rooms");
    pendingGhostMap_ = nullptr;
    pendingWallLocationIds_.clear();
    pendingSyntheticTreasureIds_.clear();
    pendingShardRooms_.clear();
}

void InGameTracker::ApplyMapMarkers(void* mapWidget) {
    auto* archipelago = Archipelago::ConnectedInstance();
    auto* map = static_cast<SDK::UMapManageBlueprint_C*>(mapWidget);
    activeMainMap_ = map;
    ReconcileFinishedTreasureMarkers(archipelago);
    ClearMainMapMarkers();
    if (!IsMainMapEnabled() || !archipelago || !map) return;
    pendingWallLocationIds_.clear();
    pendingSyntheticTreasureIds_.clear();
    pendingShardRooms_.clear();

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
            reachableCells_ = BuildReachableRoomCells(reachableRooms_);
            reachabilityDirty_ = false;
            LOG_MAP_DIAGNOSTIC("[Tracker] Recomputed logical traversal; reachable map rooms:", reachableRooms_.size());
        }
        pendingGhostMap_ = map;
    }
    const auto reachableLocations =
        tracker.GetReachableMissingLocations(difficulty, archipelago->GetMissingLocationIds());

    std::unordered_set<std::string> treasureIds;
    std::string reachableTreasureLocations;
    std::unordered_set<std::string> markedRooms;
    for (const auto* location : reachableLocations) {
        const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
        if (!nativeLocationName ||
            archipelago->WasLocationClearedLocally(std::string(*nativeLocationName)) ||
            !archipelago->IsMissingLocation(std::string(*nativeLocationName), location->id)) {
            continue;
        }
        if (location->type == LocationType::CHEST) {
            treasureIds.insert(NormalizeTreasureId(std::string(*nativeLocationName)));
            if constexpr (ENABLE_MAP_DIAGNOSTICS) {
                if (!reachableTreasureLocations.empty()) reachableTreasureLocations += ", ";
                reachableTreasureLocations += location->name;
            }
        } else if (location->type == LocationType::WALL) {
            pendingWallLocationIds_.insert(location->id);
        } else if (location->type == LocationType::ENEMY) {
            for (const std::string_view room : tracker.GetReachableEnemyRooms(*location, difficulty)) {
                markedRooms.insert(std::string(room));
            }
        }
    }

    std::unordered_set<std::string> materializedTreasureIds;
    std::string markedTreasureIds;
    auto* chestMarkerTexture =
        treasureIds.empty() ? nullptr : LoadCookedMarkerTexture(CHEST_MARKER_TEXTURE, "chest marker");
    for (auto* marker : map->TreasureMarkerList) {
        if (!marker || !marker->Image_30) continue;
        const std::string treasureId = NormalizeTreasureId(marker->treasureID.ToString());
        if (!treasureIds.contains(treasureId)) continue;
        // Use one renderer for every chest with an extracted physical position. Mixing native treasure widgets with
        // synthetic capacity-pickup markers produces irreconcilable differences in scale, anchoring and lifecycle.
        // Chests whose actors could not be extracted remain on this native fallback path.
        if (HasStaticChestPosition(treasureId)) {
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
        if constexpr (ENABLE_MAP_DIAGNOSTICS) {
            if (!markedTreasureIds.empty()) markedTreasureIds += ", ";
            markedTreasureIds += marker->treasureID.ToString();
        }
        materializedTreasureIds.insert(NormalizeTreasureId(marker->treasureID.ToString()));
    }
    SuppressFinishedTreasureMarkers(map->TreasureMarkerList);

    pendingShardRooms_ = std::move(markedRooms);

    if constexpr (ENABLE_MAP_DIAGNOSTICS) {
        if (!markedTreasureIds.empty()) {
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Materialized in-logic treasure IDs:", markedTreasureIds);
        }
        if (!reachableTreasureLocations.empty()) {
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Reachable missing chest checks:", reachableTreasureLocations);
        }
    }
    std::string missingTreasureMarkers;
    for (const auto& treasureId : treasureIds) {
        if (materializedTreasureIds.contains(treasureId)) continue;
        pendingSyntheticTreasureIds_.insert(treasureId);
        if constexpr (ENABLE_MAP_DIAGNOSTICS) {
            if (!missingTreasureMarkers.empty()) missingTreasureMarkers += ", ";
            missingTreasureMarkers += treasureId;
        }
    }
    if constexpr (ENABLE_MAP_DIAGNOSTICS) {
        if (!missingTreasureMarkers.empty()) {
            LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] In-logic chest IDs without native marker widgets:",
                        missingTreasureMarkers);
        }
    }
    mainMapDirty_ = false;
}

void InGameTracker::ApplyMiniMap(void* miniMapWidget) {
    auto* miniMap = static_cast<SDK::UMiniMapBlueprint_C*>(miniMapWidget);
    auto* currentCanvas = miniMap ? miniMap->TotalMapBlueprint : nullptr;
    // Menu transitions can reconstruct/synchronize these Blueprint widgets without changing their UObject pointers,
    // which clears the script-paint dispatch bit. Reassert it every native Tick so custom paint resumes on return.
    if (miniMap) miniMap->bHasScriptImplementedPaint = 1;
    if (currentCanvas) currentCanvas->bHasScriptImplementedPaint = 1;
    const bool samePaintSurface = activeMiniMap_ == miniMap && activeMiniMapCanvas_ == currentCanvas;
    const bool preserveCalibration = miniMapPaintRefreshRequested_ && samePaintSurface;
    if (!samePaintSurface || miniMapPaintRefreshRequested_) {
        ClearMiniMapMarkers(!preserveCalibration);
        if (!preserveCalibration) miniMapGhostsDirty_ = true;
        activeMiniMap_ = miniMap;
        activeMiniMapIndex_ = miniMap ? miniMap->Index : -1;
        activeMiniMapCanvas_ = currentCanvas;
        activeMiniMapCanvasIndex_ = currentCanvas ? currentCanvas->Index : -1;
        ReleaseRootedObject(miniMapChestBrush_, miniMapChestBrushIndex_);
        ForgetObject(miniMapChestTexture_, miniMapChestTextureIndex_);
        ReleaseRootedObject(miniMapWallBrush_, miniMapWallBrushIndex_);
        ForgetObject(miniMapWallTexture_, miniMapWallTextureIndex_);
        ReleaseRootedObject(miniMapShardBrush_, miniMapShardBrushIndex_);
        ForgetObject(miniMapShardTexture_, miniMapShardTextureIndex_);
        if (!preserveCalibration) {
            miniMapCanvasCalibrationValid_ = false;
            miniMapCanvasCalibrationX_ = 0.0f;
            miniMapCanvasCalibrationY_ = 0.0f;
            miniMapCalibrationMapX_ = 0.0f;
            miniMapCalibrationMapY_ = 0.0f;
            miniMapCalibrationAnchorPanelX_ = 0.0f;
            miniMapCalibrationAnchorPanelY_ = 0.0f;
            miniMapCalibrationCandidatePanelX_ = 0.0f;
            miniMapCalibrationCandidatePanelY_ = 0.0f;
            miniMapCalibrationStableFrames_ = 0;
        }
        miniMapPaintRefreshRequested_ = false;
        if (!preserveCalibration) activeMiniMapType_ = -1;
        miniMapDirty_ = true;
        if constexpr (ENABLE_MAP_DIAGNOSTICS) {
            if (miniMap) {
                // The static pak extends MapManageBlueprint's cooked registry. Blueprint Setup should pair the
                // AP-only IDs with existing cooked marker widgets before this hook runs.
                std::string tailIds;
                const SDK::int32 firstTailIndex = std::max<SDK::int32>(0, miniMap->TreasureIconList.Num() - 4);
                for (SDK::int32 index = firstTailIndex; index < miniMap->TreasureIconList.Num(); ++index) {
                    auto* marker = miniMap->TreasureIconList[index];
                    if (!tailIds.empty()) tailIds += ", ";
                    tailIds += marker ? marker->treasureID.ToString() : "<null>";
                }
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Native minimap registry after Blueprint Setup; list:",
                                   miniMap->TreasureIconList.Num(), "map:", miniMap->TreasureIconMap.Num(),
                                   "tail:", tailIds);
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Enabled clipped native map-content paint dispatch");
            }
        }
    }

    auto* archipelago = Archipelago::ConnectedInstance();
    ReconcileFinishedTreasureMarkers(archipelago);
    if (!IsMiniMapEnabled() || !archipelago || !inventorySynchronized_ || !miniMap || !currentCanvas ||
        !currentCanvas->ImageParent_Canvas || !miniMap->Treasure_Panel || !miniMap->Frame_Image ||
        !IsWidgetPaintVisible(miniMap) || !IsWidgetPaintVisible(miniMap->Frame_Image)) {
        return;
    }

    auto* mapManager = miniMap->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return;
    RegisterExtraNativeTreasureLocations(hud->m_MapTreasureIconComponent);

    const auto currentMapType =
        static_cast<SDK::int32>(mapComponent->CheckMapType(mapManager->GetCurrentAreaId()));
    if (activeMiniMapType_ != currentMapType) {
        ClearMiniMapMarkers();
        activeMiniMapType_ = currentMapType;
        miniMapDirty_ = true;
        miniMapGhostsDirty_ = true;
    }
    CaptureMiniMapDiagnostics(miniMap, mapComponent,
                              static_cast<SDK::EDivideMap>(currentMapType));

    bool ghostBrushValid = IsLiveBrush(
        static_cast<SDK::USlateBrushAsset*>(MINI_MAP_GHOST_PAINT_BRUSH),
        MINI_MAP_GHOST_PAINT_BRUSH_INDEX,
        static_cast<SDK::UObject*>(MINI_MAP_GHOST_TEXTURE), MINI_MAP_GHOST_TEXTURE_INDEX);
    bool ghostTextureValid = IsLiveObject(
        static_cast<SDK::UObject*>(MINI_MAP_GHOST_TEXTURE), MINI_MAP_GHOST_TEXTURE_INDEX);
    if (!miniMapGhostsDirty_ && (!ghostBrushValid || !ghostTextureValid)) {
        // Never attach a replacement brush to a retained texture after Slate has reclaimed either half of the pair.
        // During room streaming that texture can remain in GObjects briefly while its render resource is already
        // pending kill; DrawBox then dereferences the dying resource. Discard both handles and rebuild the atlas.
        ClearMiniMapMarkers(true);
        ghostBrushValid = false;
        ghostTextureValid = false;
        miniMapGhostsDirty_ = true;
        miniMapDirty_ = true;
        LOG_MAP_DIAGNOSTIC(LogLevel::File,
                    "[Tracker] Discarded stale minimap atlas resources for a full rebuild");
    }

    if (ghostTextureValid && MINI_MAP_ATLAS_MAP_TYPE == currentMapType) {
        UpdateReachabilityAtlasCells(
            miniMap, mapManager, mapComponent, currentCanvas,
            static_cast<SDK::EDivideMap>(currentMapType), reachableCells_);
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
            reachableCells_ = BuildReachableRoomCells(reachableRooms_);
            reachabilityDirty_ = false;
        }

        std::vector<const bloodstained::tracker::generated::LocationData*> reachableLocations;
        std::unordered_set<std::string> reachableShardRooms;
        for (const auto* location :
             tracker.GetReachableMissingLocations(difficulty, archipelago->GetMissingLocationIds())) {
            const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
            if (!nativeLocationName ||
                archipelago->WasLocationClearedLocally(std::string(*nativeLocationName)) ||
                !archipelago->IsMissingLocation(std::string(*nativeLocationName), location->id)) {
                continue;
            }
            reachableLocations.push_back(location);
            if (location->type == LocationType::ENEMY) {
                for (const std::string_view room : tracker.GetReachableEnemyRooms(*location, difficulty)) {
                    reachableShardRooms.insert(std::string(room));
                }
            }
        }

        bool renderGhostCells = miniMapGhostsDirty_;
        if (renderGhostCells && ghostTextureValid && MINI_MAP_ATLAS_MAP_TYPE == currentMapType) {
            const std::size_t patchedCells = UpdateReachabilityAtlasCells(
                miniMap, mapManager, mapComponent, currentCanvas,
                static_cast<SDK::EDivideMap>(currentMapType), reachableCells_, true);
            miniMapGhostsDirty_ = false;
            renderGhostCells = false;
            if (patchedCells > 0) {
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Patched retained minimap reachability atlas:",
                            patchedCells, "cells");
            }
        }
        ClearMiniMapMarkers(renderGhostCells);
        const MiniMapRenderResult result = RenderMiniMapTracker(
            miniMap, reachableCells_, reachableLocations, reachableShardRooms,
            activatedMiniMapMarkerWidgets_, miniMapVisibilityWidgets_, spawnedMiniMapWidgets_,
            spawnedMiniMapGhostWidgets_, renderGhostCells);
        // Menu reconstruction can invoke us once before the native minimap panels are usable. Do not consume the
        // pending grid rebuild on that incomplete pass; the following anchored pass must recreate the atlas.
        if (renderGhostCells && result.anchored) miniMapGhostsDirty_ = false;
        for (const auto* location : reachableLocations) {
            const auto nativeLocationName = Tracker::FindNativeLocationName(location->id);
            if (!nativeLocationName) continue;
            if (location->type == LocationType::CHEST) {
                miniMapTreasureIds_.insert(NormalizeTreasureId(std::string(*nativeLocationName)));
            } else if (location->type == LocationType::WALL) {
                miniMapWallLocationIds_.insert(location->id);
            }
        }
        miniMapShardRooms_ = std::move(reachableShardRooms);
        miniMapDirty_ = !result.anchored;
        if constexpr (ENABLE_MAP_DIAGNOSTICS) {
            static void* lastLoggedMiniMap = nullptr;
            static std::size_t lastLoggedGhostCells = std::numeric_limits<std::size_t>::max();
            static std::size_t lastLoggedChests = std::numeric_limits<std::size_t>::max();
            static std::size_t lastLoggedWalls = std::numeric_limits<std::size_t>::max();
            static std::size_t lastLoggedShards = std::numeric_limits<std::size_t>::max();
            static bool lastLoggedAnchored = false;
            if (lastLoggedMiniMap != miniMap || lastLoggedGhostCells != result.ghostCells ||
                lastLoggedChests != result.chests || lastLoggedWalls != result.walls ||
                lastLoggedShards != result.shards || lastLoggedAnchored != result.anchored) {
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Rebuilt native minimap tracking:", result.ghostCells,
                            "reachable cells,", result.chests, "chests,", result.walls, "walls, and",
                            result.shards, "shard rooms; anchored:", result.anchored);
                lastLoggedMiniMap = miniMap;
                lastLoggedGhostCells = result.ghostCells;
                lastLoggedChests = result.chests;
                lastLoggedWalls = result.walls;
                lastLoggedShards = result.shards;
                lastLoggedAnchored = result.anchored;
            }
        }
        if (miniMapDirty_) return;
    }

    // UpdateIcons is authoritative for native layout and runs before this post-Tick hook. Only override widgets
    // inside the largest possible minimap window. Reasserting every reachable marker made Slate retain and process
    // the entire castle's marker set even though the minimap clips all but a handful of them.
    SDK::int32 currentTraverseX = -1;
    SDK::int32 currentTraverseY = -1;
    mapManager->GetTraverseCurrent(&currentTraverseX, &currentTraverseY);

    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker) continue;
        const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
        const auto* location = FindLocationByNativeId(markerId);
        const bool insideWindow = location &&
            IsLocationInsideMiniMapWindow(*location, currentTraverseX, currentTraverseY);
        if (insideWindow && RECONCILED_FINISHED_TREASURE_IDS.contains(markerId)) {
            if (marker->Image_30) SetCollapsed(marker->Image_30);
            SetCollapsed(marker);
            continue;
        }
        if (markerId == HIDDEN_CHEST_NATIVE_ID) {
            // This repurposed cooked slot is laid out at the stale registry position. A synthetic child of the same
            // native treasure panel owns this marker, so suppress the stale slot to avoid a duplicate.
            if (insideWindow) {
                if (marker->Image_30) SetCollapsed(marker->Image_30);
                SetCollapsed(marker);
            }
            continue;
        }
        const bool syntheticWall = IsAuthoritativeWallMarkerId(markerId);
        const bool syntheticChest = HasStaticChestPosition(markerId);
        if (syntheticWall || syntheticChest) {
            if (insideWindow) {
                if (marker->Image_30) SetCollapsed(marker->Image_30);
                SetCollapsed(marker);
            }
            continue;
        }
        if (!miniMapTreasureIds_.contains(markerId)) continue;
        if (insideWindow) {
            SetVisible(marker);
            if (marker->Image_30) SetVisible(marker->Image_30);
        }
    }
    // The borrowed cooked slot is only needed by the legacy hidden-chest path. Keep it alive only while that room
    // can actually intersect the minimap; native paint owns every other synthetic marker.
    if (const auto* hiddenLocation = FindChestLocationByNativeId(HIDDEN_CHEST_NATIVE_ID);
        hiddenLocation && IsLocationInsideMiniMapWindow(
                              *hiddenLocation, currentTraverseX, currentTraverseY)) {
        ReassertMiniMapCustomMarker(miniMap);
    } else if (auto* customImage = ResolveWidget(BORROWED_CUSTOM_MINIMAP_IMAGE)) {
        SetCollapsed(customImage);
    }
    return;
}

void InGameTracker::ReassertMiniMapCustomMarker(void* miniMapWidget) {
    if (!miniMapWidget) miniMapWidget = activeMiniMap_;
    if (!BORROWED_CUSTOM_MINIMAP_ACTIVE || !miniMapWidget || miniMapWidget != activeMiniMap_) return;
    auto* miniMap = static_cast<SDK::UMiniMapBlueprint_C*>(miniMapWidget);
    auto* customImage = static_cast<SDK::UImage*>(ResolveWidget(BORROWED_CUSTOM_MINIMAP_IMAGE));
    auto* customPanel = ResolveWidget(BORROWED_CUSTOM_MINIMAP_PANEL);
    const bool validPanel = customPanel &&
        (customPanel == miniMap->Custom_Marker_Panel ||
         (customPanel->Name.ToString().starts_with("AP_Marker_Panel_") &&
          customPanel->GetParent() == miniMap->Treasure_Panel));
    if (!customImage || !validPanel ||
        customImage->GetParent() != customPanel) {
        miniMapDirty_ = true;
        ApplyMiniMap(miniMap);
        return;
    }
    auto* customSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(customImage);
    if (!customSlot) return;
    if (customPanel != miniMap->Custom_Marker_Panel) {
        auto* apPanelSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(customPanel);
        auto* stockPanelSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(
            miniMap->Custom_Marker_Panel);
        if (apPanelSlot && stockPanelSlot) {
            apPanelSlot->SetLayout(stockPanelSlot->GetLayout());
            apPanelSlot->SetAutoSize(stockPanelSlot->GetAutoSize());
            apPanelSlot->SetZOrder(stockPanelSlot->GetZOrder());
        }
    }
    SetVisible(customPanel);
    customPanel->SetRenderOpacity(1.0f);
    customSlot->SetPosition(BORROWED_CUSTOM_MINIMAP_POSITION);
    customSlot->SetSize(BORROWED_CUSTOM_MINIMAP_SIZE);
    SetImageBrush(customImage, BORROWED_CUSTOM_MINIMAP_BRUSH);
    SetImageColor(customImage, {1.0f, 1.0f, 1.0f, 1.0f});
    customImage->SetRenderOpacity(1.0f);
    SetVisible(customImage);
}

void InGameTracker::ReactivateMiniMapForMenu() {
    if (!activeMiniMap_ || !activeMiniMapCanvas_) return;
    // The map menu clears the child widget's live Slate paint registration without replacing either UObject.
    // Request the proven ApplyMiniMap initialization path, but let it retain the calibrated coordinate relationship
    // when those two object identities are unchanged.
    miniMapPaintRefreshRequested_ = true;
    miniMapDirty_ = true;
}

void InGameTracker::PaintMiniMap(void* miniMapWidget, void* rawParams) {
    auto* paintWidget = static_cast<SDK::UUserWidget*>(miniMapWidget);
    auto* miniMap = static_cast<SDK::UMiniMapBlueprint_C*>(activeMiniMap_);
    auto* params = static_cast<SDK::Params::UserWidget_OnPaint*>(rawParams);
    auto* archipelago = Archipelago::ConnectedInstance();
    if (!IsLiveObject(miniMap, activeMiniMapIndex_) ||
        !IsLiveObject(static_cast<SDK::UObject*>(activeMiniMapCanvas_), activeMiniMapCanvasIndex_)) {
        return;
    }
    const bool calibrationPass = paintWidget == miniMap;
    const bool renderPass = paintWidget == activeMiniMapCanvas_ &&
                            paintWidget == miniMap->TotalMapBlueprint;
    if (!params || !paintWidget || !miniMap || (!calibrationPass && !renderPass) || !IsMiniMapEnabled() ||
        !archipelago || !inventorySynchronized_ || !miniMap->TotalMapBlueprint) {
        return;
    }
    // The parent widget is hooked only long enough to learn the native marker-panel transform. Once stable, avoid a
    // second complete map/brush/geometry pass every frame; the child TotalMap widget is the sole rendering surface.
    if (calibrationPass && miniMapCanvasCalibrationValid_) return;

    auto* totalMap = miniMap->TotalMapBlueprint;
    auto* mapManager = miniMap->GetMapManager();
    auto* playerController = GameManager::Instance().PlayerController();
    auto* hud = playerController ? static_cast<SDK::APBInterfaceHUD*>(playerController->MyHUD) : nullptr;
    auto* mapComponent = hud ? hud->m_MapComponent : nullptr;
    if (!mapManager || !mapComponent) return;
    SDK::int32 currentTraverseX = -1;
    SDK::int32 currentTraverseY = -1;
    mapManager->GetTraverseCurrent(&currentTraverseX, &currentTraverseY);

    auto geometry = GetMiniMapGeometry(totalMap);
    if (!geometry) return;
    // FPaintContext's first native member is a reference to the allotted geometry. Dumper-7 cannot describe
    // references and leaves the reflected structure opaque, but NativePaint supplies this live geometry for the
    // current frame. The widget's cached geometry can still contain the pre-render transform here.
    auto* allottedGeometry = *reinterpret_cast<const SDK::FGeometry* const*>(&params->Context);
    if (!allottedGeometry) return;
    const SDK::FGeometry miniMapGeometry = *allottedGeometry;
    const SDK::FVector2D miniMapSize = SDK::USlateBlueprintLibrary::GetLocalSize(miniMapGeometry);
    if (miniMapSize.X <= 0.0f || miniMapSize.Y <= 0.0f) return;
    auto resolveBrush = [&](void*& rawBrush, std::int32_t& objectIndex,
                            void*& rawTexture, std::int32_t& textureIndex, std::string_view assetPath,
                            std::string_view description) -> SDK::USlateBrushAsset* {
        if (IsLiveBrush(static_cast<SDK::USlateBrushAsset*>(rawBrush), objectIndex,
                        static_cast<SDK::UObject*>(rawTexture), textureIndex)) {
            return static_cast<SDK::USlateBrushAsset*>(rawBrush);
        }
        ReleaseRootedObject(rawBrush, objectIndex);
        ForgetObject(rawTexture, textureIndex);
        auto* texture = LoadCookedMarkerTexture(assetPath, description);
        if (!texture) return nullptr;
        auto* brush = static_cast<SDK::USlateBrushAsset*>(
            SDK::UGameplayStatics::SpawnObject(SDK::USlateBrushAsset::StaticClass(), miniMap));
        if (!brush) return nullptr;
        brush->Brush = SDK::UWidgetBlueprintLibrary::MakeBrushFromTexture(texture, 64, 64);
        // NativePaint receives only a raw brush handle, so retain that transient wrapper for the paint-surface
        // lifetime. The cooked texture is package-owned and remains referenced by the rooted brush.
        brush->Flags |= SDK::EObjectFlags::MarkAsRootSet;
        rawBrush = brush;
        objectIndex = brush->Index;
        rawTexture = texture;
        textureIndex = texture->Index;
        return brush;
    };

    auto* chestBrush = resolveBrush(miniMapChestBrush_, miniMapChestBrushIndex_,
                                    miniMapChestTexture_, miniMapChestTextureIndex_,
                                    CHEST_MARKER_TEXTURE, "native-paint chest marker");
    auto* wallBrush = resolveBrush(miniMapWallBrush_, miniMapWallBrushIndex_,
                                   miniMapWallTexture_, miniMapWallTextureIndex_,
                                   WALL_MARKER_TEXTURE, "native-paint wall marker");
    auto* shardBrush = resolveBrush(miniMapShardBrush_, miniMapShardBrushIndex_,
                                    miniMapShardTexture_, miniMapShardTextureIndex_,
                                    SHARD_MARKER_TEXTURE, "native-paint shard marker");
    SDK::USlateBrushAsset* ghostBrush = nullptr;
    if (IsLiveBrush(static_cast<SDK::USlateBrushAsset*>(MINI_MAP_GHOST_PAINT_BRUSH),
                    MINI_MAP_GHOST_PAINT_BRUSH_INDEX,
                    static_cast<SDK::UObject*>(MINI_MAP_GHOST_TEXTURE),
                    MINI_MAP_GHOST_TEXTURE_INDEX)) {
        ghostBrush = static_cast<SDK::USlateBrushAsset*>(MINI_MAP_GHOST_PAINT_BRUSH);
    }
    if (!chestBrush && !wallBrush && !shardBrush && !ghostBrush) return;

    auto drawGhostAtlas = [&](const SDK::FVector2D& position, const SDK::FVector2D& size) {
        // The native minimap is translucent, so it cannot act as an opacity mask the way the total map does. The
        // retained atlas itself now contains only reachable, unexplored cells and may paint normally in this layer.
        SDK::UWidgetBlueprintLibrary::DrawBox(
            params->Context, position, size, ghostBrush,
            {1.0f, 1.0f, 1.0f, 1.0f});
    };

    const SDK::EDivideMap areaMapType = mapComponent->CheckMapType(mapManager->GetCurrentAreaId());
    const SDK::EDivideMap renderMapType = areaMapType;
    const NativeMapAxes axes =
        FindNativeRenderAxes(mapManager, mapComponent, renderMapType, totalMap, areaMapType);

    auto findStoredTreasurePosition = [&](std::string_view normalizedId) -> std::optional<SDK::FVector2D> {
        if (!hud->m_MapTreasureIconComponent) return std::nullopt;
        for (const auto& entry : hud->m_MapTreasureIconComponent->TreasureMarkerMapLocation) {
            if (NormalizeTreasureId(entry.Key().ToString()) == normalizedId) return entry.Value();
        }
        return std::nullopt;
    };

    if (renderPass && ghostBrush) {
        // Follow RenderTargetMini_Image itself so the atlas inherits the native map's continuous player scrolling
        // and room-transition jumps exactly. Apply calibration scale around the already-moving atlas center; scaling
        // absolute map coordinates made room size and scroll speed inseparable.
        const std::array<SDK::FVector2D, 4> renderCorners{{
            MINI_MAP_GHOST_RENDER_MINIMUM,
            {MINI_MAP_GHOST_RENDER_MAXIMUM.X, MINI_MAP_GHOST_RENDER_MINIMUM.Y},
            {MINI_MAP_GHOST_RENDER_MINIMUM.X, MINI_MAP_GHOST_RENDER_MAXIMUM.Y},
            MINI_MAP_GHOST_RENDER_MAXIMUM,
        }};
        const SDK::FVector2D canvasSize =
            SDK::USlateBlueprintLibrary::GetLocalSize(geometry->canvas);
        const SDK::FVector2D renderTargetSize =
            SDK::USlateBlueprintLibrary::GetLocalSize(geometry->renderTarget);
        if (canvasSize.X > 0.0f && canvasSize.Y > 0.0f &&
            renderTargetSize.X > 0.0f && renderTargetSize.Y > 0.0f) {
            // RenderTargetMini_Image moves smoothly with m_MiniLeftTop, but the game also rewrites its 2048x2048
            // texture window at room boundaries. Its cached geometry therefore includes both the physical scroll
            // and a discrete m_MiniWrite recenter. The latter is texture bookkeeping, not map movement: counting it
            // shifts the custom atlas by exactly one or more rooms on every transition. Remove that term in absolute
            // render-target coordinates. Unlike retaining a first-frame baseline, this remains deterministic when a
            // save is loaded in another room or the minimap widget is reconstructed after a menu.
            const SDK::FVector2D renderTargetCenter = renderTargetSize * 0.5f;
            const SDK::FVector2D renderWindowCorrection{
                renderTargetCenter.X - mapComponent->m_MiniWrite.X,
                mapComponent->m_MiniWrite.Y - renderTargetCenter.Y,
            };
            const SDK::FVector2D canvasWindowCorrection =
                MapRenderToCanvas(*geometry, renderWindowCorrection) -
                MapRenderToCanvas(*geometry, {});
            auto correctedCanvasPosition = [&](const SDK::FVector2D& renderPosition) {
                return MapRenderToCanvas(*geometry, renderPosition) + canvasWindowCorrection;
            };

            SDK::FVector2D canvasMinimum = correctedCanvasPosition(renderCorners.front());
            SDK::FVector2D canvasMaximum = canvasMinimum;
            const SDK::FVector2D firstAbsolute = SDK::USlateBlueprintLibrary::LocalToAbsolute(
                geometry->canvas, canvasMinimum);
            SDK::FVector2D paintMinimum =
                SDK::USlateBlueprintLibrary::AbsoluteToLocal(miniMapGeometry, firstAbsolute);
            SDK::FVector2D paintMaximum = paintMinimum;
            for (const auto& renderCorner : renderCorners) {
                const SDK::FVector2D canvasCorner = correctedCanvasPosition(renderCorner);
                canvasMinimum.X = std::min(canvasMinimum.X, canvasCorner.X);
                canvasMinimum.Y = std::min(canvasMinimum.Y, canvasCorner.Y);
                canvasMaximum.X = std::max(canvasMaximum.X, canvasCorner.X);
                canvasMaximum.Y = std::max(canvasMaximum.Y, canvasCorner.Y);
                const SDK::FVector2D absoluteCorner =
                    SDK::USlateBlueprintLibrary::LocalToAbsolute(geometry->canvas, canvasCorner);
                const SDK::FVector2D paintCorner =
                    SDK::USlateBlueprintLibrary::AbsoluteToLocal(miniMapGeometry, absoluteCorner);
                paintMinimum.X = std::min(paintMinimum.X, paintCorner.X);
                paintMinimum.Y = std::min(paintMinimum.Y, paintCorner.Y);
                paintMaximum.X = std::max(paintMaximum.X, paintCorner.X);
                paintMaximum.Y = std::max(paintMaximum.Y, paintCorner.Y);
            }
            const SDK::FVector2D paintCenter = (paintMinimum + paintMaximum) * 0.5f;
            const SDK::FVector2D nativeHalfSize = (paintMaximum - paintMinimum) * 0.5f;
            // The native minimap samples a 21x12 source grid at 2x magnification, while the atlas coordinate grid
            // is 26.25x15. Both axes therefore require exactly (2 * source / atlas) = 1.6. The final two-pixel
            // residual aligns the centers of the native and imported brushes.
            constexpr float NATIVE_ATLAS_SCALE_X = 1.6f;
            constexpr float NATIVE_ATLAS_SCALE_Y = 1.6f;
            constexpr SDK::FVector2D NATIVE_ATLAS_RESIDUAL_OFFSET{2.0f, 2.0f};
            const float effectiveScaleX = NATIVE_ATLAS_SCALE_X;
            const float effectiveScaleY = NATIVE_ATLAS_SCALE_Y;
            const SDK::FVector2D calibratedHalfSize{
                nativeHalfSize.X * effectiveScaleX,
                nativeHalfSize.Y * effectiveScaleY,
            };
            SDK::FVector2D automaticOffset{};
            MINI_MAP_GHOST_LAST_AUTO_OFFSET_VALID = false;
            if (totalMap->PlayerPos_Image) {
                const SDK::FGeometry playerGeometry =
                    totalMap->PlayerPos_Image->GetCachedGeometry();
                const SDK::FVector2D playerSize =
                    SDK::USlateBlueprintLibrary::GetLocalSize(playerGeometry);
                if (playerSize.X > 0.0f && playerSize.Y > 0.0f) {
                    const SDK::FVector2D playerRenderPosition =
                        mapComponent->GetPlayerInMapPosition(
                            renderMapType, totalMap->IconPixelOffset, totalMap->canvasSize);
                    SDK::FVector2D playerAtlasRenderPosition = playerRenderPosition;
                    // The atlas rasterizer normalizes GetRoomCenterInMapPosition's 28-unit assignment-row stride
                    // to the visible 15-unit room pitch. Apply the same coordinate transform to the live player
                    // anchor; using its raw Y made the automatic offset vary at the discarded 28-unit rate and
                    // caused the otherwise aligned atlas to over-scroll vertically.
                    const float playerVerticalCenterScale =
                        std::abs(axes.z.Y) > 0.01f
                            ? totalMap->RoomPixelSize.Y / std::abs(axes.z.Y)
                            : 1.0f;
                    playerAtlasRenderPosition.Y *= playerVerticalCenterScale;
                    const SDK::FVector2D playerCanvasPosition =
                        correctedCanvasPosition(playerAtlasRenderPosition);
                    const SDK::FVector2D playerCanvasAbsolute =
                        SDK::USlateBlueprintLibrary::LocalToAbsolute(
                            geometry->canvas, playerCanvasPosition);
                    const SDK::FVector2D playerAtlasPaint =
                        SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                            miniMapGeometry, playerCanvasAbsolute);
                    const SDK::FVector2D scaledPlayerAtlasPaint{
                        paintCenter.X + (playerAtlasPaint.X - paintCenter.X) *
                                            effectiveScaleX,
                        paintCenter.Y + (playerAtlasPaint.Y - paintCenter.Y) *
                                            effectiveScaleY,
                    };
                    const SDK::FVector2D playerNativeAbsolute =
                        SDK::USlateBlueprintLibrary::LocalToAbsolute(
                            playerGeometry, playerSize * 0.5f);
                    const SDK::FVector2D playerNativePaint =
                        SDK::USlateBlueprintLibrary::AbsoluteToLocal(
                            miniMapGeometry, playerNativeAbsolute);
                    automaticOffset = playerNativePaint - scaledPlayerAtlasPaint;
                    MINI_MAP_GHOST_LAST_AUTO_OFFSET = automaticOffset;
                    MINI_MAP_GHOST_LAST_PLAYER_RENDER = playerRenderPosition;
                    MINI_MAP_GHOST_LAST_PLAYER_NATIVE_PAINT = playerNativePaint;
                    MINI_MAP_GHOST_LAST_PLAYER_ATLAS_PAINT = scaledPlayerAtlasPaint;
                    MINI_MAP_GHOST_LAST_AUTO_OFFSET_VALID = true;
                }
            }
            const SDK::FVector2D calibratedCenter =
                paintCenter + automaticOffset + NATIVE_ATLAS_RESIDUAL_OFFSET;
            paintMinimum = calibratedCenter - calibratedHalfSize;
            paintMaximum = calibratedCenter + calibratedHalfSize;
            MINI_MAP_GHOST_LAST_CANVAS_MINIMUM = canvasMinimum;
            MINI_MAP_GHOST_LAST_CANVAS_MAXIMUM = canvasMaximum;
            MINI_MAP_GHOST_LAST_PAINT_MINIMUM = paintMinimum;
            MINI_MAP_GHOST_LAST_PAINT_MAXIMUM = paintMaximum;
            const SDK::FVector2D paintSize = calibratedHalfSize * 2.0f;
            if (paintSize.X > 0.0f && paintSize.Y > 0.0f) {
                drawGhostAtlas(paintMinimum, paintSize);
            }

        }
    }

    // Treasure_Panel is the permanent parent of every stock minimap treasure marker. The Blueprint moves this panel
    // by its live m_MiniLeftTop delta every Tick. Record one native marker only to learn the static coordinate
    // relationship, then retain that measurement and follow the panel itself; the marker can subsequently disappear.
    if (calibrationPass) {
        if (!miniMap->Treasure_Panel) return;
        const SDK::FGeometry panelGeometry = miniMap->Treasure_Panel->GetCachedGeometry();
        const SDK::FVector2D panelSize = SDK::USlateBlueprintLibrary::GetLocalSize(panelGeometry);
        if (panelSize.X <= 0.0f || panelSize.Y <= 0.0f) return;
        if (!miniMapCanvasCalibrationValid_) {
            float nearestDistanceSquared = std::numeric_limits<float>::max();
            std::string nearestMarkerId;
            const auto hiddenPosition = findStoredTreasurePosition(HIDDEN_CHEST_NATIVE_ID);
            for (auto* marker : miniMap->TreasureIconList) {
                if (!marker || !IsWidgetPaintVisible(marker)) continue;
                const std::string markerId = NormalizeTreasureId(marker->treasureID.ToString());
                if (markerId == HIDDEN_CHEST_NATIVE_ID) continue;
                const auto markerMapPosition = findStoredTreasurePosition(markerId);
                if (!markerMapPosition) continue;
                auto* anchorWidget = marker->Image_30 ? static_cast<SDK::UWidget*>(marker->Image_30)
                                                      : static_cast<SDK::UWidget*>(marker);
                const SDK::FGeometry anchorGeometry = anchorWidget->GetCachedGeometry();
                const SDK::FVector2D anchorSize = SDK::USlateBlueprintLibrary::GetLocalSize(anchorGeometry);
                if (anchorSize.X <= 0.0f || anchorSize.Y <= 0.0f) continue;
                float distanceSquared = 0.0f;
                if (hiddenPosition) {
                    const SDK::FVector2D delta = *markerMapPosition - *hiddenPosition;
                    distanceSquared = delta.X * delta.X + delta.Y * delta.Y;
                }
                if (distanceSquared >= nearestDistanceSquared) continue;
                const SDK::FVector2D anchorAbsolute =
                    SDK::USlateBlueprintLibrary::LocalToAbsolute(anchorGeometry, anchorSize * 0.5f);
                const SDK::FVector2D anchorCanvas =
                    SDK::USlateBlueprintLibrary::AbsoluteToLocal(geometry->canvas, anchorAbsolute);
                const SDK::FVector2D anchorPanel =
                    SDK::USlateBlueprintLibrary::AbsoluteToLocal(panelGeometry, anchorAbsolute);
                miniMapCanvasCalibrationX_ = anchorCanvas.X;
                miniMapCanvasCalibrationY_ = anchorCanvas.Y;
                miniMapCalibrationMapX_ = markerMapPosition->X;
                miniMapCalibrationMapY_ = markerMapPosition->Y;
                miniMapCalibrationAnchorPanelX_ = anchorPanel.X;
                miniMapCalibrationAnchorPanelY_ = anchorPanel.Y;
                nearestDistanceSquared = distanceSquared;
                nearestMarkerId = markerId;
            }
            if (!nearestMarkerId.empty()) {
                const float candidatePanelX = miniMapCalibrationAnchorPanelX_;
                const float candidatePanelY = miniMapCalibrationAnchorPanelY_;
                constexpr float STABLE_OFFSET_TOLERANCE = 0.25f;
                constexpr std::uint8_t REQUIRED_STABLE_PAINT_FRAMES = 4;
                const bool stableWithPrevious =
                    miniMapCalibrationStableFrames_ > 0 &&
                    std::abs(candidatePanelX - miniMapCalibrationCandidatePanelX_) <= STABLE_OFFSET_TOLERANCE &&
                    std::abs(candidatePanelY - miniMapCalibrationCandidatePanelY_) <= STABLE_OFFSET_TOLERANCE;
                miniMapCalibrationStableFrames_ = stableWithPrevious
                                                       ? static_cast<std::uint8_t>(miniMapCalibrationStableFrames_ + 1)
                                                       : 1;
                miniMapCalibrationCandidatePanelX_ = candidatePanelX;
                miniMapCalibrationCandidatePanelY_ = candidatePanelY;
                miniMapCanvasCalibrationValid_ =
                    miniMapCalibrationStableFrames_ >= REQUIRED_STABLE_PAINT_FRAMES;
            }
            if (miniMapCanvasCalibrationValid_) {
                LOG_MAP_DIAGNOSTIC(LogLevel::File, "[Tracker] Recorded panel-relative minimap calibration; marker canvas:",
                            miniMapCanvasCalibrationX_, miniMapCanvasCalibrationY_, "map:",
                            miniMapCalibrationMapX_, miniMapCalibrationMapY_, "anchor panel local:",
                            miniMapCalibrationAnchorPanelX_, miniMapCalibrationAnchorPanelY_, "after",
                            static_cast<int>(miniMapCalibrationStableFrames_),
                            "stable paint frames; from:", nearestMarkerId);
            }
        }
        return;
    }

    std::unordered_set<std::string> nativelyVisibleIds;
    for (auto* marker : miniMap->TreasureIconList) {
        if (!marker || !IsWidgetPaintVisible(marker)) continue;
        const SDK::FVector2D size =
            SDK::USlateBlueprintLibrary::GetLocalSize(marker->GetCachedGeometry());
        if (size.X <= 0.0f || size.Y <= 0.0f) continue;
        nativelyVisibleIds.insert(NormalizeTreasureId(marker->treasureID.ToString()));
    }

    const SDK::FGeometry livePanelGeometry = miniMap->Treasure_Panel->GetCachedGeometry();
    const SDK::FVector2D livePanelSize =
        SDK::USlateBlueprintLibrary::GetLocalSize(livePanelGeometry);
    auto mapToPanel = [](const SDK::FVector2D& mapPosition) {
        return SDK::FVector2D{
            static_cast<float>(MINI_MAP_PANEL_MAP_X_TO_X * mapPosition.X +
                               MINI_MAP_PANEL_MAP_Y_TO_X * mapPosition.Y +
                               MINI_MAP_PANEL_OFFSET_X),
            static_cast<float>(MINI_MAP_PANEL_MAP_X_TO_Y * mapPosition.X +
                               MINI_MAP_PANEL_MAP_Y_TO_Y * mapPosition.Y +
                               MINI_MAP_PANEL_OFFSET_Y),
        };
    };
    auto renderToPanel = [](const SDK::FVector2D& renderPosition) {
        return SDK::FVector2D{
            static_cast<float>(MINI_MAP_RENDER_TO_PANEL_X_SCALE * renderPosition.X +
                               MINI_MAP_RENDER_TO_PANEL_OFFSET_X),
            static_cast<float>(MINI_MAP_RENDER_TO_PANEL_Y_SCALE * renderPosition.Y +
                               MINI_MAP_RENDER_TO_PANEL_OFFSET_Y),
        };
    };
    auto panelToPaint = [&](const SDK::FVector2D& panelPosition) {
        const SDK::FVector2D absolute =
            SDK::USlateBlueprintLibrary::LocalToAbsolute(livePanelGeometry, panelPosition);
        return SDK::USlateBlueprintLibrary::AbsoluteToLocal(miniMapGeometry, absolute);
    };

    // The reachability atlas is already calibrated against the native minimap. Place synthetic markers in that
    // same texture coordinate system instead of independently fitting Treasure_Panel. This makes room-local marker
    // positions inherit the atlas's player anchor, room-transition correction, clipping and menu reconstruction.
    auto roomPositionToAtlasPaint = [&](std::string_view roomName, float requestedMapX,
                                        float requestedMapZ, float markerScale)
        -> std::optional<std::pair<SDK::FVector2D, SDK::FVector2D>> {
        if (MINI_MAP_ATLAS_MAP_TYPE != static_cast<SDK::int32>(areaMapType) ||
            MINI_MAP_GHOST_TEXTURE_WIDTH <= 0 || MINI_MAP_GHOST_TEXTURE_HEIGHT <= 0) {
            return std::nullopt;
        }
        const SDK::FVector2D atlasPaintSize =
            MINI_MAP_GHOST_LAST_PAINT_MAXIMUM - MINI_MAP_GHOST_LAST_PAINT_MINIMUM;
        if (atlasPaintSize.X <= 0.0f || atlasPaintSize.Y <= 0.0f) return std::nullopt;

        const auto* room = Tracker::FindRoom(roomName);
        if (!room || room->out_of_map || room->width == 0 || room->height == 0) return std::nullopt;
        const float mapX = std::clamp(requestedMapX, 0.0f, static_cast<float>(room->width));
        const float mapZ = std::clamp(requestedMapZ, 0.0f, static_cast<float>(room->height));
        const std::uint32_t cellX =
            std::min(static_cast<std::uint32_t>(mapX), room->width - 1u);
        const std::uint32_t cellZ =
            std::min(static_cast<std::uint32_t>(mapZ), room->height - 1u);
        const std::uint32_t assignment = cellZ * room->width + cellX + 1u;
        auto cell = std::ranges::find_if(MINI_MAP_ATLAS_CELLS, [&](const MiniMapAtlasCell& candidate) {
            return candidate.room == roomName && candidate.assignment == assignment;
        });
        // A geometric room center can land on a deliberately hidden/nonexistent cell. Any visible cell in the same
        // room still supplies the atlas pitch and origin, so use it as the coordinate anchor.
        if (cell == MINI_MAP_ATLAS_CELLS.end()) {
            cell = std::ranges::find_if(MINI_MAP_ATLAS_CELLS, [&](const MiniMapAtlasCell& candidate) {
                return candidate.room == roomName;
            });
        }
        if (cell == MINI_MAP_ATLAS_CELLS.end()) return std::nullopt;

        // Cell rectangles deliberately leave a one-pixel separator. Add that pixel back to recover the atlas pitch,
        // then position within the logical room cell using the exported actor coordinates.
        const float pitchX = static_cast<float>(cell->right - cell->left + 1);
        const float pitchY = static_cast<float>(cell->bottom - cell->top + 1);
        const std::uint32_t anchorCell = cell->assignment - 1u;
        const float anchorCellX = static_cast<float>(anchorCell % room->width);
        const float anchorCellZ = static_cast<float>(anchorCell / room->width);
        const float targetCellLocalZ = mapZ - static_cast<float>(cellZ);
        const SDK::FVector2D atlasPixel{
            static_cast<float>(cell->left) + pitchX * (mapX - anchorCellX),
            static_cast<float>(cell->top) +
                // Atlas Y increases downward while room-local Z increases upward. If the target cell itself is
                // hidden and `cell` is a visible fallback from another row, move opposite the Z-row delta. The old
                // sign doubled that row error, placing a one-row fallback two minimap rooms away.
                pitchY * (anchorCellZ - static_cast<float>(cellZ) + 1.0f - targetCellLocalZ),
        };
        const SDK::FVector2D paintCenter{
            MINI_MAP_GHOST_LAST_PAINT_MINIMUM.X +
                atlasPaintSize.X * atlasPixel.X / static_cast<float>(MINI_MAP_GHOST_TEXTURE_WIDTH),
            MINI_MAP_GHOST_LAST_PAINT_MINIMUM.Y +
                atlasPaintSize.Y * atlasPixel.Y / static_cast<float>(MINI_MAP_GHOST_TEXTURE_HEIGHT),
        };
        const SDK::FVector2D paintedCellPitch{
            atlasPaintSize.X * pitchX / static_cast<float>(MINI_MAP_GHOST_TEXTURE_WIDTH),
            atlasPaintSize.Y * pitchY / static_cast<float>(MINI_MAP_GHOST_TEXTURE_HEIGHT),
        };
        const float markerExtent = std::clamp(
            std::min(std::abs(paintedCellPitch.X), std::abs(paintedCellPitch.Y)) * markerScale,
            12.0f, 64.0f);
        return std::pair{paintCenter, SDK::FVector2D{markerExtent, markerExtent}};
    };
    auto locationToAtlasPaint = [&](const bloodstained::tracker::generated::LocationData& location,
                                    float markerScale) {
        const auto* room = Tracker::FindRoom(location.room);
        if (!room) return std::optional<std::pair<SDK::FVector2D, SDK::FVector2D>>{};
        return roomPositionToAtlasPaint(location.room, location.map_x,
                                        GetLocationMarkerMapZ(location, room->height), markerScale);
    };
    auto roomCenterToAtlasPaint = [&](std::string_view roomName, float markerScale)
        -> std::optional<std::pair<SDK::FVector2D, SDK::FVector2D>> {
        if (MINI_MAP_ATLAS_MAP_TYPE != static_cast<SDK::int32>(areaMapType) ||
            MINI_MAP_GHOST_TEXTURE_WIDTH <= 0 || MINI_MAP_GHOST_TEXTURE_HEIGHT <= 0) {
            return std::nullopt;
        }
        const SDK::FVector2D atlasPaintSize =
            MINI_MAP_GHOST_LAST_PAINT_MAXIMUM - MINI_MAP_GHOST_LAST_PAINT_MINIMUM;
        if (atlasPaintSize.X <= 0.0f || atlasPaintSize.Y <= 0.0f) return std::nullopt;

        auto first = std::ranges::find_if(MINI_MAP_ATLAS_CELLS, [&](const MiniMapAtlasCell& cell) {
            return cell.room == roomName;
        });
        if (first == MINI_MAP_ATLAS_CELLS.end()) return std::nullopt;
        float left = static_cast<float>(first->left);
        float top = static_cast<float>(first->top);
        float right = static_cast<float>(first->right + 1);
        float bottom = static_cast<float>(first->bottom + 1);
        for (const auto& cell : MINI_MAP_ATLAS_CELLS) {
            if (cell.room != roomName) continue;
            left = std::min(left, static_cast<float>(cell.left));
            top = std::min(top, static_cast<float>(cell.top));
            right = std::max(right, static_cast<float>(cell.right + 1));
            bottom = std::max(bottom, static_cast<float>(cell.bottom + 1));
        }
        const SDK::FVector2D atlasPixel{(left + right) * 0.5f, (top + bottom) * 0.5f};
        const SDK::FVector2D paintCenter{
            MINI_MAP_GHOST_LAST_PAINT_MINIMUM.X +
                atlasPaintSize.X * atlasPixel.X / static_cast<float>(MINI_MAP_GHOST_TEXTURE_WIDTH),
            MINI_MAP_GHOST_LAST_PAINT_MINIMUM.Y +
                atlasPaintSize.Y * atlasPixel.Y / static_cast<float>(MINI_MAP_GHOST_TEXTURE_HEIGHT),
        };
        const float pitchX = static_cast<float>(first->right - first->left + 1);
        const float pitchY = static_cast<float>(first->bottom - first->top + 1);
        const float markerExtent = std::clamp(
            std::min(atlasPaintSize.X * pitchX / static_cast<float>(MINI_MAP_GHOST_TEXTURE_WIDTH),
                     atlasPaintSize.Y * pitchY / static_cast<float>(MINI_MAP_GHOST_TEXTURE_HEIGHT)) *
                markerScale,
            12.0f, 64.0f);
        return std::pair{paintCenter, SDK::FVector2D{markerExtent, markerExtent}};
    };

    std::unordered_set<std::string> paintedPhysicalIds;
    for (const auto& location : bloodstained::tracker::generated::LOCATIONS) {
        // Reject off-screen locations before native-name normalization, set lookup, room/area reflection calls, atlas
        // searches, or widget painting. At maximum zoom this leaves at most an 11x11 conservative neighborhood.
        if (!IsLocationInsideMiniMapWindow(location, currentTraverseX, currentTraverseY)) continue;
        SDK::USlateBrushAsset* brush = nullptr;
        std::string normalizedId;
        if (location.type == LocationType::CHEST) {
            const auto nativeName = Tracker::FindNativeLocationName(location.id);
            if (!nativeName) continue;
            normalizedId = NormalizeTreasureId(std::string(*nativeName));
            if (!miniMapTreasureIds_.contains(normalizedId)) continue;
            if (!paintedPhysicalIds.insert(normalizedId).second) continue;
            brush = chestBrush;
        } else if (location.type == LocationType::WALL) {
            if (!miniMapWallLocationIds_.contains(location.id)) continue;
            normalizedId = GetNativeWallMarkerId(location.id);
            if (nativelyVisibleIds.contains(normalizedId)) continue;
            brush = wallBrush;
        } else {
            continue;
        }
        if (!brush || !location.has_map_position) continue;
        const auto* room = Tracker::FindRoom(location.room);
        if (!room || room->out_of_map ||
            mapComponent->CheckMapType(mapManager->RoomIdToAreaId(NameFromString(location.room))) != areaMapType) {
            continue;
        }

        if (location.type == LocationType::WALL || location.type == LocationType::CHEST) {
            const auto atlasPaint = locationToAtlasPaint(
                location, location.type == LocationType::CHEST ? 1.50f : 0.60f);
            if (!atlasPaint) continue;
            const auto& [paintCenter, paintSize] = *atlasPaint;
            // The actor point lies at floor height, while the native chest glyph is centered roughly one quarter of
            // its height above it. A full bottom-center anchor overcompensates and puts these pickups at the ceiling.
            // Breakable-wall markers remain centered on their actor point.
            const SDK::FVector2D paintMinimum =
                location.type == LocationType::CHEST
                    ? SDK::FVector2D{paintCenter.X - paintSize.X * 0.5f,
                                     paintCenter.Y - paintSize.Y * 0.75f}
                    : paintCenter - paintSize * 0.5f;
            SDK::UWidgetBlueprintLibrary::DrawBox(params->Context, paintMinimum,
                                                  paintSize, brush, {1.0f, 1.0f, 1.0f, 1.0f});
            continue;
        }

        std::optional<SDK::FVector2D> nativePosition;
        bool positionIsTreasureMap = false;
        if (location.type == LocationType::CHEST && location.prefer_native_marker) {
            nativePosition = findStoredTreasurePosition(normalizedId);
            positionIsTreasureMap = nativePosition.has_value();
        }
        if (!nativePosition) {
            nativePosition = GetStaticLocationMapPosition(mapComponent, renderMapType, totalMap, axes, location);
        }
        if (!nativePosition || (positionIsTreasureMap && !MINI_MAP_PANEL_TRANSFORM_VALID) ||
            (!positionIsTreasureMap && !MINI_MAP_RENDER_TO_PANEL_VALID) ||
            livePanelSize.X <= 0.0f || livePanelSize.Y <= 0.0f) {
            continue;
        }
        SDK::FVector2D adjustedNativePosition = *nativePosition;
        if (normalizedId == HIDDEN_CHEST_NATIVE_ID) {
            // Preserve the empirically verified physical-floor adjustment in map units. Unlike the old canvas-space
            // correction, this remains attached to the native panel transform through room transitions.
            if (positionIsTreasureMap) {
                if (std::abs(MINI_MAP_PANEL_MAP_X_TO_X) > 0.001) {
                    adjustedNativePosition.X -= static_cast<float>(30.0 / MINI_MAP_PANEL_MAP_X_TO_X);
                }
                if (std::abs(MINI_MAP_PANEL_MAP_Y_TO_Y) > 0.001) {
                    adjustedNativePosition.Y -= static_cast<float>(30.0 / MINI_MAP_PANEL_MAP_Y_TO_Y);
                }
            } else {
                const SDK::FVector2D renderDelta = axes.z * 0.9f;
                adjustedNativePosition -= renderDelta;
            }
        }
        const SDK::FVector2D panelCenter = positionIsTreasureMap
                                              ? mapToPanel(adjustedNativePosition)
                                              : renderToPanel(adjustedNativePosition);
        const SDK::FVector2D paintCenter = panelToPaint(panelCenter);
        const SDK::FVector2D PAINT_SIZE =
            location.type == LocationType::CHEST ? SDK::FVector2D{60.0f, 60.0f}
                                                 : SDK::FVector2D{64.0f, 64.0f};
        SDK::UWidgetBlueprintLibrary::DrawBox(params->Context, paintCenter - PAINT_SIZE * 0.5f,
                                              PAINT_SIZE, brush, {1.0f, 1.0f, 1.0f, 1.0f});
    }

    if (shardBrush) {
        for (const std::string& roomName : miniMapShardRooms_) {
            const auto* room = Tracker::FindRoom(roomName);
            if (!IsRoomInsideMiniMapWindow(room, currentTraverseX, currentTraverseY) ||
                mapComponent->CheckMapType(mapManager->RoomIdToAreaId(NameFromString(roomName))) != areaMapType) {
                continue;
            }
            const auto atlasPaint = roomCenterToAtlasPaint(roomName, 0.60f);
            if (!atlasPaint) continue;
            const auto& [paintCenter, paintSize] = *atlasPaint;
            SDK::UWidgetBlueprintLibrary::DrawBox(params->Context, paintCenter - paintSize * 0.5f,
                                                  paintSize, shardBrush,
                                                  {1.0f, 1.0f, 1.0f, 1.0f});
        }
    }
}
