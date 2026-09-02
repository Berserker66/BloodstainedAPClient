#include "APBridge.h"

#include <Windows.h>

#include <deque>
#include <mutex>

#include "Archipelago.h"

APBridge& APBridge::Instance() {
    static APBridge instance;
    return instance;
}

APBridge::APBridge() {}

void APBridge::EnqueueConnect(const std::string& slotName, const std::string& password, const std::string& uri,
                              DeathLinkMode deathLinkMode) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(Command{Command::Connect, slotName, password, uri, deathLinkMode});
}

void APBridge::EnqueueDisconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(Command{Command::Disconnect, "", "", ""});
}

void APBridge::EnqueueSync() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(Command{Command::Sync, "", "", ""});
}

void APBridge::ProcessPending() {
    std::deque<Command> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(queue_);
    }
    // Dispatch outside the lock
    for (const auto& cmd : local) {
        switch (cmd.type) {
            case Command::Connect:
                Archipelago::Instance().Connect(cmd.slotName, cmd.password, cmd.uri, cmd.deathLinkMode);
                break;
            case Command::Disconnect:
                Archipelago::Instance().Disconnect();
                break;
            case Command::Sync:
                Archipelago::Instance().Sync();
                break;
            case Command::Poll:
                Archipelago::Instance().Poll();
                break;
        }
    }
}
