#pragma once

#include <string>

bool isAutostartEnabled(std::wstring& error);
bool setAutostartEnabled(bool enabled, std::wstring& error);
