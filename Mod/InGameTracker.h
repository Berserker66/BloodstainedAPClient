#pragma once

#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

enum class TrackerDisplayMode : std::int32_t {
    NONE = 0,
    MAIN_MAP = 1,
    MINI_MAP = 2,
    FULL = 3,
};

struct TrackedWidgetHandle {
    void* pointer = nullptr;
    std::int32_t objectIndex = -1;
};

struct ReachableRoomCell {
    std::string room;
    std::uint32_t assignment = 0;
};

class InGameTracker {
   public:
    static InGameTracker& Instance();

    void ApplyDeferredGhostMap(void* mapWidget);
    void ApplyMapMarkers(void* mapWidget);
    void ApplyMiniMap(void* miniMapWidget);
    void ReassertMiniMapCustomMarker(void* miniMapWidget);
    void ReactivateMiniMapForMenu();
    void PaintMiniMap(void* miniMapWidget, void* paintParams);
    TrackerDisplayMode GetDisplayMode() const { return displayMode_.load(); }
    void InvalidateReachability(std::string_view reason);
    void LoadDisplayMode();
    void MarkInventorySynchronized();
    void ObserveNativeItem(std::string_view itemName);
    void ObserveLocationCleared(std::string_view locationName);
    void ResetConnection();
    void SetDisplayMode(TrackerDisplayMode mode);

   private:
    InGameTracker() = default;

    void ClearMainMapMarkers();
    void ClearMiniMapMarkers(bool clearGhosts = true);
    bool IsMainMapEnabled() const;
    bool IsMiniMapEnabled() const;

    bool inventorySynchronized_ = false;
    bool reachabilityDirty_ = true;
    bool miniMapDirty_ = true;
    bool miniMapGhostsDirty_ = true;
    void* pendingGhostMap_ = nullptr;
    void* activeMainMap_ = nullptr;
    void* activeMiniMap_ = nullptr;
    std::int32_t activeMiniMapIndex_ = -1;
    void* activeMiniMapCanvas_ = nullptr;
    std::int32_t activeMiniMapCanvasIndex_ = -1;
    std::int32_t activeMiniMapType_ = -1;
    bool mainMapDirty_ = true;
    std::atomic<TrackerDisplayMode> displayMode_{TrackerDisplayMode::FULL};
    std::vector<TrackedWidgetHandle> spawnedMainMapGhostWidgets_;
    std::vector<TrackedWidgetHandle> activatedNativeMarkerWidgets_;
    std::vector<TrackedWidgetHandle> spawnedWallMarkerWidgets_;
    std::vector<TrackedWidgetHandle> activatedMiniMapMarkerWidgets_;
    std::vector<TrackedWidgetHandle> miniMapVisibilityWidgets_;
    std::vector<TrackedWidgetHandle> spawnedMiniMapWidgets_;
    std::vector<TrackedWidgetHandle> spawnedMiniMapGhostWidgets_;
    std::unordered_set<std::uint64_t> pendingWallLocationIds_;
    std::unordered_set<std::string> pendingSyntheticTreasureIds_;
    std::unordered_set<std::string> pendingShardRooms_;
    std::unordered_set<std::uint64_t> miniMapWallLocationIds_;
    std::unordered_set<std::string> miniMapShardRooms_;
    std::unordered_set<std::string> miniMapTreasureIds_;
    std::unordered_set<std::string> reachableRooms_;
    std::vector<ReachableRoomCell> reachableCells_;
    void* miniMapChestBrush_ = nullptr;
    std::int32_t miniMapChestBrushIndex_ = -1;
    void* miniMapChestTexture_ = nullptr;
    std::int32_t miniMapChestTextureIndex_ = -1;
    void* miniMapWallBrush_ = nullptr;
    std::int32_t miniMapWallBrushIndex_ = -1;
    void* miniMapWallTexture_ = nullptr;
    std::int32_t miniMapWallTextureIndex_ = -1;
    void* miniMapShardBrush_ = nullptr;
    std::int32_t miniMapShardBrushIndex_ = -1;
    void* miniMapShardTexture_ = nullptr;
    std::int32_t miniMapShardTextureIndex_ = -1;
    bool miniMapCanvasCalibrationValid_ = false;
    float miniMapCanvasCalibrationX_ = 0.0f;
    float miniMapCanvasCalibrationY_ = 0.0f;
    float miniMapCalibrationMapX_ = 0.0f;
    float miniMapCalibrationMapY_ = 0.0f;
    float miniMapCalibrationAnchorPanelX_ = 0.0f;
    float miniMapCalibrationAnchorPanelY_ = 0.0f;
    float miniMapCalibrationCandidatePanelX_ = 0.0f;
    float miniMapCalibrationCandidatePanelY_ = 0.0f;
    std::uint8_t miniMapCalibrationStableFrames_ = 0;
    bool miniMapPaintRefreshRequested_ = false;
};
