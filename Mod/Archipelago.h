#pragma once
#include <apclient.hpp>
#include <cstdint>
#include <fstream>
#include <list>
#include <map>
#include <optional>
#include <string>

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
    void Poll();
    void Sync();
    std::optional<ArchipelagoConnectionInfo> LoadSavedConnectionInfo() const;

    LocationCheckResult SendLocationChecks(const std::string& locationId);
    ItemLookupResult GetItemLookupResult(const std::string& itemName) const;

    void ExecuteConsoleCommand(const char* command);

    void BaelDefeated();
    void InvokeDeathLink();

   private:
    void AbortPassword();
    void ConnectSlot();
    void LoadLocalProgress();
    void ProcessReceivedItems();
    void SaveConnectionInfo() const;
    void SaveLocalValue(const std::string& name, int32_t value) const;
    void UpdateState(ArchipelagoConnectionState newState);

    // General Management Actions
    void GivePlayerItem(std::string& itemName, bool shouldDisplay = true);

    ArchipelagoConnectionState state_;
    std::string currentUri_;
    std::string slotName_;
    std::string password_;
    std::string localSavePrefix_;
    int itemsHandling_ = 0b111;  // Send all received items, including starting inventory

    mutable std::string lastError_;
    mutable int64_t lastReceivedItemIndex_ = -1;
    int64_t lastQueuedItemIndex_ = -1;
    mutable bool pendingDeathlink_ = false;
    mutable bool wantsDeathlink_ = false;
    bool localProgressLoaded_ = false;

    std::map<int64_t, APClient::NetworkItem> pendingReceivedItems_;
    json slotData_;

    int64_t shardDropInitialGrade_;

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
