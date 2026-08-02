#pragma once
#include "Utils.h"

#include <UnrealContainers.hpp>

#include "SDK.hpp"
#include "Windows.h"

UC::FString FStringFromString(const std::string& string) {
    const std::wstring wide = Utf8ToWide(string);
    return UC::FString(wide.c_str());
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};

    const int sourceSize = static_cast<int>(str.size());
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), sourceSize, nullptr, 0);
    if (size <= 0) return std::wstring(str.begin(), str.end());

    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), sourceSize, wide.data(), size);
    return wide;
}

SDK::FName FNameFromString(const std::string& str) {
    std::wstring wideName = Utf8ToWide(str);
    return SDK::UKismetStringLibrary::Conv_StringToName(wideName.c_str());
}

bool isEqual(double a, double b) {
    return fabs(a - b) < std::numeric_limits<double>::epsilon() * fmax(fabs(a), fabs(b));
}
