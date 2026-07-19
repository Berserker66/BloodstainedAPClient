#include "MainMenuStatus.h"

#include <Windows.h>

#include <filesystem>
#include <string>

#include "ClientVersion.h"
#include "Logger.h"
#include "ProjectBlood_classes.hpp"
#include "UMG_classes.hpp"
#include "Utils.h"
#include "VersionNumber_classes.hpp"

namespace {

constexpr unsigned long long STATUS_REFRESH_INTERVAL_MS = 1000;

bool IsOwnedBy(const SDK::UObject* object, const SDK::UObject* owner) {
    for (const SDK::UObject* outer = object ? object->Outer : nullptr; outer; outer = outer->Outer) {
        if (outer == owner) return true;
    }
    return false;
}

std::filesystem::path GetGameBinaryDirectory() {
    std::wstring executablePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    executablePath.resize(length);
    return std::filesystem::path(executablePath).parent_path();
}

SDK::FSlateColor MakeSlateColor(float red, float green, float blue) {
    SDK::FSlateColor color{};
    color.SpecifiedColor = {red, green, blue, 1.0f};
    color.ColorUseRule = SDK::ESlateColorStylingMode::UseColor_Specified;
    return color;
}

}  // namespace

MainMenuStatus& MainMenuStatus::Instance() {
    static MainMenuStatus instance;
    return instance;
}

void MainMenuStatus::Show(SDK::UObject* worldContext) {
    if (!worldContext) return;

    if (!widget || !SDK::UKismetSystemLibrary::IsValid(widget)) {
        widget = nullptr;
        textBlock = nullptr;
        if (!CreateWidget(worldContext)) return;
    }

    const unsigned long long now = GetTickCount64();
    if (now - lastRefresh >= STATUS_REFRESH_INTERVAL_MS) {
        RefreshText();
        lastRefresh = now;
    }
}

void MainMenuStatus::Hide() {
    if (widget && SDK::UKismetSystemLibrary::IsValid(widget)) widget->RemoveFromParent();
    widget = nullptr;
    textBlock = nullptr;
    lastRefresh = 0;
    pakStatus = PakStatus::Unknown;
}

bool MainMenuStatus::CreateWidget(SDK::UObject* worldContext) {
    auto* owningWidget = static_cast<SDK::UUserWidget*>(worldContext);
    auto* createdWidget = SDK::UWidgetBlueprintLibrary::Create(
        worldContext, SDK::TSubclassOf<SDK::UUserWidget>(SDK::UVersionNumber_C::StaticClass()),
        owningWidget->GetOwningPlayer());
    widget = static_cast<SDK::UVersionNumber_C*>(createdWidget);
    if (!widget) return false;

    widget->AddToViewport(1000);

    for (SDK::int32 index = 0; index < SDK::UObject::GObjects->Num(); index++) {
        auto* object = SDK::UObject::GObjects->GetByIndex(index);
        if (!object || !object->IsA(SDK::UTextBlock::StaticClass()) || !IsOwnedBy(object, widget)) continue;
        textBlock = static_cast<SDK::UTextBlock*>(object);
        break;
    }

    if (!textBlock) {
        Logger::Log(LogLevel::File, "[AP] Main-menu status widget has no text block");
        Hide();
        return false;
    }

    auto* canvasSlot = SDK::UWidgetLayoutLibrary::SlotAsCanvasSlot(textBlock);
    if (canvasSlot) {
        SDK::FAnchors topRight{};
        topRight.Minimum = {1.0f, 0.0f};
        topRight.Maximum = {1.0f, 0.0f};
        canvasSlot->SetAnchors(topRight);
        canvasSlot->SetAlignment({1.0f, 0.0f});
        canvasSlot->SetPosition({-19.0f, 19.0f});
        canvasSlot->SetAutoSize(true);
        canvasSlot->SetZOrder(1000);
    }

    textBlock->SetRenderTranslation({0.0f, 0.0f});
    textBlock->SetJustification(SDK::ETextJustify::Right);
    auto font = textBlock->Font;
    font.Size = 16;
    textBlock->SetFont(font);
    textBlock->SetShadowOffset({1.0f, 1.0f});
    textBlock->SetShadowColorAndOpacity({0.0f, 0.0f, 0.0f, 0.9f});

    Logger::Log(LogLevel::File, "[AP] Created native main-menu status widget");
    RefreshText();
    lastRefresh = GetTickCount64();
    return true;
}

void MainMenuStatus::RefreshText() {
    if (!textBlock || !SDK::UKismetSystemLibrary::IsValid(textBlock)) return;

    if (pakStatus == PakStatus::Unknown) pakStatus = DetectPakStatus();
    const bool ue4ssGuiEnabled = IsUE4SSGuiEnabled();

    std::string status = "Archipelago Client v";
    status += ClientVersion::Display;

    if (pakStatus == PakStatus::Unknown) {
        status += "\r\nChecking Randomizer.pak...";
    } else if (pakStatus == PakStatus::ArchipelagoDisabled) {
        status += "\r\nERROR: Randomizer.pak is not Archipelago-enabled";
    }

    if (ue4ssGuiEnabled) status += "\r\nWARNING: UE4SS GUI is enabled and may crash the game";
    if (pakStatus == PakStatus::ArchipelagoEnabled && !ue4ssGuiEnabled) status += "\r\nSetup: OK";

    textBlock->SetText(SDK::UKismetTextLibrary::Conv_StringToText(FStringFromString(status)));

    if (pakStatus == PakStatus::ArchipelagoDisabled) {
        textBlock->SetColorAndOpacity(MakeSlateColor(1.0f, 0.25f, 0.2f));
    } else if (ue4ssGuiEnabled || pakStatus == PakStatus::Unknown) {
        textBlock->SetColorAndOpacity(MakeSlateColor(1.0f, 0.78f, 0.2f));
    } else {
        textBlock->SetColorAndOpacity(MakeSlateColor(0.65f, 1.0f, 0.72f));
    }
}

MainMenuStatus::PakStatus MainMenuStatus::DetectPakStatus() const {
    auto* shardTable = SDK::UPBDataTableManager::GetLoadedDataTable(SDK::EPBDataTables::ShardMaster);
    if (!shardTable || shardTable->RowMap.Num() == 0) return PakStatus::Unknown;

    for (const auto& row : shardTable->RowMap) {
        if (row.Key().ToString().starts_with("AP_")) return PakStatus::ArchipelagoEnabled;
    }
    return PakStatus::ArchipelagoDisabled;
}

bool MainMenuStatus::IsUE4SSGuiEnabled() const {
    if (!GetModuleHandleW(L"UE4SS.dll")) return false;

    const std::filesystem::path settingsPath = GetGameBinaryDirectory() / L"UE4SS-settings.ini";
    const int guiEnabled = GetPrivateProfileIntW(L"Debug", L"GuiConsoleEnabled", 1, settingsPath.c_str());
    const int guiVisible = GetPrivateProfileIntW(L"Debug", L"GuiConsoleVisible", 1, settingsPath.c_str());
    return guiEnabled != 0 || guiVisible != 0;
}
