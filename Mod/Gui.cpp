#pragma once
#include "Gui.h"

#include <PB_Chr_PlayerRoot_classes.hpp>
#include <Step_P0000_classes.hpp>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>
#include <wincodec.h>

#include "APBridge.h"
#include "Archipelago.h"
#include "GameManager.h"
#include "HookManager.h"
#include "InGameTracker.h"
#include "Logger.h"
#include "PBBronzeTreasureBox_BP_classes.hpp"
#include "ProjectBlood_classes.hpp"
#include "QualityOfLife.h"
#include "Resource.h"
#include "ThreadQueue.h"
#include "ToggleMods.h"
#include "Utils.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#pragma comment(lib, "windowscodecs.lib")

extern "C" IMAGE_DOS_HEADER __ImageBase;

long(__stdcall* Gui::originalPresent)(IDXGISwapChain*, unsigned int, unsigned int) = nullptr;
bool Gui::g_Hooked = false;

static ID3D11ShaderResourceView* s_WallMarkerTexture = nullptr;
static ID3D11ShaderResourceView* s_ShardMarkerTexture = nullptr;
static ID3D11ShaderResourceView* s_ChestMarkerTexture = nullptr;

static ID3D11ShaderResourceView* LoadEmbeddedPngTexture(ID3D11Device* device, int resourceId) {
    if (!device) return nullptr;
    const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return nullptr;
    const HGLOBAL loadedResource = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(loadedResource));
    if (!loadedResource || !bytes || byteCount == 0) return nullptr;

    IWICImagingFactory* factory = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (result == CO_E_NOTINITIALIZED) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    }
    if (FAILED(result) || !factory) return nullptr;

    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* textureView = nullptr;

    result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result = stream->InitializeFromMemory(bytes, byteCount);
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder);
    }
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) {
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                       0.0, WICBitmapPaletteTypeCustom);
    }

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    std::vector<BYTE> pixels;
    if (SUCCEEDED(result) && width > 0 && height > 0) {
        const UINT stride = width * 4;
        pixels.resize(static_cast<std::size_t>(stride) * height);
        result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
        if (SUCCEEDED(result)) {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = width;
            description.Height = height;
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_IMMUTABLE;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA imageData{pixels.data(), stride, 0};
            result = device->CreateTexture2D(&description, &imageData, &texture);
        }
        if (SUCCEEDED(result)) result = device->CreateShaderResourceView(texture, nullptr, &textureView);
    }

    if (texture) texture->Release();
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    factory->Release();
    return SUCCEEDED(result) ? textureView : nullptr;
}

static UnlimitedStrengthMod unlimitedStrengthMod;
static UnlimitedLuckMod unlimitedLuckMod;
static UnlimitedConMod unlimitedConMod;
static UnlimitedMindMod unlimitedMindMod;
static UnlimitedIntMod unlimitedIntMod;
static UnlimitedSpeedMod unlimitedSpeedMod;
static ExpModifierMod expModifierMod;

#ifdef _DEBUG
static char s_Host[256] = "localhost";
static char s_Port[256] = "38281";
static char s_SlotName[256] = "VGFreak";
static char s_Password[256] = "";
static char s_Console[256] = "Knife";
static char s_TeleportField[256] = "";
#else
static char s_Host[256] = "archipelago.gg";
static char s_Port[256] = "";
static char s_SlotName[256] = "";
static char s_Password[256] = "";
static char s_Console[256] = "";
#endif

static bool s_Connected = false;
static bool s_wantsDeathlink = false;

static void CopyConnectionField(char* destination, size_t destinationSize, const std::string& value) {
    strncpy_s(destination, destinationSize, value.c_str(), _TRUNCATE);
}

static void PopulateConnectionFields(const ArchipelagoConnectionInfo& connectionInfo) {
    std::string address = connectionInfo.uri;
    if (address.starts_with("ws://")) address.erase(0, 5);
    if (address.starts_with("wss://")) address.erase(0, 6);

    size_t portSeparator = address.rfind(':');
    std::string host = portSeparator == std::string::npos ? address : address.substr(0, portSeparator);
    std::string port = portSeparator == std::string::npos ? "" : address.substr(portSeparator + 1);
    CopyConnectionField(s_Host, IM_ARRAYSIZE(s_Host), host);
    CopyConnectionField(s_Port, IM_ARRAYSIZE(s_Port), port);
    CopyConnectionField(s_SlotName, IM_ARRAYSIZE(s_SlotName), connectionInfo.slotName);
    CopyConnectionField(s_Password, IM_ARRAYSIZE(s_Password), connectionInfo.password);
    s_wantsDeathlink = connectionInfo.wantsDeathlink;
}

static void RenderArchipelagoPanel() {
    ImGui::SeparatorText("Archipelago");

    if (!Archipelago::Instance().IsConnected()) {
        s_Connected = false;
    }

    if (!s_Connected) {
        ImGui::Text("Host    ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##Host", s_Host, IM_ARRAYSIZE(s_Host));

        ImGui::Text("Port    ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##Port", s_Port, IM_ARRAYSIZE(s_Port));

        ImGui::Text("Slot    ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##Slot Name", s_SlotName, IM_ARRAYSIZE(s_SlotName));

        ImGui::Text("Password");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##Password", s_Password, IM_ARRAYSIZE(s_Password), ImGuiInputTextFlags_Password);

        ImGui::Checkbox("DeathLink", &s_wantsDeathlink);

        if (ImGui::Button("Connect")) {
            std::string uri = std::string(s_Host) + ":" + s_Port;

            if (s_SlotName[0] != '\0') {
                Logger::Log("Tried to connect to AP");
                APBridge::Instance().EnqueueConnect(s_SlotName, s_Password, uri, s_wantsDeathlink);
            } else {
                Logger::Log("Cannot connect to Archipelago with empty slotName");
            }
        }

        ImGui::Text("Archipelago Connection State: %s", Archipelago::Instance().GetStateAsString().c_str());

        if (Archipelago::Instance().IsConnected()) {
            s_Connected = true;
        }

#ifdef _DEBUG
        ImGui::Text("Console ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##Console", s_Console, IM_ARRAYSIZE(s_Console));

        if (ImGui::Button("Execute")) {
            if (s_Console[0] != '\0') {
                Logger::Log("Executing console command");
                Archipelago::Instance().ExecuteConsoleCommand(s_Console);
            } else {
                Logger::Log("Cannot execute console command with empty command");
            }
        }
#endif

    } else {
        ImGui::Text("Connected as: %s", s_SlotName);
        ImGui::Text("Archipelago Connection State: %s", Archipelago::Instance().GetStateAsString().c_str());
        if (ImGui::Button("Disconnect")) {
            Logger::Log("Disconnecting from Archipelago");
            APBridge::Instance().EnqueueDisconnect();
        }
    }

    constexpr const char* TRACKER_MODE_NAMES[] = {"None", "Main Map", "Mini Map", "Full"};
    const auto trackerMode = InGameTracker::Instance().GetDisplayMode();
    const int trackerModeIndex = static_cast<int>(trackerMode);
    ImGui::Text("In-game tracking");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##In-game tracking", TRACKER_MODE_NAMES[trackerModeIndex])) {
        for (int modeIndex = 0; modeIndex < IM_ARRAYSIZE(TRACKER_MODE_NAMES); ++modeIndex) {
            const bool selected = trackerModeIndex == modeIndex;
            if (ImGui::Selectable(TRACKER_MODE_NAMES[modeIndex], selected)) {
                const auto selectedMode = static_cast<TrackerDisplayMode>(modeIndex);
                ThreadQueue::Instance().Enqueue(
                    [selectedMode] { InGameTracker::Instance().SetDisplayMode(selectedMode); });
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

}

static void RenderQualityOfLifePanel() {
    ImGui::SeparatorText("Quality of Life");

    const bool saveLoaded = GameManager::Instance().IsPlayerLoadedInGame();
    bool autoSellWastedShards = QualityOfLife::Instance().IsAutoSellWastedShardsEnabled();
    ImGui::BeginDisabled(!saveLoaded);
    if (ImGui::Checkbox("Auto-sell wasted shards", &autoSellWastedShards)) {
        ThreadQueue::Instance().Enqueue([autoSellWastedShards] {
            QualityOfLife::Instance().SetAutoSellWastedShardsEnabled(autoSellWastedShards);
            HookManager::ApplyCompatibilityShardMasterData();
        });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(
        "Automatically sell repeated randomized shard drops and incoming shards that would exceed grade 9.");

    if (!saveLoaded) ImGui::TextDisabled("Load a save to change this setting.");
}

static void RenderDebugInfoPanel() {
    #ifdef _DEBUG
    ImGui::SeparatorText("DebugInfo");
    if (GameManager::Instance().IsPlayerLoadedInGame()) {
        auto instance = reinterpret_cast<SDK::UPBGameInstance*>(GameManager::Instance().GameInstance());
        auto manager = reinterpret_cast<SDK::UPBRoomManager*>(instance->GetRoomManager());
        auto roomId = manager->GetCurrentRoomId().ToString();
        auto text = "Current Room Id: " + roomId;
        ImGui::Text(text.c_str());

        // SDK::APBWarpManager* wm = instance->GetWarpManager();
        auto text2 = "Treasurebox name: " + SDK::APBBronzeTreasureBox_BP_C::StaticClass()->GetName();
        ImGui::Text(text2.c_str());

        ImGui::Text("TeleportToRoomId");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(256);
        ImGui::InputText("##TeleportToRoomId", s_TeleportField, IM_ARRAYSIZE(s_TeleportField));

        // const char* items[] = {"50", "100", "500", "1000", "2000"};
        // const char* items[] = GameManager::Instance().GetBossNames();

        static int tpIndex = 0;
        const auto& names = GameManager::Instance().GetBossNames();
        std::vector<std::string> items(names.begin(), names.end());

        if (ImGui::BeginCombo("##label", items[tpIndex].c_str())) {
            for (int i = 0; i < items.size(); i++) {
                bool isSelected = (tpIndex == i);
                if (ImGui::Selectable(items[i].c_str(), isSelected)) tpIndex = i;
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (ImGui::Button("Teleport")) {
            std::string tpDestination;
            std::string tpField = s_TeleportField;
            if (tpField.empty()) {
                tpDestination = GameManager::Instance().GetBossRoomIdFromBossName(items[tpIndex]);
            } else {
                tpDestination = s_TeleportField;
            }

            auto tpDestinationName = FNameFromString(tpDestination);
            auto instance = (SDK::UPBGameInstance*)GameManager::Instance().GameInstance();
            // ThreadQueue::Instance().Enqueue([startName, instance, manager]() {
            SDK::FLinearColor blackFade = {0.0f, 0.0f, 0.0f, 1.0f};  // Black fade
            Logger::Log("Warping player");
            auto roomName = manager->GetCurrentRoomId();
            Logger::Log(roomName.ToString());
            instance->pRoomManager->Warp(tpDestinationName, true, true, roomName, blackFade);
            //});
        }

        if (ImGui::Button("Kill Player")) {
            auto player = static_cast<SDK::APB_Chr_PlayerRoot_C*>(GameManager::Instance().Player());
            ThreadQueue::Instance().Enqueue([player] { player->Kill(); });
        }
    }
    ImGui::Spacing();
    #endif
}

Gui& Gui::Instance() {
    static Gui instance;
    return instance;
}

void Gui::TryAutoConnect() {
    if (m_AutoConnectAttempted) return;
    m_AutoConnectAttempted = true;

    auto connectionInfo = Archipelago::Instance().LoadSavedConnectionInfo();
    if (!connectionInfo) {
        Logger::Log("[AP] No saved connection info for this save");
        return;
    }

    PopulateConnectionFields(*connectionInfo);
    Logger::Log("[AP] Trying the saved connection once");
    APBridge::Instance().EnqueueConnect(connectionInfo->slotName, connectionInfo->password, connectionInfo->uri,
                                        connectionInfo->wantsDeathlink);
}

// Verified 100% correct, DO NOT MODIFY
bool Gui::Init() {
    if (m_Initialized) return true;

    // Find game window
    m_GameWindow = nullptr;
    for (int retry = 0; retry < 60 && !m_GameWindow; retry++) {
        m_GameWindow = FindWindowW(L"UnrealWindow", nullptr);
        if (!m_GameWindow) Sleep(1000);
    }

    if (!m_GameWindow) {
        Logger::Log(LogLevel::Error, "Failed to find game window");
        return false;
    }

    Logger::Log("Found game window: ", (DWORD_PTR)m_GameWindow);

    m_Initialized = true;
    Logger::Log("Gui initialized");
    return true;
}

bool Gui::InitImGui(IDXGISwapChain* swapChain) {
    if (m_ImGuiInit) return true;

    if (!swapChain) return false;

    DXGI_SWAP_CHAIN_DESC swapChainDescription{};
    if (FAILED(swapChain->GetDesc(&swapChainDescription)) || !swapChainDescription.OutputWindow) {
        Logger::Log(LogLevel::File, "[AP] Failed to resolve the game window from the DX11 swap chain");
        return false;
    }

    m_GameWindow = swapChainDescription.OutputWindow;
    RECT clientRect{};
    GetClientRect(m_GameWindow, &clientRect);
    Logger::Log(LogLevel::File, "[AP] Resolved swap-chain game window:", (DWORD_PTR)m_GameWindow,
                "client size:", clientRect.right - clientRect.left, "x", clientRect.bottom - clientRect.top);

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    swapChain->GetDevice(IID_PPV_ARGS(&device));
    if (!device) return false;

    device->GetImmediateContext(&context);
    if (!context) {
        device->Release();
        return false;
    }

    m_Device = device;
    m_DeviceContext = context;

    ImGui_ImplWin32_EnableDpiAwareness();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableSetMousePos;

    // Allow for cursor darwing in ImGui
    io.MouseDrawCursor = true;
    // io.WantCaptureKeyboard = true;
    // io.WantCaptureMouse = true;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(m_GameWindow);
    ImGui_ImplDX11_Init(m_Device, m_DeviceContext);
    s_WallMarkerTexture = LoadEmbeddedPngTexture(m_Device, IDR_WALL_MARKER_PNG);
    if (!s_WallMarkerTexture) {
        Logger::Log(LogLevel::Warning, "[Tracker] Failed to create the DX11 breakable-wall marker texture");
    }
    s_ShardMarkerTexture = LoadEmbeddedPngTexture(m_Device, IDR_SHARD_MARKER_PNG);
    if (!s_ShardMarkerTexture) {
        Logger::Log(LogLevel::Warning, "[Tracker] Failed to create the DX11 shard marker texture");
    }
    s_ChestMarkerTexture = LoadEmbeddedPngTexture(m_Device, IDR_CHEST_MARKER_PNG);
    if (!s_ChestMarkerTexture) {
        Logger::Log(LogLevel::Warning, "[Tracker] Failed to create the DX11 chest marker texture");
    }

    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer) {
        m_Device->CreateRenderTargetView(pBackBuffer, nullptr, &m_RenderTargetView);
        pBackBuffer->Release();
    }

    m_ImGuiInit = true;
    Logger::Log("ImGui initialized");
    return true;
}

static bool IsFreshMiniMapOverlay(const MiniMapOverlaySnapshot& snapshot) {
    if (!snapshot.visible || snapshot.clipRight <= snapshot.clipLeft || snapshot.clipBottom <= snapshot.clipTop) {
        return false;
    }
    const std::uint64_t now = GetTickCount64();
    return now >= snapshot.updatedAtMilliseconds && now - snapshot.updatedAtMilliseconds <= 150;
}

static void RenderMiniMapOverlay(const MiniMapOverlaySnapshot& snapshot) {
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    if (!drawList) return;

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const ImVec2 clipMin{std::clamp(snapshot.clipLeft, 0.0f, displaySize.x),
                         std::clamp(snapshot.clipTop, 0.0f, displaySize.y)};
    const ImVec2 clipMax{std::clamp(snapshot.clipRight, 0.0f, displaySize.x),
                         std::clamp(snapshot.clipBottom, 0.0f, displaySize.y)};
    if (clipMax.x <= clipMin.x || clipMax.y <= clipMin.y) return;

    drawList->PushClipRect(clipMin, clipMax, true);
    for (const auto& marker : snapshot.markers) {
        if (!std::isfinite(marker.centerX) || !std::isfinite(marker.centerY) || marker.width <= 0.0f ||
            marker.height <= 0.0f) {
            continue;
        }

        const float centerX = marker.centerX;
        const float centerY = marker.centerY;
        const bool centerInside = centerX >= clipMin.x && centerX <= clipMax.x && centerY >= clipMin.y &&
                                  centerY <= clipMax.y;
        if (!centerInside) continue;

        const float width = std::clamp(marker.width, 8.0f, 96.0f);
        const float height = std::clamp(marker.height, 8.0f, 96.0f);
        const float left = centerX - width * 0.5f;
        const float top = centerY - height * 0.5f;
        const float right = centerX + width * 0.5f;
        const float bottom = centerY + height * 0.5f;
        if (marker.kind == MiniMapOverlayMarkerKind::CHEST) {
            if (s_ChestMarkerTexture) {
                drawList->AddImage(ImTextureID(reinterpret_cast<intptr_t>(s_ChestMarkerTexture)),
                                   ImVec2(left, top), ImVec2(right, bottom));
            } else {
                drawList->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom),
                                        IM_COL32(38, 217, 255, 255), 2.0f);
                drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom),
                                  IM_COL32(8, 70, 82, 255), 2.0f, 0, 1.5f);
            }
            continue;
        }
        if (marker.kind == MiniMapOverlayMarkerKind::WALL && s_WallMarkerTexture) {
            drawList->AddImage(ImTextureID(reinterpret_cast<intptr_t>(s_WallMarkerTexture)), ImVec2(left, top),
                               ImVec2(right, bottom));
            continue;
        }

        if (marker.kind == MiniMapOverlayMarkerKind::WALL) {
            // Resource-load fallback: retain the wall's nine-block silhouette instead of reverting to a chest.
            const float gap = std::max(1.0f, width * 0.04f);
            const float blockWidth = (width - gap * 2.0f) / 3.0f;
            const float blockHeight = (height - gap * 2.0f) / 3.0f;
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    const float x = left + column * (blockWidth + gap);
                    const float y = top + row * (blockHeight + gap);
                    drawList->AddRectFilled(ImVec2(x, y), ImVec2(x + blockWidth, y + blockHeight),
                                            IM_COL32(35, 235, 55, 255), 1.0f);
                    drawList->AddRect(ImVec2(x, y), ImVec2(x + blockWidth, y + blockHeight),
                                      IM_COL32(5, 55, 12, 255), 1.0f, 0, 1.0f);
                }
            }
            continue;
        }

        if (s_ShardMarkerTexture) {
            drawList->AddImage(ImTextureID(reinterpret_cast<intptr_t>(s_ShardMarkerTexture)), ImVec2(left, top),
                               ImVec2(right, bottom));
            continue;
        }

        // Resource-load fallback: preserve a distinct green shard silhouette.
        const ImVec2 topPoint{centerX, top};
        const ImVec2 rightPoint{right, centerY};
        const ImVec2 bottomPoint{centerX, bottom};
        const ImVec2 leftPoint{left, centerY};
        drawList->AddQuadFilled(topPoint, rightPoint, bottomPoint, leftPoint, IM_COL32(54, 224, 92, 255));
        drawList->AddTriangleFilled(topPoint, rightPoint, ImVec2(centerX, centerY),
                                    IM_COL32(150, 255, 175, 255));
        drawList->AddQuad(topPoint, rightPoint, bottomPoint, leftPoint, IM_COL32(8, 75, 25, 255), 2.0f);
    }
    drawList->PopClipRect();
}

void Gui::Render() {
    if (m_IsResizing) return;

    // Check for F5 key to toggle menu
    if (GetAsyncKeyState(VK_F5) & 1) {
        m_Open = !m_Open;
    }

    if (!m_ImGuiInit || !m_Device || !m_DeviceContext) {
        return;
    }

    const MiniMapOverlaySnapshot miniMapOverlay = InGameTracker::Instance().GetMiniMapOverlaySnapshot();
    const bool renderMiniMapOverlay = IsFreshMiniMapOverlay(miniMapOverlay);
    if (!m_Open && !renderMiniMapOverlay) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (renderMiniMapOverlay) RenderMiniMapOverlay(miniMapOverlay);

    if (m_Open) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(325.0f, 134.0f), ImVec2(FLT_MAX, FLT_MAX));
        bool menuOpened = ImGui::Begin("Mod Menu", &m_Open, ImGuiWindowFlags_AlwaysAutoResize);

        if (menuOpened) {
            if (ImGui::BeginTabBar("MainTabs")) {
                if (ImGui::BeginTabItem("Archipelago")) {
                    RenderArchipelagoPanel();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Quality of Life")) {
                    RenderQualityOfLifePanel();
                    ImGui::EndTabItem();
                }
#ifdef _DEBUG
                if (ImGui::BeginTabItem("Mods")) {
                // Test toggle button
                if (GameManager::Instance().IsPlayerLoadedInGame()) {
                    unlimitedStrengthMod.Init("Unlimited Strength");
                    unlimitedLuckMod.Init("Unlimited Luck");
                    unlimitedConMod.Init("Unlimited Constition");
                    unlimitedIntMod.Init("Unlimited Intelligence");
                    unlimitedMindMod.Init("Unlimited Mind");
                    unlimitedSpeedMod.Init("Unlimited Speed");
                    expModifierMod.Init("Exp Modifier");

                    const char* items[] = {"50", "100", "500", "1000", "2000"};
                    static int selectedIndex = 0;
                    if (ImGui::BeginCombo("##label", items[selectedIndex])) {
                        for (int i = 0; i < IM_ARRAYSIZE(items); i++) {
                            bool isSelected = (selectedIndex == i);
                            if (ImGui::Selectable(items[i], isSelected)) selectedIndex = i;
                            if (isSelected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Give Coin")) {
                        GameManager::Instance().GivePlayerCoin(std::stoi(items[selectedIndex]));
                    }
                }
                ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("DebugInfo")) {
                    RenderDebugInfoPanel();
                    ImGui::EndTabItem();
                }
#endif
            }
            ImGui::EndTabBar();
        }

        ImGui::End();
    }

    ImGui::EndFrame();

    ImGui::Render();

    if (m_RenderTargetView) {
        m_DeviceContext->OMSetRenderTargets(1, &m_RenderTargetView, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void Gui::Shutdown() {
    if (s_WallMarkerTexture) {
        s_WallMarkerTexture->Release();
        s_WallMarkerTexture = nullptr;
    }
    if (s_ShardMarkerTexture) {
        s_ShardMarkerTexture->Release();
        s_ShardMarkerTexture = nullptr;
    }
    if (s_ChestMarkerTexture) {
        s_ChestMarkerTexture->Release();
        s_ChestMarkerTexture = nullptr;
    }
    if (m_ImGuiInit) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }

    if (m_RenderTargetView) {
        m_RenderTargetView->Release();
        m_RenderTargetView = nullptr;
    }

    if (m_DeviceContext) {
        m_DeviceContext->Release();
        m_DeviceContext = nullptr;
    }

    if (m_Device) {
        m_Device->Release();
        m_Device = nullptr;
    }

    if (m_OriginalWndProc && m_GameWindow) {
        SetWindowLongPtrW(m_GameWindow, GWLP_WNDPROC, (LONG_PTR)m_OriginalWndProc);
    }

    m_Initialized = false;
    m_ImGuiInit = false;
}
