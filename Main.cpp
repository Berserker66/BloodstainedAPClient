#include <Windows.h>
#include <minwindef.h>
#include <winuser.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <iostream>
#include <thread>

#include "Mod/APBridge.h"
#include "Mod/Archipelago.h"
#include "Mod/GameManager.h"
#include "Mod/Gui.h"
#include "Mod/HookManager.h"
#include "Mod/Logger.h"
#include "imgui.h"
#include "kiero.h"
#include "version/version.h"

long __stdcall HookedPresent(IDXGISwapChain* swapChain, unsigned int syncInterval, unsigned int flags);

namespace {
constexpr UINT_PTR ARCHIPELAGO_POLL_TIMER_ID = 0x4150;
constexpr UINT ARCHIPELAGO_POLL_INTERVAL_MS = 100;
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT __stdcall HookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == ARCHIPELAGO_POLL_TIMER_ID) {
        if (!HookManager::shuttingDown && GameManager::Instance().IsInitialized()) {
            APBridge::Instance().ProcessPending();
            Archipelago::Instance().Poll();
        }
        return 0;
    }

    if (msg == WM_CLOSE || msg == WM_DESTROY) {
        HookManager::shuttingDown = true;
        KillTimer(hwnd, ARCHIPELAGO_POLL_TIMER_ID);
        Archipelago::Instance().Shutdown();
    }

    // Track resize state
    if (msg == WM_ENTERSIZEMOVE) {
        Gui::Instance().SetResizing(true);
    } else if (msg == WM_EXITSIZEMOVE) {
        Gui::Instance().SetResizing(false);
    }

    // Let resize/move messages pass through to original handler
    if (msg == WM_SIZE || msg == WM_MOVE || msg == WM_WINDOWPOSCHANGED || msg == WM_SIZING || msg == WM_ENTERSIZEMOVE ||
        msg == WM_EXITSIZEMOVE) {
        return CallWindowProc((WNDPROC)Gui::Instance().GetOriginalWndProc(), hwnd, msg, wParam, lParam);
    }

    if (Gui::Instance().IsOpen() && Gui::Instance().IsImGuiInit()) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);

        const bool isKeyboardMessage = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN ||
                                       msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_UNICHAR;
        const bool isMouseMessage = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        const ImGuiIO& io = ImGui::GetIO();
        if ((isKeyboardMessage && (io.WantCaptureKeyboard || io.WantTextInput)) ||
            (isMouseMessage && io.WantCaptureMouse)) {
            return 0;
        }
    }
    return CallWindowProc((WNDPROC)Gui::Instance().GetOriginalWndProc(), hwnd, msg, wParam, lParam);
}

// Salvaged Code from Livestream
BOOL APIENTRY InitKieroAndHook() {
    HWND hwnd = Gui::Instance().GetGameWindow();
    if (hwnd) {
        WNDPROC originalWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)HookWndProc);
        Gui::Instance().SetOriginalWndProc(originalWndProc);
        if (!SetTimer(hwnd, ARCHIPELAGO_POLL_TIMER_ID, ARCHIPELAGO_POLL_INTERVAL_MS, nullptr)) {
            Logger::Log(LogLevel::File, "[AP] Failed to install pause-safe network timer; error:", GetLastError());
        } else {
            Logger::Log(LogLevel::File, "[AP] Installed pause-safe network timer; interval ms:",
                        ARCHIPELAGO_POLL_INTERVAL_MS);
        }
        Logger::Log("WndProc Hooked");
    }

    kiero::init(kiero::RenderType::D3D11);

    if (kiero::bind(8, (void**)&Gui::originalPresent, (void*)HookedPresent) == kiero::Status::Success) {
        Logger::Log("Present hook installed");
        Gui::g_Hooked = true;
        return true;
    } else {
        Logger::Log("ERROR: Failed to hook Present");
        return false;
    }
}

void renderGui() {
    if (!Gui::Instance().IsImGuiInit()) return;

    Gui::Instance().Render();
}

long __stdcall HookedPresent(IDXGISwapChain* swapChain, unsigned int syncInterval, unsigned int flags) {
    static bool init = false;

    if (!init) {
        init = true;
        Gui::Instance().InitImGui(swapChain);
        Logger::Log("Present hooked - GUI initialized");
    }

    renderGui();

    return Gui::originalPresent(swapChain, syncInterval, flags);
}

DWORD APIENTRY MainThread(HMODULE Module) {
    char dllName[MAX_PATH];
    GetModuleFileNameA(Module, dllName, MAX_PATH);

    Logger::Init();
    Logger::Log("Starting Bloodstained Modding SDK");

    while (!GameManager::Instance().Init()) Sleep(500);

    while (!HookManager::Instance().Init()) Sleep(500);

    Sleep(5000);
    Gui::Instance().Init();
    if (!InitKieroAndHook()) return 0;

    while (!GameManager::Instance().PostInit()) Sleep(100);

    while (!HookManager::Instance().PostInit()) Sleep(100);

    Logger::Log("Ready to Game!");

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    std::thread* second;
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            setupWrappers();
            second = new std::thread(MainThread, hModule);
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
