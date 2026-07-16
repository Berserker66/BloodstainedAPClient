#pragma once
#include "Gui.h"

#include <PB_Chr_PlayerRoot_classes.hpp>
#include <Step_P0000_classes.hpp>
#include <iterator>

#include "APBridge.h"
#include "Archipelago.h"
#include "GameManager.h"
#include "Logger.h"
#include "PBBronzeTreasureBox_BP_classes.hpp"
#include "ProjectBlood_classes.hpp"
#include "ThreadQueue.h"
#include "ToggleMods.h"
#include "Utils.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

long(__stdcall* Gui::originalPresent)(IDXGISwapChain*, unsigned int, unsigned int) = nullptr;
bool Gui::g_Hooked = false;

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

    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer) {
        m_Device->CreateRenderTargetView(pBackBuffer, nullptr, &m_RenderTargetView);
        pBackBuffer->Release();
    }

    m_ImGuiInit = true;
    Logger::Log("ImGui initialized");
    return true;
}

void Gui::Render() {
    if (m_IsResizing) return;

    // Check for F5 key to toggle menu
    if (GetAsyncKeyState(VK_F5) & 1) {
        m_Open = !m_Open;
    }

    if (!m_Open) {
        return;
    }

    if (!m_ImGuiInit || !m_Device || !m_DeviceContext) {
        return;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    bool menuOpened = ImGui::Begin("Mod Menu", &m_Open, ImGuiWindowFlags_AlwaysAutoResize);

    if (menuOpened) {
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("Archipelago")) {
                RenderArchipelagoPanel();
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

    ImGui::EndFrame();

    ImGui::Render();

    if (m_RenderTargetView) {
        m_DeviceContext->OMSetRenderTargets(1, &m_RenderTargetView, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
}

void Gui::Shutdown() {
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
