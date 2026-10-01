#pragma once

#include <string>

bool isAutostartEnabled(std::wstring& error);
bool setAutostartEnabled(bool enabled, std::wstring& error);

#ifdef BIG_HEAD_VPN_TESTING
bool createStartupShortcutForTest(const std::wstring& shortcut, std::wstring& error);
#endif

bool runAutostartTask(std::wstring& error);

bool removeOwnedAutostart(std::wstring& error);
