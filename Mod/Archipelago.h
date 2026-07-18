#pragma once
#include <apclient.hpp>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using json = nlohmann::json;

struct ArchipelagoConnectionInfo {
    std::string uri;
    std::string slotName;
    std::string password;
    bool wantsDeathlink = false;
};

enum class LocationCheckResult { NotReady, UnknownLocation, AlreadyChecked, Sent };

enum class ItemLookupResult { NotReady, UnknownItem, KnownItem };

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
    std::optional<ArchipelagoConnectionInfo> LoadSavedConnectionInfo() const;

    LocationCheckResult SendLocationChecks(const std::string& locationId);
    ItemLookupResult GetItemLookupResult(const std::string& itemName) const;
    bool IsMissingLocation(const std::string& locationName, std::uint64_t expectedId) const;
    std::unordered_map<std::string, std::uint32_t> GetTrackerInventory() const;
    std::unordered_set<std::uint64_t> GetMissingLocationIds() const;

    void ExecuteConsoleCommand(const char* command);

    void BaelDefeated();
    void InvokeDeathLink();

   private:
    void AbortPassword();
    void ConnectSlot();
    void LoadClearedLocations();
    void LoadLocalProgress();
    void ProcessReceivedItems();
    void ReconcileReceivedShards();
    void TryMigrateLegacyProgress();
    void RecordClearedLocation(const std::string& locationId);
    void SaveConnectionInfo() const;
    void SaveLocalString(const std::string& name, const std::string& value) const;
    size_t SendMissingClearedLocations();
    void SaveLocalValue(const std::string& name, int32_t value) const;
    void UpdateState(ArchipelagoConnectionState newState);

    // General Management Actions
    bool GivePlayerItem(std::string& itemName, bool shouldDisplay = true);

    ArchipelagoConnectionState state_;
    std::string currentUri_;
    std::string slotName_;
    std::string password_;
    std::string localSavePrefix_;
    int itemsHandling_ = 0b111;  // Send all received items, including starting inventory

    mutable std::string lastError_;
    mutable int64_t lastReceivedItemIndex_ = -1;
    int64_t lastQueuedItemIndex_ = -1;
    std::optional<int32_t> legacyReceivedItemIndex_;
    mutable bool pendingDeathlink_ = false;
    mutable bool wantsDeathlink_ = false;
    bool localProgressLoaded_ = false;
    bool processingReceivedItems_ = false;
    bool receivedShardsReconciled_ = false;
    bool clearedLocationsLoaded_ = false;
    std::atomic_flag polling_ = ATOMIC_FLAG_INIT;

    std::map<int64_t, APClient::NetworkItem> pendingReceivedItems_;
    std::map<int64_t, APClient::NetworkItem> receivedItems_;
    std::vector<std::string> clearedLocations_;
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
