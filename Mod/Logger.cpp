#pragma once
#include "Logger.h"

#include <Windows.h>

#include <filesystem>
#include <iomanip>
#include <mutex>

namespace {
std::filesystem::path logPath;
std::mutex logMutex;
}

void Logger::Init() {
#ifdef _DEBUG
    AllocConsole();
    FILE* fDummy;
    freopen_s(&fDummy, "CONIN$", "r", stdin);
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    freopen_s(&fDummy, "CONOUT$", "w", stderr);
    std::cout << "[Logger] Console initialized" << std::endl;
#endif

    wchar_t localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    logPath = std::filesystem::path(localAppData) / L"BloodstainedRotN" / L"Saved" / L"Logs" /
              L"BloodstainedAP.log";
    std::filesystem::create_directories(logPath.parent_path());
    {
        std::lock_guard<std::mutex> lock(logMutex);
        std::ofstream file(logPath, std::ios_base::trunc);
    }
    WriteFile("--- BloodstainedAP session started ---");
}

void Logger::WriteFile(const std::string& message) {
    if (logPath.empty()) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream file(logPath, std::ios_base::app);
    file << '[' << std::setfill('0') << std::setw(4) << time.wYear << '-' << std::setw(2) << time.wMonth << '-'
         << std::setw(2) << time.wDay << ' ' << std::setw(2) << time.wHour << ':' << std::setw(2) << time.wMinute << ':'
         << std::setw(2) << time.wSecond << '.' << std::setw(3) << time.wMilliseconds << "] " << message << '\n';
}
