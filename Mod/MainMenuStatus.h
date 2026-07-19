#pragma once

#include "CoreUObject_classes.hpp"

namespace SDK {
class UTextBlock;
class UVersionNumber_C;
}

class MainMenuStatus {
   public:
    static MainMenuStatus& Instance();

    void Show(SDK::UObject* worldContext);
    void Hide();

   private:
    enum class PakStatus {
        Unknown,
        ArchipelagoEnabled,
        ArchipelagoDisabled,
    };

    MainMenuStatus() = default;
    MainMenuStatus(const MainMenuStatus&) = delete;
    MainMenuStatus& operator=(const MainMenuStatus&) = delete;

    bool CreateWidget(SDK::UObject* worldContext);
    void RefreshText();
    PakStatus DetectPakStatus() const;
    bool IsUE4SSGuiEnabled() const;

    SDK::UVersionNumber_C* widget = nullptr;
    SDK::UTextBlock* textBlock = nullptr;
    unsigned long long lastRefresh = 0;
    PakStatus pakStatus = PakStatus::Unknown;
};
