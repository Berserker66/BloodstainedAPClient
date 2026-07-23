#pragma once

#include <atomic>

namespace SDK {
class UObject;
class UFunction;
}  // namespace SDK

class QualityOfLife {
   public:
    static QualityOfLife& Instance();

    bool IsAutoSellWastedShardsEnabled() const { return autoSellWastedShards_.load(); }
    void LoadSettings();
    void SetAutoSellWastedShardsEnabled(bool enabled);

    void ProcessEventBefore(SDK::UObject* obj, SDK::UFunction* func, void* params);

   private:
    QualityOfLife() = default;
    QualityOfLife(const QualityOfLife&) = delete;
    QualityOfLife& operator=(const QualityOfLife&) = delete;

    std::atomic_bool autoSellWastedShards_{false};
};
