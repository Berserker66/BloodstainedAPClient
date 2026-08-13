#include "MainMenuStatus.h"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <openssl/evp.h>
#include <sstream>
#include <string>

#include "ClientVersion.h"
#include "Logger.h"
#include "ProjectBlood_classes.hpp"
#include "UMG_classes.hpp"
#include "Utils.h"
#include "VersionNumber_classes.hpp"

namespace {

constexpr unsigned long long STATUS_REFRESH_INTERVAL_MS = 1000;
constexpr std::string_view STATIC_PAK_SHA256 =
    "ff01e4bcacb38920e7ac0f0f8d83aa5b65378a46f8c8751cd13c85c3cab993e9";

bool IsOwnedBy(const SDK::UObject* object, const SDK::UObject* owner) {
    for (const SDK::UObject* outer = object ? object->Outer : nullptr; outer; outer = outer->Outer) {
        if (outer == owner) return true;
    }
    return false;
}

bool IsLiveObject(const SDK::UObject* object, SDK::int32 expectedIndex) {
    if (!object || expectedIndex < 0 || !SDK::UObject::GObjects ||
        SDK::UObject::GObjects->GetByIndex(expectedIndex) != object || !object->Class) {
        return false;
    }
    constexpr SDK::int32 DESTROYED_FLAGS =
        static_cast<SDK::int32>(SDK::EObjectFlags::BeginDestroyed) |
        static_cast<SDK::int32>(SDK::EObjectFlags::FinishDestroyed);
    return (static_cast<SDK::int32>(object->Flags) & DESTROYED_FLAGS) == 0;
}

std::filesystem::path GetGameBinaryDirectory() {
    std::wstring executablePath(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    executablePath.resize(length);
    return std::filesystem::path(executablePath).parent_path();
}

std::filesystem::path GetModPakDirectory() {
    return GetGameBinaryDirectory().parent_path().parent_path() / L"Content" / L"Paks" / L"~mods";
}

std::string Sha256File(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return {};
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    std::array<char, 1024 * 1024> buffer{};
    while (ok && input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) ok = EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) == 1;
    }
    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digestLength = 0;
    ok = ok && input.eof() && EVP_DigestFinal_ex(context, digest, &digestLength) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return {};

    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digestLength; ++index) {
        result << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return result.str();
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

    if (!IsLiveObject(widget, widgetIndex)) {
        Forget();
        if (!CreateWidget(worldContext)) return;
    }

    const unsigned long long now = GetTickCount64();
    if (now - lastRefresh >= STATUS_REFRESH_INTERVAL_MS) {
        RefreshText();
        lastRefresh = now;
    }
}

void MainMenuStatus::Hide() {
    if (IsLiveObject(widget, widgetIndex)) widget->RemoveFromParent();
    Forget();
}

void MainMenuStatus::Forget() {
    widget = nullptr;
    textBlock = nullptr;
    widgetIndex = -1;
    textBlockIndex = -1;
    lastRefresh = 0;
    pakStatus = PakStatus::Unknown;
}

bool MainMenuStatus::IsStaticPakReady() const { return DetectPakStatus() == PakStatus::Ready; }

bool MainMenuStatus::CreateWidget(SDK::UObject* worldContext) {
    auto* owningWidget = static_cast<SDK::UUserWidget*>(worldContext);
    auto* createdWidget = SDK::UWidgetBlueprintLibrary::Create(
        worldContext, SDK::TSubclassOf<SDK::UUserWidget>(SDK::UVersionNumber_C::StaticClass()),
        owningWidget->GetOwningPlayer());
    widget = static_cast<SDK::UVersionNumber_C*>(createdWidget);
    if (!widget) return false;
    widgetIndex = widget->Index;

    widget->AddToViewport(1000);

    for (SDK::int32 index = 0; index < SDK::UObject::GObjects->Num(); index++) {
        auto* object = SDK::UObject::GObjects->GetByIndex(index);
        if (!object || !object->IsA(SDK::UTextBlock::StaticClass()) || !IsOwnedBy(object, widget)) continue;
        textBlock = static_cast<SDK::UTextBlock*>(object);
        textBlockIndex = textBlock->Index;
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
    if (!IsLiveObject(widget, widgetIndex) || !IsLiveObject(textBlock, textBlockIndex) ||
        !IsOwnedBy(textBlock, widget)) {
        Forget();
        return;
    }

    if (pakStatus == PakStatus::Unknown) pakStatus = DetectPakStatus();
    const bool ue4ssGuiEnabled = IsUE4SSGuiEnabled();

    std::string status = "Archipelago Client v";
    status += ClientVersion::Display;

    if (pakStatus == PakStatus::Missing) {
        status += "\r\nERROR: BloodstainedAP.pak is missing";
    } else if (pakStatus == PakStatus::Invalid) {
        status += "\r\nERROR: BloodstainedAP.pak is not the 1.1.0 release pak";
    } else if (pakStatus == PakStatus::LegacyConflict) {
        status += "\r\nERROR: Remove the legacy Randomizer.pak";
    } else if (pakStatus == PakStatus::Unknown) {
        status += "\r\nChecking BloodstainedAP.pak...";
    }

    if (ue4ssGuiEnabled) status += "\r\nWARNING: UE4SS GUI is enabled and may crash the game";
    if (pakStatus == PakStatus::Ready && !ue4ssGuiEnabled) status += "\r\nSetup: OK";

    textBlock->SetText(SDK::UKismetTextLibrary::Conv_StringToText(FStringFromString(status)));

    // SetText dispatches through ProcessEvent and can re-enter the title UI while it is being torn down. The
    // widget may therefore be destroyed before the following style update even though it was live on entry.
    if (!IsLiveObject(widget, widgetIndex) || !IsLiveObject(textBlock, textBlockIndex) ||
        !IsOwnedBy(textBlock, widget)) {
        Forget();
        return;
    }

    if (pakStatus == PakStatus::Missing || pakStatus == PakStatus::Invalid ||
        pakStatus == PakStatus::LegacyConflict) {
        textBlock->SetColorAndOpacity(MakeSlateColor(1.0f, 0.25f, 0.2f));
    } else if (ue4ssGuiEnabled || pakStatus == PakStatus::Unknown) {
        textBlock->SetColorAndOpacity(MakeSlateColor(1.0f, 0.78f, 0.2f));
    } else {
        textBlock->SetColorAndOpacity(MakeSlateColor(0.65f, 1.0f, 0.72f));
    }
}

MainMenuStatus::PakStatus MainMenuStatus::DetectPakStatus() const {
    const auto pakDirectory = GetModPakDirectory();
    if (std::filesystem::exists(pakDirectory / L"Randomizer.pak")) return PakStatus::LegacyConflict;
    const auto staticPak = pakDirectory / L"BloodstainedAP.pak";
    if (!std::filesystem::exists(staticPak)) return PakStatus::Missing;
    const std::string hash = Sha256File(staticPak);
    if (hash.empty()) return PakStatus::Unknown;
    return hash == STATIC_PAK_SHA256 ? PakStatus::Ready : PakStatus::Invalid;
}

bool MainMenuStatus::IsUE4SSGuiEnabled() const {
    if (!GetModuleHandleW(L"UE4SS.dll")) return false;

    const std::filesystem::path settingsPath = GetGameBinaryDirectory() / L"UE4SS-settings.ini";
    const int guiEnabled = GetPrivateProfileIntW(L"Debug", L"GuiConsoleEnabled", 1, settingsPath.c_str());
    const int guiVisible = GetPrivateProfileIntW(L"Debug", L"GuiConsoleVisible", 1, settingsPath.c_str());
    return guiEnabled != 0 || guiVisible != 0;
}
