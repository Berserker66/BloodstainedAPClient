#pragma once
#include <string.h>

#include "SDK.hpp"
#include "UnrealContainers.hpp"

UC::FString FStringFromString(const std::string& string);
std::wstring Utf8ToWide(const std::string& str);
SDK::FName FNameFromString(const std::string& str);
bool isEqual(double a, double b);
