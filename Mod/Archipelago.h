#pragma once
#include <apclient.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

enum class ItemGrantResult;

struct ArchipelagoConnectionInfo {
    std::string uri;
    std::string slotName;
    std::string password;
    bool wantsDeathlink = false;
};

enum class LocationCheckResult { NotReady, UnknownLocation, AlreadyChecked, Sent };

enum class ArchipelagoConnectionState {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    SlotConnected = 3,
    SocketDisconnectedError = 4,
    ConnectionRefusedError = 5,
    InvalidSlotError = 6
};

class Archipelago {
   public:
    static Archipelago& Instance();
    static Archipelago* ConnectedInstance();

    Archipelago();
    ~Archipelago();

    bool IsConnected() const { return state_ == ArchipelagoConnectionState::SlotConnected; }

    ArchipelagoConnectionState GetConnectionState() const { return state_; }
    std::string GetStateAsString();
    std::string GetSlotName() const { return slotName_; }
    std::string GetLastError() const { return lastError_; }

    void SetLastError(const std::string error) const { lastError_ = error; };
    bool IsPendingDeathlink() const { return pendingDeathlink_; };
    void ResetDeathLink() const { pendingDeathlink_ = false; };

    // AP Actions
    bool Connect(const std::string& slotName, const std::string& password, std::string uri, const bool& wantsDeathlink);
    void Disconnect();
    void Shutdown();
    void Poll();
    void Sync();
    void ResetLocalLocationCache();
    void ApplySavedEnemyDropShuffle();
    std::optional<ArchipelagoConnectionInfo> LoadSavedConnectionInfo() const;

    LocationCheckResult SendLocationChecks(const std::string& locationId);
    std::optional<bool> IsCurrentWorldLocation(const std::string& locationId) const;
    bool IsMissingLocation(const std::string& locationName, std::uint64_t expectedId) const;
    bool WasLocationClearedLocally(const std::string& locationName);
    std::unordered_map<std::string, std::uint32_t> GetTrackerInventory() const;
    std::unordered_set<std::uint64_t> GetMissingLocationIds() const;

    void ExecuteConsoleCommand(const char* command);

    void BaelDefeated();
    void InvokeDeathLink();

   private:
    void AbortPassword();
    void ConnectSlot();
    void LoadClearedLocations();
    void LoadPendingClearedLocations();
    bool LoadLocalProgress();
    void ProcessReceivedItems();
    void CompleteReceivedItem(int64_t itemIndex, int64_t itemId, const std::string& itemName,
                              const std::string& savePrefix, uint64_t generation, ItemGrantResult result);
    bool LoadEntitlementLedger();
    void PersistAwardedItemCounts() const;
    void ReconcileReceivedProgressionInventory();
    void ReconcileCompletedBossShardLocations();
    bool ValidateSlotAndSave(const json& slotData);
    bool HasCurrentSaveSchema() const;
    bool HasLegacySaveState() const;
    void RecordClearedLocation(const std::string& locationId);
    void RecordPendingClearedLocation(const std::string& locationId);
    void PromotePendingClearedLocations();
    void SaveConnectionInfo() const;
    bool ApplyConnectedEnemyDropShuffle(const std::string& seedName, std::uint32_t slotId);
    void SaveLocalInt64(const std::string& name, int64_t value) const;
    void SaveLocalString(const std::string& name, const std::string& value) const;
    size_t SendMissingClearedLocations();
    void SaveLocalValue(const std::string& name, int32_t value) const;
    void UpdateState(ArchipelagoConnectionState newState);

    // General Management Actions
    bool GivePlayerItem(const std::string& itemName, bool shouldDisplay,
                        std::function<void(ItemGrantResult)> completion = {});

    ArchipelagoConnectionState state_;
    std::string currentUri_;
    std::string slotName_;
    std::string password_;
    std::string localSavePrefix_;
    int itemsHandling_ = 0b111;  // Send all received items, including starting inventory

    mutable std::string lastError_;
    mutable bool pendingDeathlink_ = false;
    mutable bool wantsDeathlink_ = false;
    bool localProgressLoaded_ = false;
    bool processingReceivedItems_ = false;
    bool receivedItemGrantPending_ = false;
    bool receivedProgressionInventoryReconciled_ = false;
    bool clearedLocationsLoaded_ = false;
    bool pendingClearedLocationsLoaded_ = false;
    bool worldLocationSnapshotReady_ = false;
    std::atomic_flag polling_ = ATOMIC_FLAG_INIT;
    std::chrono::steady_clock::time_point receivedItemRetryAt_{};
    uint64_t receivedItemGeneration_ = 0;

    std::map<int64_t, APClient::NetworkItem> receivedItems_;
    std::unordered_map<int64_t, std::uint32_t> awardedItemCounts_;
    std::vector<std::string> clearedLocations_;
    std::vector<std::string> pendingClearedLocations_;
    json slotData_;

    int64_t shardDropInitialGrade_ = 1;

    std::list<std::string> deathReasons_ = {"Dominiques elbow was too strong for Miriam.",
                                            "Johannes failed at at making his latest potion",
                                            "The Galleon Minerva crashed into an Iceberg",
                                            "Miriam thought of the wrong place when using a waystone",
                                            "Miriam starved because Susie ate all the food",
                                            "Miriam forgot to return a book...",
                                            "Miriams invert didn't auto cancel...",
                                            "Miriam tripped and fell while backstepping",
                                            "Miriam saw a large shard of stained glass flying at her and didn't "
                                            "realize it was from a building, not a monster.",
                                            "Zangetsu missed the train..."};
};
