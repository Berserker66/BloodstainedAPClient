#pragma once

#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
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

enum class MiniMapOverlayMarkerKind : std::uint8_t {
    CHEST,
    WALL,
    SHARD,
};

struct MiniMapOverlayMarker {
    MiniMapOverlayMarkerKind kind = MiniMapOverlayMarkerKind::WALL;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct MiniMapOverlaySnapshot {
    bool visible = false;
    float clipLeft = 0.0f;
    float clipTop = 0.0f;
    float clipRight = 0.0f;
    float clipBottom = 0.0f;
    std::uint64_t updatedAtMilliseconds = 0;
    std::vector<MiniMapOverlayMarker> markers;
};

class InGameTracker {
   public:
    static InGameTracker& Instance();

    void ApplyDeferredGhostMap(void* mapWidget);
    void ApplyMapMarkers(void* mapWidget);
    void ApplyMiniMap(void* miniMapWidget);
    MiniMapOverlaySnapshot GetMiniMapOverlaySnapshot() const;
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
    void ClearMiniMapMarkers();
    bool IsMainMapEnabled() const;
    bool IsMiniMapEnabled() const;

    bool inventorySynchronized_ = false;
    bool reachabilityDirty_ = true;
    bool miniMapDirty_ = true;
    void* pendingGhostMap_ = nullptr;
    void* activeMiniMap_ = nullptr;
    void* activeMiniMapCanvas_ = nullptr;
    TrackedWidgetHandle miniMapGhostPanel_;
    TrackedWidgetHandle miniMapIconPanel_;
    std::int32_t activeMiniMapType_ = -1;
    std::atomic<TrackerDisplayMode> displayMode_{TrackerDisplayMode::FULL};
    std::vector<TrackedWidgetHandle> spawnedMainMapGhostWidgets_;
    std::vector<TrackedWidgetHandle> activatedNativeMarkerWidgets_;
    std::vector<TrackedWidgetHandle> spawnedWallMarkerWidgets_;
    std::vector<TrackedWidgetHandle> activatedMiniMapMarkerWidgets_;
    std::vector<TrackedWidgetHandle> miniMapVisibilityWidgets_;
    std::vector<TrackedWidgetHandle> spawnedMiniMapWidgets_;
    std::unordered_set<std::uint64_t> pendingWallLocationIds_;
    std::unordered_set<std::string> pendingSyntheticTreasureIds_;
    std::unordered_set<std::uint64_t> miniMapWallLocationIds_;
    std::unordered_set<std::string> miniMapShardRooms_;
    std::unordered_set<std::string> miniMapTreasureIds_;
    std::unordered_set<std::string> reachableRooms_;
    mutable std::mutex miniMapOverlayMutex_;
    MiniMapOverlaySnapshot miniMapOverlaySnapshot_;
};
